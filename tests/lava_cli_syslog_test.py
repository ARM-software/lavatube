#!/usr/bin/python3

import glob
import os
import socket
import subprocess
import sys
import tempfile
import time


FAKE_JOURNALCTL = r'''#!/usr/bin/python3
import os
import re
import sys
import time

arguments = sys.argv[1:]
source = 'user' if '--user' in arguments else 'system'
if '--follow' not in arguments:
	if os.environ.get('FAKE_JOURNAL_' + source.upper()) == 'fail':
		print(source + ' journal permission denied', file=sys.stderr)
		sys.exit(1)
	sys.exit(0)

with open(os.environ['FAKE_JOURNAL_ARGUMENTS'], 'w', encoding='utf-8') as output:
	output.write('\n'.join(arguments) + '\n')
if os.environ.get('FAKE_JOURNAL_RUNTIME_FAIL') == 'yes':
	time.sleep(0.2)
	print('journal stream access revoked', file=sys.stderr)
	sys.exit(7)

pattern = next(argument[7:] for argument in arguments if argument.startswith('--grep='))
expression = re.compile(pattern, re.IGNORECASE)
path = os.environ['FAKE_JOURNAL_INPUT']
with open(path, 'r', encoding='utf-8') as journal:
	journal.seek(0, os.SEEK_END)
	while True:
		line = journal.readline()
		if not line:
			time.sleep(0.02)
			continue
		if expression.search(line):
			print(line.rstrip('\n'), flush=True)
'''


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
			raise RuntimeError('lava-replay exited before accepting syslog commands')
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


def append_journal(path, *lines):
	with open(path, 'a', encoding='utf-8') as journal:
		for line in lines:
			journal.write(line + '\n')


