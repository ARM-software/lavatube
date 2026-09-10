#!/usr/bin/python3

import http.server
import json
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


EXPECTED_TOOLS = {
	'trace_get_metadata',
	'trace_list_threads',
	'trace_get_thread',
	'trace_get_frame',
	'trace_list_objects',
	'trace_get_object',
	'trace_get_packets',
	'trace_find_calls',
	'replay_get_status',
	'replay_list_threads',
	'replay_diagnose_deadlock',
	'replay_diagnose_device',
	'replay_get_memory',
	'replay_get_suballocator',
	'replay_get_object_state',
	'replay_get_instrumentation',
	'replay_get_as_build',
}


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
		timeout=5,
	)


def response_output(handler, output):
	body = json.dumps({'output': output}, separators=(',', ':')).encode('utf-8')
	handler.send_response(200)
	handler.send_header('Content-Type', 'application/json')
	handler.send_header('Content-Length', str(len(body)))
	handler.end_headers()
	handler.wfile.write(body)


class AgentModelHandler(http.server.BaseHTTPRequestHandler):
	requests = []
	failure = None
	mode = 'normal'
	cli = None
	replay_port = None

	def log_message(self, format_string, *args):
		pass

	def do_POST(self):
		try:
			if self.path != '/v1/responses':
				raise RuntimeError('unexpected model path: ' + self.path)
			length = int(self.headers['Content-Length'])
			request = json.loads(self.rfile.read(length))
			self.__class__.requests.append(request)
			tool_names = {tool['name'] for tool in request['tools']}
			if tool_names != EXPECTED_TOOLS:
				raise RuntimeError('unexpected tool registry: %r' % tool_names)
			if request.get('parallel_tool_calls') is not False:
				raise RuntimeError('parallel tool calls were not disabled')

			round_number = len(self.__class__.requests)
			if self.__class__.mode in ('truncated', 'tool_stall'):
				if round_number == 1:
					response_output(self, [
						{'type': 'function_call', 'name': 'replay_get_instrumentation',
						 'call_id': 'instrumentation', 'arguments': '{"command_buffer":0}'},
					])
					return
				if self.__class__.mode == 'truncated' and round_number == 2:
					outputs = [item for item in request['input'] if item.get('type') == 'function_call_output']
					tool_output = json.loads(outputs[-1]['output'])
					result = tool_output['result']
					if (not tool_output['ok'] or not result.get('truncated')
							or result.get('limit_bytes') != 1024 * 1024
							or result.get('received_bytes') <= result['limit_bytes']):
						raise RuntimeError('receive-limit truncation was not explicit: %r' % tool_output)
					final = {
						'status': 'answered',
						'conclusion': 'The replay response exceeded the receive limit.',
						'confidence': 1.0,
						'evidence': [{'tool_id': 1, 'inference': 'The response was explicitly truncated.'}],
						'unresolved': [],
					}
					response_output(self, [
						{'type': 'message', 'content': [{'type': 'output_text',
						 'text': json.dumps(final, separators=(',', ':'))}]},
					])
					return
				raise RuntimeError('unexpected model round %d in %s mode' % (round_number, self.__class__.mode))
			if self.__class__.mode == 'immediate':
				final = {
					'status': 'inconclusive',
					'conclusion': 'No investigation was needed.',
					'confidence': 0.0,
					'evidence': [],
					'unresolved': [],
				}
				response_output(self, [
					{'type': 'message', 'content': [{'type': 'output_text',
					 'text': json.dumps(final, separators=(',', ':'))}]},
				])
				return
			if self.__class__.mode == 'unstable':
				step = run_cli(self.__class__.cli, self.__class__.replay_port, 'step', '0', 'packets', '1')
				if step.returncode != 0:
					raise RuntimeError('external replay step failed: %r %r' % (step.stdout, step.stderr))
				final = {
					'status': 'inconclusive',
					'conclusion': 'No investigation was needed.',
					'confidence': 0.0,
					'evidence': [],
					'unresolved': [],
				}
				response_output(self, [
					{'type': 'message', 'content': [{'type': 'output_text', 'text': '```json\n' + json.dumps(final, separators=(',', ':')) + '\n```'}]},
				])
				return
			if self.__class__.mode == 'oversized':
				final = {
					'status': 'answered',
					'conclusion': 'x' * 4096,
					'confidence': 1.0,
					'evidence': [],
					'unresolved': [],
				}
				response_output(self, [
					{'type': 'message', 'content': [{'type': 'output_text', 'text': json.dumps(final, separators=(',', ':'))}]},
				])
				return
			if round_number == 1:
				response_output(self, [
					{'type': 'function_call', 'name': 'trace_find_calls', 'call_id': 'find',
					 'arguments': '{"name":"vkCreateInstance","thread":0,"limit":2}'},
					{'type': 'function_call', 'name': 'replay_get_status', 'call_id': 'status',
					 'arguments': '{}'},
				])
				return
			if round_number == 2:
				outputs = [item for item in request['input'] if item.get('type') == 'function_call_output']
				if len(outputs) != 2:
					raise RuntimeError('expected two first-round tool outputs: %r' % outputs)
				find = json.loads(outputs[0]['output'])
				if find['tool_id'] != 1 or not find['ok'] or not find['result']['matches']:
					raise RuntimeError('unexpected find result: %r' % find)
				status = json.loads(outputs[1]['output'])
				if status['tool_id'] != 2 or not status['ok'] or not status['result']['output'].startswith('PAUSED'):
					raise RuntimeError('unexpected status result: %r' % status)
				match = find['result']['matches'][0]
				arguments = json.dumps({'thread': match['thread'], 'start': match['packet']}, separators=(',', ':'))
				response_output(self, [
					{'type': 'function_call', 'name': 'trace_get_packets', 'call_id': 'packet', 'arguments': arguments},
				])
				return
			if round_number == 3:
				outputs = [item for item in request['input'] if item.get('type') == 'function_call_output']
				packet = json.loads(outputs[-1]['output'])
				if packet['tool_id'] != 3 or not packet['ok']:
					raise RuntimeError('unexpected packet result: %r' % packet)
				if packet['result']['packets'][0].get('name') != 'vkCreateInstance':
					raise RuntimeError('wrong decoded packet: %r' % packet)
				final = {
					'status': 'answered',
					'conclusion': 'The selected replay is paused on the same trace, and vkCreateInstance is present.',
					'confidence': 1.0,
					'evidence': [
						{'tool_id': 1, 'inference': 'The exact call was found in the local trace.'},
						{'tool_id': 2, 'inference': 'The live replay service reported a paused state.'},
						{'tool_id': 3, 'inference': 'The selected local packet decoded as vkCreateInstance.'},
					],
					'unresolved': [],
				}
				response_output(self, [
					{'type': 'message', 'content': [{'type': 'output_text', 'text': json.dumps(final, separators=(',', ':'))}]},
				])
				return
			raise RuntimeError('unexpected model round %d' % round_number)
		except Exception as exc:
			self.__class__.failure = exc
			body = json.dumps({'error': {'message': str(exc)}}).encode('utf-8')
			self.send_response(500)
			self.send_header('Content-Length', str(len(body)))
			self.end_headers()
			self.wfile.write(body)


