#!/usr/bin/env python3

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time


PACKAGE = "org.arm.lavatube.replay"
COMPONENT = PACKAGE + "/android.app.NativeActivity"
DEVICE_FILES = "/sdcard/Android/data/" + PACKAGE + "/files"
RESULT_FILE = DEVICE_FILES + "/lava-replay-result.json"
SERVICE_LOG = DEVICE_FILES + "/lava-replay-service.log"
SUPPORTED_ABIS = ("x86_64", "arm64-v8a")


def run_command(command, check=True, capture=False, environment=None):
	return subprocess.run(
		command,
		check=check,
		text=True,
		env=environment,
		stdout=subprocess.PIPE if capture else None,
		stderr=subprocess.PIPE if capture else None,
	)


def android_environment():
	environment = os.environ.copy()
	if environment.get("ANDROID_HOME") or environment.get("ANDROID_SDK_ROOT"):
		return environment
	candidates = [Path.home() / "Android" / "Sdk", Path("/usr/lib/android-sdk")]
	for candidate in candidates:
		if candidate.is_dir():
			environment["ANDROID_HOME"] = str(candidate)
			environment["ANDROID_SDK_ROOT"] = str(candidate)
			return environment
	raise RuntimeError("Android SDK not found; set ANDROID_HOME or ANDROID_SDK_ROOT")


def adb_devices(adb):
	result = run_command([adb, "devices"], capture=True)
	devices = []
	for line in result.stdout.splitlines()[1:]:
		fields = line.split()
		if len(fields) == 2 and fields[1] == "device":
			devices.append(fields[0])
	return devices


def select_serial(adb, requested):
	serial = requested or os.environ.get("ANDROID_SERIAL")
	devices = adb_devices(adb)
	if serial:
		if serial not in devices:
			raise RuntimeError("Android device " + serial + " is not available")
		return serial
	if len(devices) != 1:
		raise RuntimeError("Expected one Android device, found: " + ", ".join(devices))
	return devices[0]


def adb_command(adb, serial, *arguments, check=True, capture=False):
	return run_command([adb, "-s", serial, *arguments], check=check, capture=capture)


def adb_shell_command(adb, serial, *arguments, check=True, capture=False):
	return adb_command(adb, serial, "shell", shlex.join(arguments), check=check, capture=capture)


def detect_abi(adb, serial):
	result = adb_command(adb, serial, "shell", "getprop", "ro.product.cpu.abi", capture=True)
	abi = result.stdout.strip()
	if abi not in SUPPORTED_ABIS:
		raise RuntimeError("Unsupported Android ABI: " + abi)
	return abi


def apk_from_output_metadata(repo, build_type):
	output_directory = repo / "android" / "replay" / "build" / "outputs" / "apk" / build_type
	metadata_path = output_directory / "output-metadata.json"
	try:
		metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
	except (OSError, json.JSONDecodeError) as error:
		raise RuntimeError("Failed to read Gradle APK metadata " + str(metadata_path) + ": " + str(error)) from error

	apks = []
	for element in metadata.get("elements", []):
		output_file = element.get("outputFile")
		if not isinstance(output_file, str):
			continue
		apk = output_directory / output_file
		if apk.is_file():
			apks.append(apk)
	if len(apks) != 1:
		raise RuntimeError("Gradle metadata did not identify exactly one APK in " + str(output_directory))
	return apks[0]


def build_apk(repo, abi, build_type):
	gradle_task = ":replay:assemble" + build_type.capitalize()
	run_command([
		str(repo / "android" / "gradlew"),
		"-p", str(repo / "android"),
		gradle_task,
		"-PlavatubeAbi=" + abi,
	], environment=android_environment())
	return apk_from_output_metadata(repo, build_type)


def validate_apk_abi(apk, abi):
	contents = run_command(["unzip", "-Z1", str(apk)], capture=True)
	if "lib/" + abi + "/liblava-replay.so" not in contents.stdout.splitlines():
		raise RuntimeError("APK does not contain lava-replay for " + abi + ": " + str(apk))


def read_result(adb, serial):
	result = adb_command(adb, serial, "shell", "cat", RESULT_FILE, check=False, capture=True)
	if result.returncode != 0:
		return None
	try:
		return json.loads(result.stdout)
	except json.JSONDecodeError:
		return None


def app_is_running(adb, serial):
	result = adb_command(adb, serial, "shell", "pidof", PACKAGE, check=False, capture=True)
	return result.returncode == 0 and bool(result.stdout.strip())


def wait_for_result(adb, serial, terminal_only, timeout):
	deadline = time.monotonic() + timeout
	while time.monotonic() < deadline:
		result = read_result(adb, serial)
		if result is not None:
			if not terminal_only or result.get("status") in ("done", "failed", "stopped"):
				return result
			if result.get("status") == "running" and not app_is_running(adb, serial):
				raise RuntimeError("Android replay process exited without writing a terminal result")
		time.sleep(0.25)
	raise RuntimeError("Timed out waiting for Android replay result")


