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


def check_save(cli, port, index, filename, size):
	result = run_cli(cli, port, 'save', 'buffer', str(index), filename)
	if result.returncode != 0:
		raise RuntimeError('save buffer failed: rc=%d stdout=%r stderr=%r' %
		                   (result.returncode, result.stdout, result.stderr))
	lines = result.stdout.splitlines()
	if len(lines) != 6:
		raise RuntimeError('unexpected save statistics: %r' % result.stdout)
	prefix = 'DONE bytes=%d path=' % size
	if (not lines[0].startswith(prefix)
	    or not lines[0].endswith((' receive=splice', ' receive=fallback', ' receive=mixed'))):
		raise RuntimeError('unexpected save summary: %r' % lines[0])
	path = lines[0][len(prefix):].split()[0]
	if path not in ('mapped', 'staging'):
		raise RuntimeError('unexpected save path: %r' % path)
	expected_chunks = 2 if path == 'mapped' else 5
	if ' chunks=%d ' % expected_chunks not in lines[0]:
		raise RuntimeError('unexpected chunk count: %r' % lines[0])
	for label, line in zip(('replay', 'readback', 'send', 'controller', 'total'), lines[1:]):
		parts = line.split()
		if len(parts) != 4 or not parts[0].startswith(label + '=') or parts[1] != 'MiB/s' or not parts[2].startswith('time=') or parts[3] != 's':
			raise RuntimeError('malformed %s statistics: %r' % (label, line))
		if float(parts[0].split('=', 1)[1]) <= 0.0 or float(parts[2].split('=', 1)[1]) <= 0.0:
			raise RuntimeError('non-positive %s statistics: %r' % (label, line))


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_save_buffer_test.py LAVA_REPLAY LAVA_CLI TRACE')
	replay_path, cli_path, trace_path = sys.argv[1:]
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file, tempfile.TemporaryDirectory() as output_dir:
		replay_environment = dict(os.environ)
		replay_environment['LAVATUBE_CLI_STAGING_CHUNK_SIZE'] = str(1024 * 1024)
		replay = subprocess.Popen(
			[replay_path, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', trace_path],
			stdout=log_file,
			stderr=subprocess.STDOUT,
			env=replay_environment,
		)
		try:
			wait_for_listener(replay, port, log_file)
			goto = run_cli(cli_path, port, 'goto', '0', 'vkDeviceWaitIdle')
			if goto.returncode != 0 or 'name=vkDeviceWaitIdle ' not in goto.stdout:
				raise RuntimeError('failed to reach buffer checkpoint: stdout=%r stderr=%r' % (goto.stdout, goto.stderr))

			mapped_output = os.path.join(output_dir, 'mapped buffer.bin')
			check_save(cli_path, port, 0, mapped_output, 4 * 1024 * 1024 + 257)
			check_pattern(mapped_output, 4 * 1024 * 1024 + 257)

			device_output = os.path.join(output_dir, 'device buffer.bin')
			check_save(cli_path, port, 1, device_output, 4 * 1024 * 1024 + 257)
			check_pattern(device_output, 4 * 1024 * 1024 + 257)

			preserved_output = os.path.join(output_dir, 'preserved.bin')
			with open(preserved_output, 'wb') as output:
				output.write(b'preserve me')
			result = run_cli(cli_path, port, 'save', 'buffer', '9999', preserved_output)
			if result.returncode != 1 or not result.stdout.startswith('ERROR invalid buffer index'):
				raise RuntimeError('invalid buffer save did not fail cleanly: rc=%d stdout=%r stderr=%r' %
				                   (result.returncode, result.stdout, result.stderr))
			with open(preserved_output, 'rb') as output:
				if output.read() != b'preserve me':
					raise RuntimeError('failed save replaced the destination file')

			check_cli(cli_path, port, 'OK\n', 'stop')
		finally:
			if replay.poll() is None:
				try:
					run_cli(cli_path, port, 'stop')
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
