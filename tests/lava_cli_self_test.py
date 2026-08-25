#!/usr/bin/python3

import socket
import subprocess
import sys
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
		timeout=2,
	)


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_self_test.py LAVA_REPLAY LAVA_CLI TRACE')

	replay_path, cli_path, trace_path = sys.argv[1:]
	port = reserve_port()
	replay = subprocess.Popen(
		[replay_path, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', '-B', trace_path],
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)

	try:
		deadline = time.monotonic() + 15
		result = None
		while time.monotonic() < deadline:
			if replay.poll() is not None:
				raise RuntimeError('lava-replay exited before self-test:\n' + replay.stdout.read())
			try:
				result = run_cli(cli_path, port, 'self-test')
			except subprocess.TimeoutExpired:
				result = None
			if result is not None and result.returncode == 0:
				break
			time.sleep(0.05)
		else:
			raise RuntimeError('timed out waiting for a successful self-test')

		if result.stdout != 'OK\n':
			raise RuntimeError('unexpected self-test response: ' + repr(result.stdout))

		result = run_cli(cli_path, port, 'step', '0', 'calls', '1')
		if result.returncode != 0 or 'api_calls=1' not in result.stdout:
			raise RuntimeError('failed to step one call before self-test: stdout=' + repr(result.stdout) + ' stderr=' + repr(result.stderr))
		result = run_cli(cli_path, port, 'self-test')
		if result.returncode != 0 or result.stdout != 'OK\n':
			raise RuntimeError('self-test failed after stepping: stdout=' + repr(result.stdout) + ' stderr=' + repr(result.stderr))
	finally:
		if replay.poll() is None:
			try:
				run_cli(cli_path, port, 'stop')
			except (OSError, subprocess.TimeoutExpired):
				pass
		try:
			replay.wait(timeout=15)
		except subprocess.TimeoutExpired:
			replay.terminate()
			replay.wait(timeout=5)

	if replay.returncode != 0:
		raise RuntimeError('lava-replay failed:\n' + replay.stdout.read())


if __name__ == '__main__':
	main()
