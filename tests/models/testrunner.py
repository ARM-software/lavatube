#!/usr/bin/env python3
"""Run one model-sweep scenario against one model and dump the raw result.

Usage: testrunner.py <scenario name> <model name> <output file>

The scenario name is a key in scenarios.json (e.g. 'mipmap') and the model
name is a model_id in models.json (e.g. 'ministral-3b-2512').

Inputs (next to this script):
* models.json    - the model matrix (see tests/models/models.json)
* scenarios.json - scenario name -> {trace_file, question, setup}

Run from the repo root. The scenario's setup steps are lava-cli command
token lists (a plain string is split on whitespace), for example:
	"setup": [ ["goto", "0", "vkQueueSubmit"], ["step", "0", "packets", "5"] ]

The runner starts `lava-replay --service` for the scenario's trace in a
background thread, waits until it is paused, runs the setup steps, runs
`lava-agent` against that service with the selected model, writes the
agent's JSON result to the output file, and stops the replay service.
The agent's debug transcript is written next to the output file as
'<output file>.debug.jsonl' (one JSON event per line: model_request,
model_response, tool_call, model_error); for budget_exhausted results it
shows what the model was doing in its final rounds, which helps decide
whether more budget or a higher reasoning level would have helped.
Reasoning level is ignored for now (no --reasoning-effort is passed).

Exit codes: 0 = agent finished (check the result's status field),
1 = agent finished with an error result, 2 = runner-side failure.
On runner-side failure an error envelope is still written to the output
file so the failure is visible to the CSV processing step.
"""

import collections
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time


HERE = os.path.dirname(os.path.abspath(__file__))
MODELS_FILE = os.path.join(HERE, 'models.json')
SCENARIOS_FILE = os.path.join(HERE, 'scenarios.json')
BUILD_DIR = os.environ.get('LAVATUBE_BUILD_DIR', 'build')
REPLAY_WAIT_SECONDS = 60
SETUP_TIMEOUT_SECONDS = 120
AGENT_TIMEOUT_SECONDS = 900
REPLAY_LOG_CAP = 100000
# The agent's default 32 KiB result cap is too small for evidence-rich answers
# (evidence entries embed full tool results), which demotes correct answers to
# budget_exhausted. Raise it so status reflects answer quality, not size.
MAX_OUTPUT_BYTES = 262144


def usage_error(message):
	sys.stderr.write('testrunner: %s\n' % message)
	sys.stderr.write('usage: testrunner.py <scenario name> <model name> <output file>\n')
	sys.exit(2)


def load_json(path):
	with open(path, encoding='utf-8') as file:
		return json.load(file)


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
		timeout=SETUP_TIMEOUT_SECONDS,
	)


def drain_replay(replay_process, output):
	total_bytes = 0
	for line in replay_process.stdout:
		output.append(line)
		total_bytes += len(line)
		while output and total_bytes > REPLAY_LOG_CAP:
			total_bytes -= len(output.popleft())


def replay_tail(replay_output):
	text = ''.join(replay_output)
	return text[-8000:] if text else '(no output)'


def wait_for_paused(cli, port, replay_process, replay_output):
	deadline = time.monotonic() + REPLAY_WAIT_SECONDS
	while time.monotonic() < deadline:
		if replay_process.poll() is not None:
			raise RuntimeError('lava-replay exited early:\n' + replay_tail(replay_output))
		try:
			status = run_cli(cli, port, 'status')
		except subprocess.TimeoutExpired:
			status = None
		if status is not None and status.returncode == 0 and status.stdout.startswith('PAUSED'):
			return
		time.sleep(0.1)
	raise RuntimeError('timed out waiting for replay service:\n' + replay_tail(replay_output))


def run_setup_steps(cli, port, steps):
	for index, step in enumerate(steps):
		if isinstance(step, str):
			argv = step.split()
		else:
			argv = [str(item) for item in step]
		if not argv:
			raise RuntimeError('setup step %d is empty: %r' % (index, step))
		try:
			result = run_cli(cli, port, *argv)
		except subprocess.TimeoutExpired:
			raise RuntimeError('setup step %d timed out: %r' % (index, argv))
		if result.returncode != 0:
			raise RuntimeError('setup step %d failed: %r\nstdout: %s\nstderr: %s' % (
				index, argv, result.stdout, result.stderr))


def stop_replay(cli, port, replay_process):
	try:
		run_cli(cli, port, 'stop')
	except (subprocess.TimeoutExpired, OSError):
		pass
	try:
		replay_process.wait(timeout=10)
	except subprocess.TimeoutExpired:
		replay_process.kill()
		try:
			replay_process.wait(timeout=10)
		except subprocess.TimeoutExpired:
			pass