class ReplayTestHandler(socketserver.BaseRequestHandler):
	def handle(self):
		command = b''
		while not command.endswith(b'\n'):
			part = self.request.recv(4096)
			if not part:
				return
			command += part
		command = command.decode('utf-8').strip()
		self.server.commands.append(command)
		if command == 'status':
			self.server.status_calls += 1
			if self.server.mode == 'validation_stall' or (
					self.server.mode == 'final_stall' and self.server.status_calls == 2):
				time.sleep(5)
				return
			self.request.sendall(b'PAUSED (Global: 0)\n')
			return
		if command == 'info trace':
			body = json.dumps({
				'device': self.server.trace_stat.st_dev,
				'inode': self.server.trace_stat.st_ino,
			}, separators=(',', ':')).encode('utf-8')
			self.request.sendall(body + b'\n')
			return
		if command == 'show instrumentation 0':
			if self.server.mode == 'tool_stall':
				time.sleep(5)
				return
			try:
				self.request.sendall(b'X' * (1024 * 1024 + 4096))
			except OSError:
				pass
			return
		self.request.sendall(b'ERROR unexpected test command\n')


class ReplayTestServer(socketserver.ThreadingTCPServer):
	allow_reuse_address = True
	daemon_threads = True

	def __init__(self, trace, mode):
		super().__init__(('127.0.0.1', 0), ReplayTestHandler)
		self.trace_stat = os.stat(trace)
		self.mode = mode
		self.commands = []
		self.status_calls = 0