def wait_for_syslog(cli_path, port, environment, expected):
	deadline = time.monotonic() + 5
	last_result = None
	while time.monotonic() < deadline:
		last_result = run_cli(cli_path, port, environment, 'syslog', 'tail')
		if last_result.returncode == 0 and expected in last_result.stdout:
			return last_result
		time.sleep(0.05)
	raise RuntimeError('timed out waiting for syslog line: ' + repr(last_result.stdout if last_result else None))


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_syslog_test.py LAVA_REPLAY LAVA_CLI TRACE')

	replay_path, cli_path, trace_path = sys.argv[1:]
	port = reserve_port()
	with tempfile.TemporaryDirectory(prefix='lavatube-cli-syslog-') as runtime_directory:
		fake_bin = os.path.join(runtime_directory, 'bin')
		os.mkdir(fake_bin)
		fake_journalctl = os.path.join(fake_bin, 'journalctl')
		with open(fake_journalctl, 'w', encoding='utf-8') as output:
			output.write(FAKE_JOURNALCTL)
		os.chmod(fake_journalctl, 0o755)
		journal_input = os.path.join(runtime_directory, 'journal.input')
		open(journal_input, 'w', encoding='utf-8').close()

		environment = os.environ.copy()
		environment['PATH'] = fake_bin + os.pathsep + environment['PATH']
		environment['XDG_RUNTIME_DIR'] = runtime_directory
		environment['FAKE_JOURNAL_INPUT'] = journal_input
		environment['FAKE_JOURNAL_ARGUMENTS'] = os.path.join(runtime_directory, 'journal.arguments')

		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			require_success(run_cli(cli_path, port, environment, 'syslog', 'tail', 'update=off'), '')
			append_journal(journal_input, 'ordinary daemon message', 'MALI GPU fault in Vulkan driver')
			result = wait_for_syslog(cli_path, port, environment, 'MALI GPU fault')
			if 'ordinary daemon message' in result.stdout:
				raise RuntimeError('fixed replay-side filtering retained an unrelated journal line')
			if result.stderr:
				raise RuntimeError('unexpected full-access warning: ' + repr(result.stderr))

			arguments = open(environment['FAKE_JOURNAL_ARGUMENTS'], encoding='utf-8').read().splitlines()
			for required in ('--user', '--system', '--follow', '--lines=0', '--no-pager', '--output=short-iso-precise', '--truncate-newline', '--case-sensitive=no'):
				if required not in arguments:
					raise RuntimeError('journalctl follower omitted ' + required + ': ' + repr(arguments))
			if not any(argument.startswith('--grep=') for argument in arguments):
				raise RuntimeError('journalctl follower omitted its fixed filter')
			pattern = next(argument[7:] for argument in arguments if argument.startswith('--grep='))
			trace_stem = os.path.splitext(os.path.basename(trace_path))[0]
			for term in ('lavatube', 'lava-replay', 'vulkan', 'gpu', 'drm', 'mali', 'panthor', 'nvidia', 'amdgpu', 'i915', trace_stem):
				if term not in pattern:
					raise RuntimeError('journalctl fixed filter omitted ' + term + ': ' + pattern)

			require_success(run_cli(cli_path, port, environment, 'log', 'update'), 'DONE\n')
			if len(glob.glob(os.path.join(runtime_directory, 'lavatube', '*.log'))) != 1:
				raise RuntimeError('internal replay log cache was not kept separate')
			if len(glob.glob(os.path.join(runtime_directory, 'lavatube', '*.syslog'))) != 1:
				raise RuntimeError('system log cache was not created separately')
		finally:
			stop_replay(replay, cli_path, port, environment)

		environment['FAKE_JOURNAL_USER'] = 'fail'
		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			result = run_cli(cli_path, port, environment, 'syslog', 'update')
			require_success(result, 'DONE\n')
			if 'WARNING user journal unavailable' not in result.stderr:
				raise RuntimeError('partial journal access was not reported: ' + repr(result.stderr))
			result = run_cli(cli_path, port, environment, 'syslog', 'tail', 'update=off')
			require_success(result, '')
			arguments = open(environment['FAKE_JOURNAL_ARGUMENTS'], encoding='utf-8').read().splitlines()
			if '--user' in arguments or '--system' not in arguments:
				raise RuntimeError('follower did not restrict itself to accessible journals: ' + repr(arguments))
		finally:
			stop_replay(replay, cli_path, port, environment)

		del environment['FAKE_JOURNAL_USER']
		environment['FAKE_JOURNAL_SYSTEM'] = 'fail'
		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			result = run_cli(cli_path, port, environment, 'syslog', 'update')
			require_success(result, 'DONE\n')
			if 'WARNING system journal unavailable' not in result.stderr:
				raise RuntimeError('system journal access failure was not reported: ' + repr(result.stderr))
			arguments = open(environment['FAKE_JOURNAL_ARGUMENTS'], encoding='utf-8').read().splitlines()
			if '--system' in arguments or '--user' not in arguments:
				raise RuntimeError('follower did not use the accessible user journal: ' + repr(arguments))
		finally:
			stop_replay(replay, cli_path, port, environment)

		environment['FAKE_JOURNAL_USER'] = 'fail'
		environment['FAKE_JOURNAL_SYSTEM'] = 'fail'
		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			require_success(run_cli(cli_path, port, environment, 'status'))
			result = run_cli(cli_path, port, environment, 'syslog', 'update')
			if result.returncode == 0 or 'system log unavailable' not in result.stderr:
				raise RuntimeError('complete journal access failure was not isolated to syslog: ' + repr(result.stderr))
		finally:
			stop_replay(replay, cli_path, port, environment)

		del environment['FAKE_JOURNAL_USER']
		del environment['FAKE_JOURNAL_SYSTEM']
		environment['FAKE_JOURNAL_RUNTIME_FAIL'] = 'yes'
		replay = start_replay(replay_path, cli_path, trace_path, port, environment)
		try:
			time.sleep(0.3)
			result = run_cli(cli_path, port, environment, 'syslog', 'update')
			if result.returncode == 0 or 'journalctl follower exited with status 7' not in result.stderr or 'access revoked' not in result.stderr:
				raise RuntimeError('runtime journal failure was not reported: ' + repr(result.stderr))
		finally:
			stop_replay(replay, cli_path, port, environment)


if __name__ == '__main__':
	main()