def api_key_for(model, providers):
	provider = providers.get(model['provider'].casefold())
	if provider is None:
		raise RuntimeError('no provider entry for %r' % model['provider'])
	match = re.search(r'\$([A-Za-z_][A-Za-z0-9_]*)', provider.get('auth', ''))
	if not match:
		raise RuntimeError('cannot find API key variable in provider auth: %r' % provider.get('auth'))
	name = match.group(1)
	value = os.environ.get(name)
	if not value:
		raise RuntimeError('environment variable %s is not set' % name)
	return value


def run_agent(agent, port, model, trace, question, providers, debug_file):
	api_model = model['model_id']
	command = [
		agent,
		'--service', '127.0.0.1:%d' % port,
		'--base-url', model['base_url'],
		'--model', api_model,
		'--api-key', api_key_for(model, providers),
		'--max-output-bytes', str(MAX_OUTPUT_BYTES),
		'--evidence-mode', 'digest',
		'-df', debug_file,
		trace,
		'ask',
		question,
	]
	try:
		proc = subprocess.run(
			command,
			text=True,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			timeout=AGENT_TIMEOUT_SECONDS,
		)
	except subprocess.TimeoutExpired:
		raise RuntimeError('lava-agent timed out after %d seconds' % AGENT_TIMEOUT_SECONDS)
	lines = [line for line in proc.stdout.splitlines() if line.strip()]
	if len(lines) != 1:
		raise RuntimeError('lava-agent stdout was not one JSON line:\n%s\nstderr: %s' % (
			proc.stdout[-8000:], proc.stderr[-4000:]))
	try:
		return json.loads(lines[0]), proc.returncode
	except json.JSONDecodeError:
		raise RuntimeError('lava-agent output is not valid JSON:\n%s' % lines[0][-8000:])