def run_fake_replay_test(agent, trace, model_port, mode):
	service = ReplayTestServer(trace, mode)
	thread = threading.Thread(target=service.serve_forever)
	thread.start()
	AgentModelHandler.mode = 'immediate' if mode in ('validation_stall', 'final_stall') else mode
	AgentModelHandler.requests = []
	timeout_seconds = 5 if mode == 'truncated' else 1
	started = time.monotonic()
	try:
		result = subprocess.run(
			[agent, '--service', '127.0.0.1:%d' % service.server_address[1],
			 '--base-url', 'http://127.0.0.1:%d/v1' % model_port,
			 '--model', 'test-model', '--timeout', str(timeout_seconds),
			 trace, 'ask', 'Exercise the replay service boundary.'],
			text=True,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			timeout=8,
		)
	finally:
		elapsed = time.monotonic() - started
		service.shutdown()
		service.server_close()
		thread.join()
	if mode != 'truncated' and elapsed > 2.5:
		raise RuntimeError('%s exceeded the shared deadline: %.3fs' % (mode, elapsed))
	lines = result.stdout.splitlines()
	if len(lines) != 1:
		raise RuntimeError('%s stdout was not one JSON line: %r' % (mode, result.stdout))
	output = json.loads(lines[0])
	return result, output, service.commands


