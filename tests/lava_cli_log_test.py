#!/usr/bin/python3

import glob
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


def run_cli(cli, port, environment, *command):
	return subprocess.run(
		[cli, '-H', '127.0.0.1', '-P', str(port)] + list(command),
		env=environment,
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=3,
	)


def start_replay(replay_path, cli_path, trace_path, port, environment):
	replay = subprocess.Popen(
		[replay_path, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', '-B', trace_path],
		env=environment,
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)
	deadline = time.monotonic() + 15
	while time.monotonic() < deadline:
		if replay.poll() is not None:
			raise RuntimeError('lava-replay exited before accepting log commands')
		try:
			result = run_cli(cli_path, port, environment, 'status')
		except subprocess.TimeoutExpired:
			result = None
		if result is not None and result.returncode == 0:
			return replay
		time.sleep(0.05)
	replay.terminate()
	replay.wait(timeout=5)
	raise RuntimeError('timed out waiting for lava-replay service')


def stop_replay(replay, cli_path, port, environment):
	if replay.poll() is None:
		try:
			run_cli(cli_path, port, environment, 'stop')
		except (OSError, subprocess.TimeoutExpired):
			pass
	try:
		replay.wait(timeout=15)
	except subprocess.TimeoutExpired:
		replay.terminate()
		replay.wait(timeout=5)
	if replay.returncode != 0:
		raise RuntimeError('lava-replay failed with exit code ' + str(replay.returncode))


def require_success(result, expected=None):
	if result.returncode != 0:
		raise RuntimeError('lava-cli failed: stdout=' + repr(result.stdout) + ' stderr=' + repr(result.stderr))
	if expected is not None and result.stdout != expected:
		raise RuntimeError('unexpected lava-cli output: ' + repr(result.stdout))


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_log_test.py LAVA_REPLAY LAVA_CLI TRACE')

	replay_path, cli_path, trace_path = sys.argv[1:]
	port = reserve_port()
	with tempfile.TemporaryDirectory(prefix='lavatube-cli-log-') as runtime_directory:
		environment = os.environ.copy()
		environment['XDG_RUNTIME_DIR'] = runtime_directory
		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'update=off')
			require_success(result, '')

			result = run_cli(cli_path, port, environment, 'log', 'update')
			require_success(result, 'DONE\n')

			cache_files = glob.glob(os.path.join(runtime_directory, 'lavatube', '*.log'))
			if len(cache_files) != 1:
				raise RuntimeError('expected one local log cache, got ' + repr(cache_files))
			cache_path = cache_files[0]
			with open(cache_path, encoding='utf-8') as cache:
				initial_log = cache.read()
			if 'Remote control listening on 127.0.0.1:' not in initial_log:
				raise RuntimeError('listener message missing from log cache: ' + repr(initial_log))
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'Remote control listening')
			require_success(result)
			if len(result.stdout.splitlines()) != 1:
				raise RuntimeError('default tail update did not return the listener log: ' + repr(result.stdout))

			initial_size = os.path.getsize(cache_path)
			require_success(run_cli(cli_path, port, environment, 'log', 'update'), 'DONE\n')
			if os.path.getsize(cache_path) != initial_size:
				raise RuntimeError('empty log update changed the cache')

			result = run_cli(cli_path, port, environment, 'log', 'tail', 'Remote control listening', 'update=off')
			require_success(result)
			lines = result.stdout.splitlines()
			if len(lines) != 1 or ':' not in lines[0]:
				raise RuntimeError('unexpected filtered tail output: ' + repr(result.stdout))
			line_number = lines[0].split(':', 1)[0]
			if not line_number.isdigit():
				raise RuntimeError('tail did not prefix a line number: ' + repr(result.stdout))

			result = run_cli(cli_path, port, environment, 'log', 'tail', 'Remote control listening', 'since=' + line_number, 'update=off')
			require_success(result, '')
			result = run_cli(cli_path, port, environment, 'log', 'tail', '[', 'update=off')
			if result.returncode == 0 or 'invalid regular expression' not in result.stderr:
				raise RuntimeError('invalid regular expression was accepted')
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'limit=1001', 'update=off')
			if result.returncode == 0 or 'between 1 and 1000' not in result.stderr:
				raise RuntimeError('oversized tail limit was accepted')
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'since=-1', 'update=off')
			if result.returncode == 0 or 'non-negative line number' not in result.stderr:
				raise RuntimeError('negative since line was accepted')

			with open(cache_path, 'ab') as cache:
				cache.write(b'corrupt')
			result = run_cli(cli_path, port, environment, 'log', 'update')
			if result.returncode == 0 or 'restart replay' not in result.stderr:
				raise RuntimeError('cache cursor mismatch did not request a replay restart')
		finally:
			stop_replay(replay, cli_path, port, environment)

		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'Remote control listening', 'update=off')
			require_success(result, '')
			if os.path.getsize(cache_path) != 0:
				raise RuntimeError('session validation did not clear the previous replay log cache')

			require_success(run_cli(cli_path, port, environment, 'log', 'update'), 'DONE\n')
			with open(cache_path, encoding='utf-8') as cache:
				restarted_log = cache.read()
			if restarted_log.count('Remote control listening on 127.0.0.1:') != 1:
				raise RuntimeError('restarted service did not reset its local cache: ' + repr(restarted_log))

			with open(cache_path, 'a', encoding='utf-8') as cache:
				for index in range(1, 21):
					cache.write('synthetic line ' + str(index) + '\n')
			result = run_cli(cli_path, port, environment, 'log', 'tail', 'synthetic', 'limit=3', 'update=off')
			require_success(result)
			if [line.split(':', 1)[1] for line in result.stdout.splitlines()] != ['synthetic line 18', 'synthetic line 19', 'synthetic line 20']:
				raise RuntimeError('tail limit did not retain the latest matches chronologically: ' + repr(result.stdout))
		finally:
			stop_replay(replay, cli_path, port, environment)

		result = run_cli(cli_path, port, environment, 'log', 'tail', 'Remote control listening', 'update=off')
		if result.returncode == 0:
			raise RuntimeError('update=off unexpectedly worked without a live replay service')


if __name__ == '__main__':
	main()
