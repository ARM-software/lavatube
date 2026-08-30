#!/usr/bin/python3

import os
import socket
import subprocess
import sys
import tempfile
import time


def reserve_port():
	with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
		sock.bind(('127.0.0.1', 0))
		return sock.getsockname()[1]


def run_cli(cli, port, *command):
	return subprocess.run(
		[cli, '-H', '127.0.0.1', '-P', str(port)] + list(command),
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=60,
	)


def wait_for_listener(replay, port, log_file):
	deadline = time.monotonic() + 15
	while time.monotonic() < deadline:
		if replay.poll() is not None:
			log_file.seek(0)
			raise RuntimeError('lava-replay exited before opening its service port:\n' + log_file.read().decode(errors='replace'))
		try:
			with socket.create_connection(('127.0.0.1', port), timeout=0.1):
				return
		except OSError:
			time.sleep(0.005)
	raise RuntimeError('timed out waiting for lava-replay service')


def check_cli(cli, port, expected, *command, expected_rc=0):
	result = run_cli(cli, port, *command)
	if result.returncode != expected_rc or result.stdout != expected:
		raise RuntimeError('%r returned rc=%d stdout=%r stderr=%r' %
		                   (command, result.returncode, result.stdout, result.stderr))
	return result


def check_pattern(filename, size):
	position = 0
	with open(filename, 'rb') as output:
		while position < size:
			data = output.read(min(64 * 1024, size - position))
			if not data:
				raise RuntimeError('saved device buffer ended at byte %d' % position)
			for offset, value in enumerate(data):
				expected = ((position + offset) * 37 + 11) & 0xff
				if value != expected:
					raise RuntimeError('saved device buffer differs at byte %d' % (position + offset))
			position += len(data)
		if output.read(1):
			raise RuntimeError('saved device buffer is larger than %d bytes' % size)


import argparse
import re

def check_save(cli, host, port, index, filename, size, staging_chunk=None):
	result = run_cli(cli, host, port, 'save', 'buffer', str(index), filename)
	if result.returncode != 0:
		raise RuntimeError('save buffer failed: rc=%d stdout=%r stderr=%r' %
		                   (result.returncode, result.stdout, result.stderr))
	lines = result.stdout.splitlines()
	if len(lines) != 6:
		raise RuntimeError('unexpected save statistics: %r' % result.stdout)
	prefix = 'DONE bytes=%d path=' % size
	if not lines[0].startswith(prefix):
		raise RuntimeError('unexpected save summary: %r' % lines[0])
	path = lines[0][len(prefix):].split()[0]
	if path not in ('mapped', 'staging'):
		raise RuntimeError('unexpected save path: %r' % path)
	if path == 'mapped':
		expected_chunks = (size + 4 * 1024 * 1024 - 1) // (4 * 1024 * 1024)
	elif staging_chunk is not None:
		expected_chunks = (size + staging_chunk - 1) // staging_chunk
	else:
		expected_chunks = None

	if expected_chunks is not None:
		if 'chunks=%d' % expected_chunks not in lines[0]:
			raise RuntimeError('unexpected chunk count (expected %d): %r' % (expected_chunks, lines[0]))
	else:
		match = re.search(r'chunks=(\d+)', lines[0])
		if not match or int(match.group(1)) <= 0:
			raise RuntimeError('unexpected chunk count: %r' % lines[0])
	stats = {}
	for label, line in zip(('replay', 'readback', 'send', 'controller', 'total'), lines[1:]):
		parts = line.split()
		if len(parts) != 4 or not parts[0].startswith(label + '=') or parts[1] != 'MiB/s' or not parts[2].startswith('time=') or parts[3] != 's':
			raise RuntimeError('malformed %s statistics: %r' % (label, line))
		val = float(parts[0].split('=', 1)[1])
		sec = float(parts[2].split('=', 1)[1])
		if val <= 0.0 or sec <= 0.0:
			raise RuntimeError('non-positive %s statistics: %r' % (label, line))
		stats[label] = (val, sec)
	return stats


def run_cli(cli, host, port, *command):
	return subprocess.run(
		[cli, '-H', host, '-P', str(port)] + list(command),
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=60,
	)