def terminal_result_exit_code(result, successful_statuses):
	try:
		exit_code = int(result.get("exit_code", 1))
	except (TypeError, ValueError):
		return 1
	if exit_code != 0:
		return exit_code
	return 0 if result.get("status") in successful_statuses else 1


def prepare_storage(adb, serial):
	adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE)
	adb_command(adb, serial, "shell", "rm", "-f", RESULT_FILE, RESULT_FILE + ".tmp", check=False)
	adb_shell_command(adb, serial, "am", "start", "-n", COMPONENT, "--es", "mode", "prepare")
	result = wait_for_result(adb, serial, True, 30)
	if result.get("status") != "done":
		raise RuntimeError("Failed to prepare Android replay storage: " + str(result))
	adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE)
	adb_command(adb, serial, "shell", "mkdir", "-p", DEVICE_FILES)


def clean_run_outputs(adb, serial):
	listing = adb_command(adb, serial, "shell", "ls", "-1", DEVICE_FILES, check=False, capture=True)
	if listing.returncode != 0:
		return
	for name in listing.stdout.splitlines():
		name = name.strip()
		if "/" in name or name in (".", ".."):
			continue
		if name.endswith(".png") or name in ("lava-replay-result.json", "lava-replay-result.json.tmp", "lava-replay-service.log"):
			adb_command(adb, serial, "shell", "rm", "-f", DEVICE_FILES + "/" + name)


def lava_cli_command(lava_cli, port, *command, check=True):
	return run_command([str(lava_cli), "-H", "127.0.0.1", "-P", str(port), *command], check=check, capture=True)


def wait_for_service(adb, serial, lava_cli, host_port, timeout):
	deadline = time.monotonic() + timeout
	last_error = ""
	while time.monotonic() < deadline:
		result = lava_cli_command(lava_cli, host_port, "status", check=False)
		if result.returncode == 0 and (result.stdout.startswith("PAUSED") or result.stdout.startswith("RUNNING")):
			return result.stdout.strip()
		last_error = (result.stderr or result.stdout).strip()
		device_result = read_result(adb, serial)
		if device_result is not None and device_result.get("status") in ("failed", "stopped", "done"):
			raise RuntimeError("Android replay exited before service became ready: " + str(device_result))
		if device_result is not None and device_result.get("status") == "running" and not app_is_running(adb, serial):
			raise RuntimeError("Android replay process exited before service became ready")
		time.sleep(0.25)
	raise RuntimeError("Timed out waiting for replay service: " + last_error)


def collect_artifacts(adb, serial, output_directory):
	output_directory.mkdir(parents=True, exist_ok=True)
	listing = adb_command(adb, serial, "shell", "ls", "-1", DEVICE_FILES, check=False, capture=True)
	if listing.returncode == 0:
		for name in listing.stdout.splitlines():
			name = name.strip()
			if name.endswith(".png") or name.endswith(".log") or name == "lava-replay-result.json":
				adb_command(adb, serial, "pull", DEVICE_FILES + "/" + name, str(output_directory / name), check=False)
	logcat = adb_command(adb, serial, "logcat", "-d", "-s", "VULKAN_LAVATUBE:*", check=False, capture=True)
	(output_directory / "logcat.txt").write_text(logcat.stdout, encoding="utf-8")


def command_build(arguments, repo):
	apk = build_apk(repo, arguments.abi, arguments.build_type)
	validate_apk_abi(apk, arguments.abi)
	print(apk)
	return 0


