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


def run_cli(cli, port, *command, timeout=60, environment=None):
	return subprocess.run(
		[cli, '-H', '127.0.0.1', '-P', str(port)] + list(command),
		env=environment,
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


def start_service(replay, port, trace, forced_result, log_file, blackhole=True, environment=None, extra_args=None):
	env = dict(os.environ if environment is None else environment)
	if forced_result is None:
		env.pop('LAVATUBE_TEST_RETVAL_RESULT', None)
	else:
		env['LAVATUBE_TEST_RETVAL_RESULT'] = forced_result
	command = [replay, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none']
	if blackhole:
		command.append('-B')
	if extra_args:
		command.extend(extra_args)
	command.append(trace)
	return subprocess.Popen(
		command,
		stdout=log_file,
		stderr=subprocess.STDOUT,
		env=env,
	)


def stop_replay(replay, cli, port, log_file, expected_exit=0, environment=None):
	if replay.poll() is None:
		try:
			run_cli(cli, port, 'stop', environment=environment)
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


def check_command(cli, port, expected, *command, expected_rc=0, environment=None):
	result = run_cli(cli, port, *command, environment=environment)
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
			for expected in ('name=vkQueueSubmit ', 'thread=1', 'result=ERROR_OUT_OF_HOST_MEMORY'):
				if expected not in response:
					raise RuntimeError('error pause response missing %r: %r' % (expected, response))
			status = check_command(cli_path, port, None, 'status').stdout
			if 'name=vkQueueSubmit ' not in status or 'result=ERROR_OUT_OF_HOST_MEMORY' not in status:
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
			if 'name=vkQueueSubmit ' not in response or 'result=ERROR_OUT_OF_HOST_MEMORY' not in response:
				raise RuntimeError('expected error pause, got: %r' % response)
			# Advancing past the error pauses normally again without the result suffix.
			result = check_command(cli_path, port, None, 'step', 'packets', '1')
			if 'result=' in result.stdout:
				raise RuntimeError('stale result reported after step: %r' % result.stdout)
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
			if 'name=vkQueueSubmit ' not in response or 'result=ERROR_OUT_OF_HOST_MEMORY' not in response:
				raise RuntimeError('expected error pause during goto function, got: %r' % response)
			# Verify error pause remains stable during queries and does not prematurely resume
			status = check_command(cli_path, port, None, 'status').stdout
			if 'name=vkQueueSubmit ' not in status or 'result=ERROR_OUT_OF_HOST_MEMORY' not in status:
				raise RuntimeError('status response missing error: %r' % status)
			parameters = check_command(cli_path, port, None, 'parameters').stdout
			if '"vkQueueSubmit"' not in parameters:
				raise RuntimeError('unexpected parameters response: %r' % parameters)
			check_command(cli_path, port, 'DONE\n', 'continue')
			check_command(cli_path, port, 'OK\n', 'stop')
		finally:
			stop_replay(replay, cli_path, port, log_file)


def check_retval_status_pause(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkWaitForFences,2', log_file)
		try:
			wait_for_listener(replay, port, log_file)
			result = check_command(cli_path, port, None, 'continue')
			if 'name=vkWaitForFences ' not in result.stdout or 'result=TIMEOUT' not in result.stdout:
				raise RuntimeError('expected status mismatch pause, got: %r' % result.stdout)
			parameters = check_command(cli_path, port, None, 'parameters').stdout
			if '"return" : "VK_TIMEOUT"' not in parameters:
				raise RuntimeError('status mismatch not preserved in parameters: %r' % parameters)
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


def check_retval_fatal_abort_from_recorded_status(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.NamedTemporaryFile() as log_file:
		replay = start_service(replay_path, port, trace_path, 'vkGetEventStatus,-4', log_file, blackhole=False)
		try:
			wait_for_listener(replay, port, log_file)
			result = check_command(cli_path, port, None, 'continue', expected_rc=1)
			if not result.stdout.startswith('ABORTED '):
				raise RuntimeError('expected abort from recorded status, got: %r' % result.stdout)
			if 'ERROR_DEVICE_LOST in vkGetEventStatus' not in result.stdout:
				raise RuntimeError('recorded-status abort response missing result: %r' % result.stdout)
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


def check_chameleon_device_loss(replay_path, cli_path, trace_path):
	port = reserve_port()
	with tempfile.TemporaryDirectory(prefix='lavatube-chameleon-device-loss-') as runtime_directory:
		with tempfile.NamedTemporaryFile() as log_file:
			environment = dict(os.environ)
			environment['XDG_RUNTIME_DIR'] = runtime_directory
			replay = start_service(
				replay_path,
				port,
				trace_path,
				None,
				log_file,
				blackhole=False,
				environment=environment,
				extra_args=['--device-fault-report'],
			)
			try:
				wait_for_listener(replay, port, log_file)
				result = check_command(cli_path, port, None, 'continue', expected_rc=1, environment=environment)
				if not result.stdout.startswith('ABORTED '):
					raise RuntimeError('expected Chameleon device-loss abort, got: %r' % result.stdout)
				for expected in ('ERROR_DEVICE_LOST in vkQueueSubmit', 'thread 0'):
					if expected not in result.stdout:
						raise RuntimeError('Chameleon abort response missing %r: %r' % (expected, result.stdout))

				check_command(cli_path, port, 'DONE\n', 'log', 'update', environment=environment)
				cache_files = glob.glob(os.path.join(runtime_directory, 'lavatube', '*.log'))
				if len(cache_files) != 1:
					raise RuntimeError('expected one replay log cache, got: %r' % cache_files)
				with open(cache_files[0], encoding='utf-8') as cache:
					replay_log = cache.read()
				for expected in (
					'Enabling replay device fault reporting with VK_KHR_device_fault',
					'Chameleon injected device loss',
					'Recent successful queue submissions (newest first):',
					'queue[0] fence[4294967295] thread=0',
				):
					if expected not in replay_log:
						raise RuntimeError('device-fault log missing %r:\n%s' % (expected, replay_log))
				check_command(cli_path, port, 'OK\n', 'stop', environment=environment)
			finally:
				stop_replay(replay, cli_path, port, log_file, expected_exit=1, environment=environment)


def main():
	if len(sys.argv) not in (4, 5):
		raise RuntimeError('usage: lava_cli_error_pause_test.py LAVA_REPLAY LAVA_CLI TRACE [--chameleon-device-loss]')

	replay_path, cli_path, trace_path = sys.argv[1:4]
	if len(sys.argv) == 5:
		if sys.argv[4] != '--chameleon-device-loss':
			raise RuntimeError('unknown mode: ' + sys.argv[4])
		check_chameleon_device_loss(replay_path, cli_path, trace_path)
		return
	check_retval_error_pause(replay_path, cli_path, trace_path)
	check_retval_error_selected_thread(replay_path, cli_path, trace_path)
	check_retval_error_goto_function(replay_path, cli_path, trace_path)
	check_retval_status_pause(replay_path, cli_path, trace_path)
	check_retval_fatal_abort(replay_path, cli_path, trace_path)
	check_retval_fatal_abort_from_recorded_status(replay_path, cli_path, trace_path)
	check_retval_fatal_abort_during_step(replay_path, cli_path, trace_path)


if __name__ == '__main__':
	main()
