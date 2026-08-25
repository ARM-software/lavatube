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


def wait_for_listener(replay, port):
	deadline = time.monotonic() + 15
	while time.monotonic() < deadline:
		if replay.poll() is not None:
			raise RuntimeError('lava-replay exited before opening its service port:\n' + replay.stdout.read())
		try:
			with socket.create_connection(('127.0.0.1', port), timeout=0.1):
				return
		except OSError:
			time.sleep(0.005)
	raise RuntimeError('timed out waiting for lava-replay service')


def stop_replay(replay, cli, port):
	if replay.poll() is None:
		try:
			run_cli(cli, port, 'stop')
		except (OSError, subprocess.TimeoutExpired):
			pass
	try:
		replay.wait(timeout=15)
	except subprocess.TimeoutExpired:
		replay.terminate()
		replay.wait(timeout=5)
	if replay.returncode != 0:
		raise RuntimeError('lava-replay failed:\n' + replay.stdout.read())


def check_startup(replay_path, cli_path, trace_path):
	port = reserve_port()
	replay = subprocess.Popen(
		[replay_path, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', '-B', trace_path],
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)
	try:
		wait_for_listener(replay, port)
		status = run_cli(cli_path, port, 'status')
		if status.returncode != 0:
			raise RuntimeError('status failed: stdout=' + repr(status.stdout) + ' stderr=' + repr(status.stderr))
		if status.stdout != 'PAUSED thread=0\n':
			raise RuntimeError('service became visible before replay initialization: ' + repr(status.stdout))

		threads = run_cli(cli_path, port, 'info', 'threads')
		if threads.returncode != 0:
			raise RuntimeError('info threads failed: stdout=' + repr(threads.stdout) + ' stderr=' + repr(threads.stderr))
		if '| Thread | Name' not in threads.stdout or '| 0 ' not in threads.stdout:
			raise RuntimeError('unexpected info threads output: ' + repr(threads.stdout))

		legacy_selection = run_cli(cli_path, port, 'thread', '0')
		if legacy_selection.returncode == 0 or legacy_selection.stdout != 'ERROR\n':
			raise RuntimeError('standalone thread selection was accepted: ' + repr(legacy_selection.stdout))
	finally:
		stop_replay(replay, cli_path, port)


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_startup_test.py LAVA_REPLAY LAVA_CLI TRACE')

	replay_path, cli_path, trace_path = sys.argv[1:]
	for unused in range(10):
		check_startup(replay_path, cli_path, trace_path)


if __name__ == '__main__':
	main()
