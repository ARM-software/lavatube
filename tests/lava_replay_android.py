#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPT_PATH = Path(__file__).resolve().parent.parent / "scripts" / "lava-replay-android.py"
SPEC = importlib.util.spec_from_file_location("lava_replay_android", SCRIPT_PATH)
LAVA_REPLAY_ANDROID = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAVA_REPLAY_ANDROID)


class AndroidReplayLauncherTest(unittest.TestCase):
	def test_apk_path_comes_from_gradle_metadata(self):
		with tempfile.TemporaryDirectory() as temporary_directory:
			repo = Path(temporary_directory)
			output_directory = repo / "android" / "replay" / "build" / "outputs" / "apk" / "release"
			output_directory.mkdir(parents=True)
			apk = output_directory / "replay-release-unsigned.apk"
			apk.touch()
			(output_directory / "output-metadata.json").write_text(
				'{"elements":[{"outputFile":"replay-release-unsigned.apk"}]}', encoding="utf-8")
			self.assertEqual(LAVA_REPLAY_ANDROID.apk_from_output_metadata(repo, "release"), apk)

	def test_regular_run_requires_done_status(self):
		get_exit_code = LAVA_REPLAY_ANDROID.terminal_result_exit_code
		self.assertEqual(get_exit_code({"status": "done", "exit_code": 0}, ("done",)), 0)
		self.assertEqual(get_exit_code({"status": "stopped", "exit_code": 0}, ("done",)), 1)
		self.assertEqual(get_exit_code({"status": "failed", "exit_code": 77}, ("done",)), 77)

	def test_stop_accepts_done_or_stopped(self):
		get_exit_code = LAVA_REPLAY_ANDROID.terminal_result_exit_code
		self.assertEqual(get_exit_code({"status": "done", "exit_code": 0}, ("done", "stopped")), 0)
		self.assertEqual(get_exit_code({"status": "stopped", "exit_code": 0}, ("done", "stopped")), 0)
		self.assertEqual(get_exit_code({"status": "failed", "exit_code": 0}, ("done", "stopped")), 1)

	def test_run_timeout_force_stops_application(self):
		with tempfile.TemporaryDirectory() as temporary_directory:
			temporary_path = Path(temporary_directory)
			trace = temporary_path / "trace.api"
			trace.touch()
			apk = temporary_path / "replay-debug.apk"
			apk.touch()
			arguments = SimpleNamespace(
				adb="adb",
				serial="emulator-5554",
				abi="auto",
				apk=str(apk),
				build_type="debug",
				trace=str(trace),
				replay_arguments=[],
				service=False,
				device_port=11901,
				host_port=11901,
				lava_cli="build/lava-cli",
				timeout=1.0,
				output_dir=str(temporary_path / "artifacts"),
			)
			with mock.patch.object(LAVA_REPLAY_ANDROID, "select_serial", return_value="emulator-5554"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "detect_abi", return_value="x86_64"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "validate_apk_abi"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "prepare_storage"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "clean_run_outputs"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "adb_shell_command"), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "wait_for_result", side_effect=RuntimeError("timeout")), \
				mock.patch.object(LAVA_REPLAY_ANDROID, "collect_artifacts") as collect_artifacts, \
				mock.patch.object(LAVA_REPLAY_ANDROID, "adb_command") as adb_command:
				with self.assertRaisesRegex(RuntimeError, "timeout"):
					LAVA_REPLAY_ANDROID.command_run(arguments, temporary_path)
				adb_command.assert_any_call(
					"adb", "emulator-5554", "shell", "am", "force-stop",
					LAVA_REPLAY_ANDROID.PACKAGE, check=False)
				collect_artifacts.assert_called_once()

	def test_stop_timeout_returns_failure(self):
		arguments = SimpleNamespace(
			adb="adb",
			serial="emulator-5554",
			lava_cli="build/lava-cli",
			host_port=11901,
			timeout=1.0,
			output_dir="artifacts",
		)
		cli_result = subprocess.CompletedProcess([], 0, stdout="OK\n", stderr="")
		with mock.patch.object(LAVA_REPLAY_ANDROID, "select_serial", return_value="emulator-5554"), \
			mock.patch.object(LAVA_REPLAY_ANDROID, "lava_cli_command", return_value=cli_result), \
			mock.patch.object(LAVA_REPLAY_ANDROID, "wait_for_result", side_effect=RuntimeError("timeout")), \
			mock.patch.object(LAVA_REPLAY_ANDROID, "collect_artifacts"), \
			mock.patch.object(LAVA_REPLAY_ANDROID, "adb_command") as adb_command, \
			mock.patch("builtins.print"):
			self.assertEqual(LAVA_REPLAY_ANDROID.command_stop(arguments, Path(".")), 1)
			adb_command.assert_any_call(
				"adb", "emulator-5554", "shell", "am", "force-stop",
				LAVA_REPLAY_ANDROID.PACKAGE, check=False)


if __name__ == "__main__":
	unittest.main()
