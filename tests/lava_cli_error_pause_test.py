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


def run_cli(cli, port, *command, timeout=60):
	return subprocess.run(
		[cli, '-H', '127.0.0.1', '-P', str(port)] + list(command),
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=timeout,
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


def start_service(replay, port, trace, retval_error, log_file):
	env = dict(os.environ)
	env['LAVATUBE_TEST_RETVAL_ERROR'] = retval_error
	return subprocess.Popen(
		[replay, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', '-B', trace],
		stdout=log_file,
		stderr=subprocess.STDOUT,
		env=env,
	)


def stop_replay(replay, cli, port, log_file, expected_exit=0):
	if replay.poll() is None:
		try:
			run_cli(cli, port, 'stop')
		except (OSError, subprocess.TimeoutExpired):
			pass
	try:
		replay.wait(timeout=30)
	except subprocess.TimeoutExpired:
		replay.terminate()
		replay.wait(timeout=5)
	if replay.returncode != expected_exit:
		log_file.seek(0)
		raise RuntimeError('lava-replay exited with %d, expected %d:\n' % (replay.returncode, expected_exit)
		                 + log_file.read().decode(errors='replace'))


def check_command(cli, port, expected, *command, expected_rc=0):
	result = run_cli(cli, port, *command)
	if result.returncode != expected_rc:
		raise RuntimeError('%s failed (rc=%d): stdout=%r stderr=%r' % (command, result.returncode, result.stdout, result.stderr))
	if expected is not None and result.stdout != expected:
		raise RuntimeError('%s response mismatch: %r' % (command, result.stdout))
	return result


def check_retval_error_pause(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkQueueSubmit,-1', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			check_command(cli_path, port, 'PAUSED thread=0\n', 'status')
			# The error happens on an unselected thread, so the selection retargets to it.
			result = check_command(cli_path, port, None, 'continue')
			response = result.stdout
			if not response.startswith('PAUSED @ packet='):
				raise RuntimeError('expected error pause, got: %r' % response)
			for expected in ('name=vkQueueSubmit ', 'thread=1', 'error=ERROR_OUT_OF_HOST_MEMORY'):
				if expected not in response:
					raise RuntimeError('error pause response missing %r: %r' % (expected, response))
			status = check_command(cli_path, port, None, 'status').stdout
			if 'name=vkQueueSubmit ' not in status or 'error=ERROR_OUT_OF_HOST_MEMORY' not in status:
				raise RuntimeError('status response missing error: %r' % status)
			parameters = check_command(cli_path, port, None, 'parameters').stdout
			if '"vkQueueSubmit"' not in parameters or '"thread" : 1' not in parameters:
				raise RuntimeError('unexpected parameters response: %r' % parameters)
			check_command(cli_path, port, 'DONE\n', 'continue')
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file)


def check_retval_error_selected_thread(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkQueueSubmit,-1', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			check_command(cli_path, port, None, 'thread', '1')
			result = check_command(cli_path, port, None, 'continue')
			response = result.stdout
			if 'name=vkQueueSubmit ' not in response or 'error=ERROR_OUT_OF_HOST_MEMORY' not in response:
				raise RuntimeError('expected error pause, got: %r' % response)
			# Advancing past the error pauses normally again without the error suffix.
			result = check_command(cli_path, port, None, 'step', 'packets', '1')
			if 'error=' in result.stdout:
				raise RuntimeError('stale error reported after step: %r' % result.stdout)
			check_command(cli_path, port, 'DONE\n', 'continue')
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file, expected_exit=0)


def check_retval_error_goto_function(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkQueueSubmit,-1', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			# Start on thread 1 and goto a function after vkQueueSubmit.
			check_command(cli_path, port, None, 'thread', '1')
			result = check_command(cli_path, port, None, 'goto', 'vkDestroyCommandPool')
			response = result.stdout
			if 'name=vkQueueSubmit ' not in response or 'error=ERROR_OUT_OF_HOST_MEMORY' not in response:
				raise RuntimeError('expected error pause during goto function, got: %r' % response)
			# Verify error pause remains stable during queries and does not prematurely resume
			status = check_command(cli_path, port, None, 'status').stdout
			if 'name=vkQueueSubmit ' not in status or 'error=ERROR_OUT_OF_HOST_MEMORY' not in status:
				raise RuntimeError('status response missing error: %r' % status)
			parameters = check_command(cli_path, port, None, 'parameters').stdout
			if '"vkQueueSubmit"' not in parameters:
				raise RuntimeError('unexpected parameters response: %r' % parameters)
			check_command(cli_path, port, 'DONE\n', 'continue')
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file)


def check_retval_fatal_abort(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkQueueSubmit,-4', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			result = check_command(cli_path, port, None, 'continue', expected_rc=1)
			if result.returncode != 1:
				raise RuntimeError('continue should exit nonzero on abort, got %d' % result.returncode)
			response = result.stdout
			if not response.startswith('ABORTED '):
				raise RuntimeError('expected abort response, got: %r' % response)
			for expected in ('ERROR_DEVICE_LOST in vkQueueSubmit', 'thread 1'):
				if expected not in response:
					raise RuntimeError('abort response missing %r: %r' % (expected, response))
			# The service stays up so the final state can still be inspected.
			result = check_command(cli_path, port, None, 'status', expected_rc=1)
			if result.returncode != 1 or not result.stdout.startswith('ABORTED '):
				raise RuntimeError('status after abort: rc=%d stdout=%r' % (result.returncode, result.stdout))
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file, expected_exit=1)


def check_retval_fatal_abort_during_step(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkQueueSubmit,-4', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			check_command(cli_path, port, None, 'thread', '1')
			result = check_command(cli_path, port, None, 'step', 'packets', '10', expected_rc=1)
			if result.returncode != 1:
				raise RuntimeError('step should exit nonzero on abort, got %d' % result.returncode)
			if not result.stdout.startswith('ABORTED '):
				raise RuntimeError('expected abort response from step, got: %r' % result.stdout)
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file, expected_exit=1)


def main():
	if len(sys.argv) != 4:
		raise RuntimeError('usage: lava_cli_error_pause_test.py LAVA_REPLAY LAVA_CLI TRACE')

	replay_path, cli_path, trace_path = sys.argv[1:]
	check_retval_error_pause(replay_path, cli_path, trace_path)
	check_retval_error_selected_thread(replay_path, cli_path, trace_path)
	check_retval_error_goto_function(replay_path, cli_path, trace_path)
	check_retval_fatal_abort(replay_path, cli_path, trace_path)
	check_retval_fatal_abort_during_step(replay_path, cli_path, trace_path)


if __name__ == '__main__':
	main()