def check_cli(cli, host, port, expected, *command, expected_rc=0):
	result = run_cli(cli, host, port, *command)
	if result.returncode != expected_rc or result.stdout != expected:
		raise RuntimeError('%r returned rc=%d stdout=%r stderr=%r' %
		                   (command, result.returncode, result.stdout, result.stderr))
	return result


def main():
	parser = argparse.ArgumentParser(description='Test lava-cli save buffer command.')
	parser.add_argument('replay_path', nargs='?', default='build/lava-replay', help='path to lava-replay or lava-replay-android.py')
	parser.add_argument('cli_path', nargs='?', default='build/lava-cli', help='path to lava-cli')
	parser.add_argument('trace_path', nargs='?', default='build/tracing_cli_save_buffer.api', help='path to trace file')
	parser.add_argument('--serial', help='target Android device serial')
	parser.add_argument('--apk', help='path to lava-replay APK for Android')
	parser.add_argument('--external', help='HOST:PORT of an existing running replay service')
	parser.add_argument('--staging-chunk', type=int, default=1024 * 1024, help='staging chunk size in bytes (default: 1048576)')
	parser.add_argument('--verbose', '-v', action='store_true', help='print performance statistics')
	args = parser.parse_args()

	cli_path = args.cli_path
	trace_path = args.trace_path
	port = reserve_port()
	host = '127.0.0.1'

	if args.external:
		host, port_str = args.external.split(':')
		port = int(port_str)
		with tempfile.TemporaryDirectory() as output_dir:
			goto = run_cli(cli_path, host, port, 'goto', '0', 'vkDeviceWaitIdle')
			if goto.returncode != 0 or 'name=vkDeviceWaitIdle ' not in goto.stdout:
				raise RuntimeError('failed to reach buffer checkpoint: stdout=%r stderr=%r' % (goto.stdout, goto.stderr))

			mapped_output = os.path.join(output_dir, 'mapped buffer.bin')
			stats0 = check_save(cli_path, host, port, 0, mapped_output, 4 * 1024 * 1024 + 257)
			check_pattern(mapped_output, 4 * 1024 * 1024 + 257)

			device_output = os.path.join(output_dir, 'device buffer.bin')
			stats1 = check_save(cli_path, host, port, 1, device_output, 4 * 1024 * 1024 + 257)
			check_pattern(device_output, 4 * 1024 * 1024 + 257)

			if args.verbose:
				print("Buffer 0 stats:", stats0)
				print("Buffer 1 stats:", stats1)
		return

	is_android = bool(args.serial) or 'android' in args.replay_path
	if is_android:
		android_script = args.replay_path if args.replay_path.endswith('.py') else 'scripts/lava-replay-android.py'
		android_cmd = [
			'python3', android_script, 'run',
			'--service',
			'--host-port', str(port),
			'--device-port', str(port),
			'--lava-cli', cli_path,
		]
		if args.serial:
			android_cmd.extend(['--serial', args.serial])
		if args.apk:
			android_cmd.extend(['--apk', args.apk])
		android_cmd.append(trace_path)

		with tempfile.TemporaryDirectory() as output_dir:
			service_res = subprocess.run(android_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
			if service_res.returncode != 0:
				raise RuntimeError('Android service startup failed:\n' + service_res.stdout)
			try:
				goto = run_cli(cli_path, host, port, 'goto', '0', 'vkDeviceWaitIdle')
				if goto.returncode != 0 or 'name=vkDeviceWaitIdle ' not in goto.stdout:
					raise RuntimeError('failed to reach buffer checkpoint: stdout=%r stderr=%r' % (goto.stdout, goto.stderr))

				mapped_output = os.path.join(output_dir, 'mapped buffer.bin')
				stats0 = check_save(cli_path, host, port, 0, mapped_output, 4 * 1024 * 1024 + 257)
				check_pattern(mapped_output, 4 * 1024 * 1024 + 257)

				device_output = os.path.join(output_dir, 'device buffer.bin')
				stats1 = check_save(cli_path, host, port, 1, device_output, 4 * 1024 * 1024 + 257)
				check_pattern(device_output, 4 * 1024 * 1024 + 257)

				preserved_output = os.path.join(output_dir, 'preserved.bin')
				with open(preserved_output, 'wb') as output:
					output.write(b'preserve me')
				result = run_cli(cli_path, host, port, 'save', 'buffer', '9999', preserved_output)
				if result.returncode != 1 or not result.stdout.startswith('ERROR invalid buffer index'):
					raise RuntimeError('invalid buffer save did not fail cleanly: rc=%d stdout=%r stderr=%r' %
					                   (result.returncode, result.stdout, result.stderr))
				with open(preserved_output, 'rb') as output:
					if output.read() != b'preserve me':
						raise RuntimeError('failed save replaced the destination file')

				check_cli(cli_path, host, port, 'OK\n', 'stop')
				if args.verbose:
					print("Buffer 0 stats (MiB/s, sec):", stats0)
					print("Buffer 1 stats (MiB/s, sec):", stats1)
			finally:
				try:
					run_cli(cli_path, host, port, 'stop')
				except Exception:
					pass
				if args.serial:
					subprocess.run(['adb', '-s', args.serial, 'shell', 'am', 'force-stop', 'org.arm.lavatube.replay'], check=False)
					subprocess.run(['adb', '-s', args.serial, 'forward', '--remove', 'tcp:%d' % port], check=False)
		return

	# Desktop execution
	replay_path = args.replay_path
	with tempfile.NamedTemporaryFile() as log_file, tempfile.TemporaryDirectory() as output_dir:
		replay_environment = dict(os.environ)
		replay_environment['LAVATUBE_CLI_STAGING_CHUNK_SIZE'] = str(args.staging_chunk)
		replay = subprocess.Popen(
			[replay_path, '--service', '-H', host, '-P', str(port), '-w', 'none', trace_path],
			stdout=log_file,
			stderr=subprocess.STDOUT,
			env=replay_environment,
		)
		try:
			wait_for_listener(replay, port, log_file)
			goto = run_cli(cli_path, host, port, 'goto', '0', 'vkDeviceWaitIdle')
			if goto.returncode != 0 or 'name=vkDeviceWaitIdle ' not in goto.stdout:
				raise RuntimeError('failed to reach buffer checkpoint: stdout=%r stderr=%r' % (goto.stdout, goto.stderr))

			mapped_output = os.path.join(output_dir, 'mapped buffer.bin')
			stats0 = check_save(cli_path, host, port, 0, mapped_output, 4 * 1024 * 1024 + 257, staging_chunk=args.staging_chunk)
			check_pattern(mapped_output, 4 * 1024 * 1024 + 257)

			device_output = os.path.join(output_dir, 'device buffer.bin')
			stats1 = check_save(cli_path, host, port, 1, device_output, 4 * 1024 * 1024 + 257, staging_chunk=args.staging_chunk)
			check_pattern(device_output, 4 * 1024 * 1024 + 257)

			preserved_output = os.path.join(output_dir, 'preserved.bin')
			with open(preserved_output, 'wb') as output:
				output.write(b'preserve me')
			result = run_cli(cli_path, host, port, 'save', 'buffer', '9999', preserved_output)
			if result.returncode != 1 or not result.stdout.startswith('ERROR invalid buffer index'):
				raise RuntimeError('invalid buffer save did not fail cleanly: rc=%d stdout=%r stderr=%r' %
				                   (result.returncode, result.stdout, result.stderr))
			with open(preserved_output, 'rb') as output:
				if output.read() != b'preserve me':
					raise RuntimeError('failed save replaced the destination file')

			check_cli(cli_path, host, port, 'OK\n', 'stop')
			if args.verbose:
				print("Buffer 0 stats (MiB/s, sec):", stats0)
				print("Buffer 1 stats (MiB/s, sec):", stats1)
		finally:
			if replay.poll() is None:
				try:
					run_cli(cli_path, host, port, 'stop')
				except (OSError, subprocess.TimeoutExpired):
					pass
			try:
				replay.wait(timeout=30)
			except subprocess.TimeoutExpired:
				replay.terminate()
				replay.wait(timeout=5)
			if replay.returncode != 0:
				log_file.seek(0)
				raise RuntimeError('lava-replay exited with %d:\n%s' %
				                   (replay.returncode, log_file.read().decode(errors='replace')))


if __name__ == '__main__':
	main()