def command_run(arguments, repo):
	adb = arguments.adb
	serial = select_serial(adb, arguments.serial)
	device_abi = detect_abi(adb, serial)
	abi = device_abi if arguments.abi == "auto" else arguments.abi
	if abi != device_abi:
		raise RuntimeError("Requested ABI " + abi + " does not match connected device")
	apk = Path(arguments.apk).resolve() if arguments.apk else build_apk(repo, abi, arguments.build_type)
	if not apk.is_file():
		raise RuntimeError("APK does not exist: " + str(apk))
	validate_apk_abi(apk, abi)
	adb_command(adb, serial, "install", "-r", "-t", str(apk))
	prepare_storage(adb, serial)
	clean_run_outputs(adb, serial)

	trace = Path(arguments.trace).resolve()
	if not trace.is_file():
		raise RuntimeError("Trace does not exist: " + str(trace))
	device_trace = DEVICE_FILES + "/" + trace.name
	adb_command(adb, serial, "push", str(trace), device_trace)

	replay_arguments = list(arguments.replay_arguments)
	if replay_arguments and replay_arguments[0] == "--":
		replay_arguments.pop(0)
	if arguments.service:
		replay_arguments[0:0] = ["--service", "-H", "127.0.0.1", "-P", str(arguments.device_port)]
	replay_arguments.extend(["--", device_trace])
	command_line = shlex.join(replay_arguments)

	lava_cli = None
	if arguments.service:
		lava_cli = Path(arguments.lava_cli).resolve()
		if not lava_cli.is_file():
			raise RuntimeError("Desktop lava-cli does not exist: " + str(lava_cli))

	if arguments.service:
		adb_command(adb, serial, "forward", "tcp:" + str(arguments.host_port), "tcp:" + str(arguments.device_port))
	adb_shell_command(adb, serial, "am", "start", "-n", COMPONENT, "--es", "args", command_line)
	output_directory = Path(arguments.output_dir) if arguments.output_dir else repo / "artifacts" / "android-replay" / time.strftime("%Y%m%d-%H%M%S")

	if arguments.service:
		try:
			status = wait_for_service(adb, serial, lava_cli, arguments.host_port, arguments.timeout)
		except RuntimeError:
			adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE, check=False)
			collect_artifacts(adb, serial, output_directory)
			adb_command(adb, serial, "forward", "--remove", "tcp:" + str(arguments.host_port), check=False)
			raise
		print(status)
		print(shlex.join([str(lava_cli), "-H", "127.0.0.1", "-P", str(arguments.host_port)]), "<command>")
		return 0

	try:
		result = wait_for_result(adb, serial, True, arguments.timeout)
	except RuntimeError:
		adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE, check=False)
		collect_artifacts(adb, serial, output_directory)
		raise
	collect_artifacts(adb, serial, output_directory)
	print(json.dumps(result, sort_keys=True))
	print("Artifacts:", output_directory)
	return terminal_result_exit_code(result, ("done",))


def command_stop(arguments, repo):
	adb = arguments.adb
	serial = select_serial(adb, arguments.serial)
	lava_cli = Path(arguments.lava_cli).resolve()
	result = lava_cli_command(lava_cli, arguments.host_port, "stop", check=False)
	if result.returncode != 0:
		adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE, check=False)
	try:
		replay_result = wait_for_result(adb, serial, True, arguments.timeout)
	except RuntimeError:
		adb_command(adb, serial, "shell", "am", "force-stop", PACKAGE, check=False)
		replay_result = {"status": "failed", "exit_code": 1}
	output_directory = Path(arguments.output_dir) if arguments.output_dir else repo / "artifacts" / "android-replay" / time.strftime("%Y%m%d-%H%M%S")
	collect_artifacts(adb, serial, output_directory)
	adb_command(adb, serial, "forward", "--remove", "tcp:" + str(arguments.host_port), check=False)
	print(json.dumps(replay_result, sort_keys=True))
	print("Artifacts:", output_directory)
	if result.returncode != 0:
		return 1
	return terminal_result_exit_code(replay_result, ("done", "stopped"))


def create_parser():
	parser = argparse.ArgumentParser(description="Build and run the lava-replay NativeActivity APK")
	parser.add_argument("--adb", default=os.environ.get("ADB", "adb"))
	subparsers = parser.add_subparsers(dest="command", required=True)

	build = subparsers.add_parser("build")
	build.add_argument("--abi", choices=SUPPORTED_ABIS, default="arm64-v8a")
	build.add_argument("--build-type", choices=("debug", "release"), default="debug")

	run = subparsers.add_parser("run")
	run.add_argument("trace")
	run.add_argument("--serial")
	run.add_argument("--abi", choices=("auto",) + SUPPORTED_ABIS, default="auto")
	run.add_argument("--apk")
	run.add_argument("--build-type", choices=("debug", "release"), default="debug")
	run.add_argument("--service", action="store_true")
	run.add_argument("--host-port", type=int, default=11901)
	run.add_argument("--device-port", type=int, default=11901)
	run.add_argument("--lava-cli", default="build/lava-cli")
	run.add_argument("--timeout", type=float, default=120.0)
	run.add_argument("--output-dir")
	run.add_argument("replay_arguments", nargs=argparse.REMAINDER)

	stop = subparsers.add_parser("stop")
	stop.add_argument("--serial")
	stop.add_argument("--host-port", type=int, default=11901)
	stop.add_argument("--lava-cli", default="build/lava-cli")
	stop.add_argument("--timeout", type=float, default=30.0)
	stop.add_argument("--output-dir")
	return parser


def main():
	parser = create_parser()
	arguments = parser.parse_args()
	repo = Path(__file__).resolve().parent.parent
	try:
		if arguments.command == "build":
			return command_build(arguments, repo)
		if arguments.command == "run":
			return command_run(arguments, repo)
		return command_stop(arguments, repo)
	except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
		print("ERROR:", error, file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())