def main():
	if len(sys.argv) != 5:
		raise RuntimeError('usage: lava_agent_test.py LAVA_AGENT LAVA_REPLAY LAVA_CLI TRACE')
	agent, replay_path, cli, trace = sys.argv[1:]
	replay_port = reserve_port()
	model_port = reserve_port()
	replay = subprocess.Popen(
		[replay_path, '--service', '-H', '127.0.0.1', '-P', str(replay_port), '-w', 'none', '-B', trace],
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)
	server = http.server.ThreadingHTTPServer(('127.0.0.1', model_port), AgentModelHandler)
	AgentModelHandler.cli = cli
	AgentModelHandler.replay_port = replay_port
	server_thread = threading.Thread(target=server.serve_forever)
	server_thread.start()

	try:
		deadline = time.monotonic() + 15
		while time.monotonic() < deadline:
			if replay.poll() is not None:
				raise RuntimeError('lava-replay exited early:\n' + replay.stdout.read())
			try:
				status = run_cli(cli, replay_port, 'status')
			except subprocess.TimeoutExpired:
				status = None
			if status is not None and status.returncode == 0 and status.stdout.startswith('PAUSED'):
				break
			time.sleep(0.05)
		else:
			raise RuntimeError('timed out waiting for replay service')

		with tempfile.TemporaryDirectory() as temporary:
			debug_file = os.path.join(temporary, 'agent.jsonl')
			result = subprocess.run(
				[agent, '--service', '127.0.0.1:%d' % replay_port,
				 '--base-url', 'http://127.0.0.1:%d/v1' % model_port,
				 '--model', 'test-model', '-d', '3', '-df', debug_file,
				 trace, 'ask', 'Confirm the test evidence.'],
				text=True,
				stdout=subprocess.PIPE,
				stderr=subprocess.PIPE,
				timeout=30,
			)
			if result.returncode != 0:
				raise RuntimeError('lava-agent failed: stdout=%r stderr=%r' % (result.stdout, result.stderr))
			if not result.stderr:
				raise RuntimeError('debug packet decoding did not emit diagnostics to stderr')
			lines = result.stdout.splitlines()
			if len(lines) != 1:
				raise RuntimeError('lava-agent stdout was not one JSON line: %r' % result.stdout)
			output = json.loads(lines[0])
			if output['schema_version'] != 1 or output['status'] != 'answered':
				raise RuntimeError('unexpected agent result: %r' % output)
			if [item['tool_id'] for item in output['evidence']] != [1, 2, 3]:
				raise RuntimeError('runtime evidence IDs were not preserved: %r' % output['evidence'])
			if output['usage'] != {'rounds': 3, 'calls': 3}:
				raise RuntimeError('unexpected usage: %r' % output['usage'])
			with open(debug_file, encoding='utf-8') as debug:
				debug_events = [json.loads(line) for line in debug]
			if [event['type'] for event in debug_events].count('tool_call') != 3:
				raise RuntimeError('debug transcript omitted exploratory calls')

		AgentModelHandler.mode = 'unstable'
		AgentModelHandler.requests = []
		unstable = subprocess.run(
			[agent, '--service', '127.0.0.1:%d' % replay_port,
			 '--base-url', 'http://127.0.0.1:%d/v1' % model_port,
			 '--model', 'test-model', trace, 'ask', 'Trigger the stability check.'],
			text=True,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			timeout=30,
		)
		unstable_output = json.loads(unstable.stdout)
		if unstable.returncode != 0 or unstable_output['status'] != 'unstable':
			raise RuntimeError('position change was not reported as unstable: %r %r' % (unstable.stdout, unstable.stderr))

		AgentModelHandler.mode = 'oversized'
		AgentModelHandler.requests = []
		oversized = subprocess.run(
			[agent, '--service', '127.0.0.1:%d' % replay_port,
			 '--base-url', 'http://127.0.0.1:%d/v1' % model_port,
			 '--model', 'test-model', '--max-output-bytes', '1024',
			 trace, 'ask', 'Return an oversized result.'],
			text=True,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			timeout=30,
		)
		oversized_output = json.loads(oversized.stdout)
		if oversized.returncode != 0 or oversized_output['status'] != 'budget_exhausted' or len(oversized.stdout.rstrip('\n').encode('utf-8')) > 1024:
			raise RuntimeError('output budget was not enforced: %r %r' % (oversized.stdout, oversized.stderr))

		with tempfile.TemporaryDirectory() as temporary:
			mismatched_trace = os.path.join(temporary, 'mismatch.api')
			shutil.copyfile(trace, mismatched_trace)
			mismatch = subprocess.run(
				[agent, '--service', '127.0.0.1:%d' % replay_port,
				 '--base-url', 'http://127.0.0.1:%d/v1' % model_port,
				 '--model', 'test-model', mismatched_trace, 'ask', 'This must not run.'],
				text=True,
				stdout=subprocess.PIPE,
				stderr=subprocess.PIPE,
				timeout=30,
			)
			mismatch_output = json.loads(mismatch.stdout)
			if mismatch.returncode == 0 or mismatch_output['status'] != 'error' or 'different filesystem objects' not in mismatch_output['conclusion']:
				raise RuntimeError('trace identity mismatch was accepted: %r %r' % (mismatch.stdout, mismatch.stderr))
		if AgentModelHandler.failure is not None:
			raise AgentModelHandler.failure

		truncated, truncated_output, truncated_commands = run_fake_replay_test(
			agent, trace, model_port, 'truncated')
		if (truncated.returncode != 0 or truncated_output['status'] != 'answered'
				or 'show instrumentation 0' not in truncated_commands):
			raise RuntimeError('oversized replay response test failed: %r %r' % (
				truncated.stdout, truncated.stderr))

		tool_stall, tool_stall_output, tool_stall_commands = run_fake_replay_test(
			agent, trace, model_port, 'tool_stall')
		if ('show instrumentation 0' not in tool_stall_commands
				or tool_stall_output['status'] not in ('budget_exhausted', 'unstable')):
			raise RuntimeError('tool-call deadline test failed: %r %r' % (
				tool_stall.stdout, tool_stall.stderr))

		validation_stall, validation_output, validation_commands = run_fake_replay_test(
			agent, trace, model_port, 'validation_stall')
		if (validation_stall.returncode == 0 or validation_output['status'] != 'error'
				or validation_commands != ['status']):
			raise RuntimeError('initial-validation deadline test failed: %r %r' % (
				validation_stall.stdout, validation_stall.stderr))

		final_stall, final_output, final_commands = run_fake_replay_test(
			agent, trace, model_port, 'final_stall')
		if (final_stall.returncode != 0 or final_output['status'] != 'unstable'
				or final_commands.count('status') != 2):
			raise RuntimeError('final-stability deadline test failed: %r %r' % (
				final_stall.stdout, final_stall.stderr))
		if AgentModelHandler.failure is not None:
			raise AgentModelHandler.failure
	finally:
		server.shutdown()
		server.server_close()
		server_thread.join()
		if replay.poll() is None:
			try:
				run_cli(cli, replay_port, 'stop')
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