def print_debug_digest(debug_file):
	"""Summarize a budget-exhausted transcript: what the model did each round."""
	rounds = []
	current = None
	try:
		with open(debug_file, encoding='utf-8') as file:
			for line in file:
				line = line.strip()
				if not line:
					continue
				event = json.loads(line)
				if event.get('type') == 'model_response':
					value = event.get('value') or {}
					choices = value.get('choices') or []
					choice = choices[0] if choices else {}
					message = choice.get('message') or {}
					tool_calls = message.get('tool_calls') or []
					current = {
						'finish_reason': choice.get('finish_reason'),
						'calls': [],
						'failed': 0,
						'final': not tool_calls,
						'content': (message.get('content') or '')[:200],
					}
					rounds.append(current)
				elif event.get('type') == 'tool_call' and current is not None:
					value = event.get('value') or {}
					output = value.get('output') or {}
					result = output.get('result') or {}
					failed = not output.get('ok', True) or 'error' in (result if isinstance(result, dict) else {})
					current['calls'].append((value.get('tool_name') or '?',
						json.dumps(value.get('arguments') or {}, sort_keys=True), failed))
					current['failed'] += 1 if failed else 0
	except (OSError, json.JSONDecodeError):
		return
	if not rounds:
		return
	print('debug digest (%d model rounds, %s):' % (len(rounds), debug_file))
	for index, round_ in enumerate(rounds, 1):
		if round_['final']:
			note = 'final answer'
			if round_['finish_reason'] == 'length':
				note += ' [TRUNCATED: finish_reason=length]'
			print('  round %d: %s' % (index, note))
			if round_['content']:
				print('    content: %s' % round_['content'].replace('\n', ' '))
		else:
			parts = []
			for name, args, failed in round_['calls']:
				part = name + '(' + args + ')'
				if len(part) > 70:
					part = part[:67] + '...'
				parts.append(part + (' [FAILED]' if failed else ''))
			print('  round %d: tools: %s' % (index, ', '.join(parts) or '(none)'))
	total_calls = sum(len(round_['calls']) for round_ in rounds)
	failed_calls = sum(round_['failed'] for round_ in rounds)
	distinct_calls = set((name, args) for round_ in rounds for name, args, _ in round_['calls'])
	repeats = total_calls - len(distinct_calls)
	last = rounds[-1]
	if last['final']:
		print('  last round already emitted a final answer -> one more round likely finishes it (near miss)')
	elif repeats > 0 and failed_calls == 0:
		print('  %d repeated identical tool call(s) -> circling; more budget may not help, try reasoning level or a stronger model' % repeats)
	elif failed_calls >= max(1, total_calls // 2):
		print('  %d of %d tool calls failed -> likely bad arguments or unknown API names; more rounds alone may not help, try a reasoning level' % (failed_calls, total_calls))
	else:
		print('  still gathering working evidence each round -> more budget or a reasoning level may help')


def error_envelope(message, context):
	output = {
		'schema_version': 1,
		'status': 'error',
		'conclusion': 'testrunner failed: ' + message,
		'confidence': 0.0,
		'evidence': [],
		'unresolved': [message],
		'usage': {'rounds': 0, 'calls': 0},
	}
	output['runner'] = context
	return output


def apply_cost(result, model):
	usage = result.get('usage')
	price = model.get('pricing_per_mtok_usd')
	if not isinstance(usage, dict) or not isinstance(price, dict):
		return
	input_tokens = usage.get('input_tokens')
	output_tokens = usage.get('output_tokens')
	if input_tokens is None or output_tokens is None:
		return
	cached = usage.get('cached_tokens') or 0
	billed = max(0, input_tokens - cached)
	input_price = price.get('input') or 0.0
	cached_price = price.get('cached') or 0.0
	output_price = price.get('output') or 0.0
	usage['cost_usd'] = round(
		(billed * input_price + cached * cached_price + output_tokens * output_price) / 1e6, 6)


def write_output(output_file, result):
	with open(output_file, 'w', encoding='utf-8') as file:
		json.dump(result, file, indent=2)
		file.write('\n')


def main(argv=None):
	if argv is None:
		argv = sys.argv[1:]
	if argv and argv[0] in ('-h', '--help'):
		print(__doc__)
		return 0
	if len(argv) != 3:
		usage_error('expected 3 arguments')
	output_file = argv[2]
	run_started = time.monotonic()

	models_doc = load_json(MODELS_FILE)
	scenarios = load_json(SCENARIOS_FILE)
	if argv[0] not in scenarios:
		usage_error('unknown scenario %r; available: %s' % (argv[0], ', '.join(scenarios.keys())))
	scenario_name = argv[0]
	scenario = scenarios[scenario_name]
	models = models_doc['models']
	model = next((item for item in models if item['model_id'] == argv[1]), None)
	if model is None:
		usage_error('unknown model %r; available: %s' % (
			argv[1], ', '.join(item['model_id'] for item in models)))
	if model.get('type') != 'chat':
		usage_error('model %s (%s) is not a chat model' % (model['model_id'], model['name']))
	if model.get('available') is False:
		usage_error('model %s (%s) is not offered by the current %s catalog; '
			'its model_id is unverified against the live API' % (model['model_id'], model['name'], model['provider']))
	trace = scenario['trace_file']
	if not os.path.exists(trace):
		usage_error('trace file not found: %s (run from the repo root)' % trace)

	replay = os.path.join(BUILD_DIR, 'lava-replay')
	agent = os.path.join(BUILD_DIR, 'lava-agent')
	cli = os.path.join(BUILD_DIR, 'lava-cli')
	providers = models_doc.get('providers', {})
	context = {
		'scenario': scenario_name,
		'model': model['name'],
		'model_id': model['model_id'],
	}
	debug_file = output_file + '.debug.jsonl'

	port = reserve_port()
	replay_output = collections.deque()
	replay_process = subprocess.Popen(
		[replay, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', '-B', trace],
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)
	replay_thread = threading.Thread(target=drain_replay, args=(replay_process, replay_output))
	replay_thread.start()

	agent_rc = 1
	agent_seconds = 0.0
	try:
		wait_for_paused(cli, port, replay_process, replay_output)
		run_setup_steps(cli, port, scenario.get('setup', []))
		agent_started = time.monotonic()
		result, agent_rc = run_agent(agent, port, model, trace, scenario['question'], providers, debug_file)
		agent_seconds = time.monotonic() - agent_started
	except RuntimeError as error:
		result = error_envelope(str(error), context)
		result['runner']['debug_file'] = debug_file
		result['runner']['wall_clock_seconds'] = round(time.monotonic() - run_started, 1)
		result['runner']['replay_output'] = replay_tail(replay_output)
		write_output(output_file, result)
		sys.stderr.write('testrunner: %s\n' % error)
		return 2
	finally:
		stop_replay(cli, port, replay_process)
		replay_thread.join(timeout=10)

	result['runner'] = context
	result['runner']['agent_seconds'] = round(agent_seconds, 1)
	result['runner']['wall_clock_seconds'] = round(time.monotonic() - run_started, 1)
	result['runner']['debug_file'] = debug_file
	apply_cost(result, model)
	write_output(output_file, result)
	if result.get('status') == 'budget_exhausted':
		print_debug_digest(debug_file)
	print('%s x %s: %s (wall %.1fs, agent %.1fs) -> %s (debug: %s)' % (
		scenario_name, model['name'], result.get('status'),
		result['runner']['wall_clock_seconds'], result['runner']['agent_seconds'], output_file, debug_file))
	return agent_rc


if __name__ == '__main__':
	sys.exit(main())
