#!/usr/bin/python3

import json
import os
import re
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
		raise RuntimeError('usage: lava_cli_info_trace.py LAVA_REPLAY LAVA_CLI TRACE')

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
				raise RuntimeError('lava-replay exited before accepting commands:\n' + replay.stdout.read())
			try:
				result = run_cli(cli_path, port, 'info', 'trace')
			except subprocess.TimeoutExpired:
				result = None
			if result is not None and result.returncode == 0:
				break
			time.sleep(0.05)
		else:
			raise RuntimeError('timed out waiting for lava-replay service')

		lines = result.stdout.splitlines()
		if len(lines) != 1:
			raise RuntimeError('info trace did not return exactly one NDJSON record: ' + repr(result.stdout))
		value = json.loads(lines[0])
		if set(value) != {'filename', 'file_size', 'creation_timestamp'}:
			raise RuntimeError('unexpected info trace fields: ' + repr(value))
		if value['filename'] != trace_path:
			raise RuntimeError('unexpected trace filename: ' + repr(value['filename']))
		if value['file_size'] != os.path.getsize(trace_path):
			raise RuntimeError('unexpected trace file size: ' + repr(value['file_size']))
		creation_timestamp = value['creation_timestamp']
		if creation_timestamp is not None and not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z', creation_timestamp):
			raise RuntimeError('unexpected creation timestamp: ' + repr(creation_timestamp))
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
