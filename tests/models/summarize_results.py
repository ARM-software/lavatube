#!/usr/bin/env python3
# Collects per-run results from results/*.json into results/results.csv.
#
# Mechanical columns (status, usage, timing, cost) are re-read from the result
# files on every run. Judgement columns (accepted, judge_notes, judged_at,
# judged_hash) are preserved across re-scans, keyed by result_file, so answers
# do not have to be re-judged. If a result file's content changes after it was
# judged, its accepted value is marked "stale" until re-judged.
#
# A "runset" is one experiment: the files it produced share a file-name prefix
# "<scenario>_<runset>" and each member file is one run. The run number is the
# trailing "_N" in the file name when present, otherwise 1. This keeps
# separately-produced runs of the same model (e.g. the pre-fix 8-round set and
# the post-fix n10 set) as distinct runsets instead of colliding.
#
# Usage:
#   summarize_results.py [-d DIR]              rescan and rewrite results.csv
#   summarize_results.py [-d DIR] judge <file> <pass|fail> [note ...]
#                                              record a judgement, then rescan

import argparse
import csv
import datetime
import glob
import hashlib
import json
import os
import re
import sys

REPO_DIR = os.path.dirname(os.path.dirname(os.path.dirname(
	os.path.abspath(__file__))))
DEFAULT_RESULTS_DIR = os.path.join(REPO_DIR, 'results')
MODELS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'models.json')

sys.path.insert(0, os.path.join(REPO_DIR, 'external', 'tracetooltests', 'scripts'))
import vulkan_spec

FIELDNAMES = [
	'scenario', 'model', 'model_id', 'provider', 'runset', 'run', 'status',
	'confidence', 'rounds', 'tool_calls', 'bad_tools', 'bad_apis',
	'agent_seconds', 'wall_seconds',
	'input_tokens', 'output_tokens', 'cached_tokens', 'cost_usd',
	'accepted', 'judge_notes', 'judged_at', 'judged_hash', 'result_file',
]

VALID_TOOLS = {
	'trace_get_metadata',
	'trace_list_threads',
	'trace_get_thread',
	'trace_get_frame',
	'trace_list_objects',
	'trace_get_object',
	'trace_get_packets',
	'trace_list_calls',
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

RAW_TOOL_RE = re.compile(r'<function=([A-Za-z0-9_]+)>|\[TOOL_CALLS\]([A-Za-z0-9_]+)')
VK_TOKEN_RE = re.compile(
	r'(?<![A-Za-z0-9_*/.\-])'
	r'(vk[A-Z][A-Za-z0-9_]+|Vk[A-Z][A-Za-z0-9_]+|VK_[A-Z0-9]+_[A-Z0-9_]+)'
	r'(?![A-Za-z0-9_*/.\-\u2026])'
)
VK_SHORTHAND_PREFIXES = {
	'vkCmd', 'vkCmdBind', 'vkCmdSet', 'vkBegin', 'vkEnd', 'VK_KHR', 'VK_EXT',
}

RUN_NUM = re.compile(r'_(\d+)$')


def load_vulkan_names():
	vulkan_spec.init()
	root = vulkan_spec.root
	funcs = set(vulkan_spec.functions) | set(vulkan_spec.valid_functions)
	for cmd in root.findall('commands/command'):
		if 'name' in cmd.attrib:
			funcs.add(cmd.attrib['name'])
		proto = cmd.find('proto/name')
		if proto is not None and proto.text:
			funcs.add(proto.text)
	types = set(vulkan_spec.types) | set(vulkan_spec.all_handles) | set(vulkan_spec.structures)
	for tp in root.findall('types/type'):
		if 'name' in tp.attrib:
			types.add(tp.attrib['name'])
		nm = tp.find('name')
		if nm is not None and nm.text:
			types.add(nm.text)
	enums = set(vulkan_spec.enums)
	for en in root.findall('.//enum'):
		if 'name' in en.attrib:
			enums.add(en.attrib['name'])
	for ext in root.findall('extensions/extension'):
		if 'name' in ext.attrib:
			enums.add(ext.attrib['name'])
	return {'funcs': funcs, 'types': types, 'enums': enums}


def check_tool_call(tool_name, args, vk_names, bad_tools, bad_apis):
	if tool_name not in VALID_TOOLS:
		bad_tools.append(str(tool_name))
	if not isinstance(args, dict):
		return
	if 'name' in args and tool_name in ('trace_find_calls', 'trace_list_calls'):
		cname = args['name']
		if not isinstance(cname, str) or cname not in vk_names['funcs']:
			bad_apis.append(str(cname))
	if 'type' in args and tool_name in ('trace_get_object', 'replay_get_object_state'):
		otype = args['type']
		if not isinstance(otype, str) or otype not in vk_names['types']:
			bad_apis.append(str(otype))


def check_hallucinations(path, doc, vk_names):
	bad_tools = []
	bad_apis = []
	debug_path = path + '.debug.jsonl'
	if os.path.exists(debug_path):
		try:
			with open(debug_path, encoding='utf-8') as f:
				for line in f:
					line = line.strip()
					if not line:
						continue
					event = json.loads(line)
					etype = event.get('type')
					val = event.get('value') or {}
					if etype == 'tool_call':
						check_tool_call(val.get('tool_name', ''),
							val.get('arguments'), vk_names, bad_tools, bad_apis)
					elif etype == 'model_response':
						choices = val.get('choices') or []
						msg = choices[0].get('message', {}) if choices else {}
						content = msg.get('content') or ''
						if isinstance(content, str):
							for m in RAW_TOOL_RE.finditer(content):
								fn = m.group(1) or m.group(2)
								if fn not in VALID_TOOLS:
									bad_tools.append(fn)
		except (OSError, json.JSONDecodeError):
			pass
	else:
		for item in doc.get('evidence', []):
			if isinstance(item, dict) and 'tool_name' in item:
				check_tool_call(item.get('tool_name', ''),
					item.get('arguments'), vk_names, bad_tools, bad_apis)

	texts = []
	if isinstance(doc.get('conclusion'), str):
		texts.append(doc['conclusion'])
	for item in doc.get('evidence', []):
		if isinstance(item, dict) and isinstance(item.get('inference'), str):
			texts.append(item['inference'])
	for item in doc.get('unresolved', []):
		if isinstance(item, str):
			texts.append(item)
	seen_text_apis = set(bad_apis)
	for m in VK_TOKEN_RE.finditer(' '.join(texts)):
		tok = m.group(1)
		if tok in VK_SHORTHAND_PREFIXES or tok in seen_text_apis:
			continue
		if tok.startswith('vk'):
			if tok in vk_names['funcs']:
				continue
			if tok.endswith('s') and tok[:-1] in vk_names['funcs']:
				continue
			bad_apis.append(tok)
			seen_text_apis.add(tok)
		elif tok.startswith('Vk'):
			if tok in vk_names['types']:
				continue
			if tok.endswith('s') and tok[:-1] in vk_names['types']:
				continue
			bad_apis.append(tok)
			seen_text_apis.add(tok)
		elif tok.startswith('VK_'):
			if tok in vk_names['enums']:
				continue
			bad_apis.append(tok)
			seen_text_apis.add(tok)
	return bad_tools, bad_apis


def provider_for(model_id, models):
	for m in models:
		if m.get('model_id') == model_id:
			return m.get('provider', '')
	# Known rename in place: the 16K Ollama variants replaced the plain ids.
	if model_id.endswith('-16k'):
		return provider_for(model_id[:-4], models)
	return ''


def sha1_of(path):
	h = hashlib.sha1()
	with open(path, 'rb') as f:
		for chunk in iter(lambda: f.read(65536), b''):
			h.update(chunk)
	return h.hexdigest()[:12]


def split_runset(stem, scenario):
	# stem is the file name without .json; split off "<scenario>_" and the
	# trailing "_<N>" run number. Returns (runset, run_or_None).
	prefix = scenario + '_'
	rest = stem[len(prefix):] if stem.startswith(prefix) else stem
	m = RUN_NUM.search(rest)
	if m:
		return rest[:m.start()], int(m.group(1))
	return rest, None


def result_key_for(results_dir, filename):
	dir_label = os.path.basename(os.path.normpath(results_dir)) or 'results'
	return dir_label + '/' + os.path.basename(filename)


def parse_file(path, models, vk_names, results_dir):
	base = os.path.basename(path)
	doc = json.load(open(path))
	runner = doc.get('runner', {})
	scenario = runner.get('scenario', '')
	model_id = runner.get('model_id', '')
	model = runner.get('model', '')
	if not scenario:
		# Fall back to the file name: "<scenario>_<runset>[_N].json".
		scenario = base.split('_', 1)[0]
	runset, run = split_runset(base[:-5], scenario)
	usage = doc.get('usage', {})
	bad_tools, bad_apis = check_hallucinations(path, doc, vk_names)
	row = {
		'scenario': scenario,
		'model': model,
		'model_id': model_id,
		'provider': provider_for(model_id, models) if model_id else '',
		'runset': runset,
		'run': run,
		'status': doc.get('status', 'error'),
		'confidence': '',
		'rounds': '',
		'tool_calls': '',
		'bad_tools': ';'.join(bad_tools),
		'bad_apis': ';'.join(bad_apis),
		'agent_seconds': '',
		'wall_seconds': '',
		'input_tokens': '',
		'output_tokens': '',
		'cached_tokens': '',
		'cost_usd': '',
		'result_file': result_key_for(results_dir, base),
	}
	if 'confidence' in doc:
		row['confidence'] = doc['confidence']
	for src, dst in (('rounds', 'rounds'), ('calls', 'tool_calls'),
			('input_tokens', 'input_tokens'), ('output_tokens', 'output_tokens'),
			('cached_tokens', 'cached_tokens'), ('cost_usd', 'cost_usd')):
		if src in usage:
			row[dst] = usage[src]
	for src, dst in (('agent_seconds', 'agent_seconds'),
			('wall_clock_seconds', 'wall_seconds')):
		if src in runner:
			row[dst] = runner[src]
	row['_bad_tools'] = bad_tools
	row['_bad_apis'] = bad_apis
	row['_path'] = path
	row['_mtime'] = os.path.getmtime(path)
	row['_hash'] = sha1_of(path)
	return row


def assign_runs(rows):
	# Within each (scenario, runset) group, number the runs. Files without a
	# trailing _N take the smallest free run number, in file-time order.
	by_key = {}
	for row in rows:
		by_key.setdefault((row['scenario'], row['runset']), []).append(row)
	for group in by_key.values():
		used = {r['run'] for r in group if r['run'] is not None}
		unnum = sorted((r for r in group if r['run'] is None),
				key=lambda r: (r['_mtime'], r['_path']))
		n = 1
		for r in unnum:
			while n in used:
				n += 1
			r['run'] = n
			used.add(n)
	return rows


def load_judgements(csv_file):
	judged = {}
	if not os.path.exists(csv_file):
		return judged
	with open(csv_file, newline='') as f:
		for row in csv.DictReader(f):
			rf = row.get('result_file')
			if rf:
				judged[rf] = row
				judged[os.path.basename(rf)] = row
	return judged


def write_csv(rows, csv_file):
	rows = sorted(rows, key=lambda r: (
		r['scenario'], r['model'].lower(), r['runset'], int(r['run'])))
	with open(csv_file, 'w', newline='') as f:
		w = csv.DictWriter(f, fieldnames=FIELDNAMES)
		w.writeheader()
		for r in rows:
			row = {k: r.get(k, '') for k in FIELDNAMES}
			w.writerow(row)
	return rows


def rescan(models, results_dir):
	csv_file = os.path.join(results_dir, 'results.csv')
	vk_names = load_vulkan_names()
	rows = []
	for path in sorted(glob.glob(os.path.join(results_dir, '*.json'))):
		base = os.path.basename(path)
		if base == 'results.json' or 'debug' in base:
			continue
		try:
			rows.append(parse_file(path, models, vk_names, results_dir))
		except (ValueError, KeyError, OSError) as e:
			print('skipping %s: %s' % (base, e), file=sys.stderr)
	rows = assign_runs(rows)
	judged = load_judgements(csv_file)
	stale = 0
	for r in rows:
		key = r['result_file']
		old = judged.get(key) or judged.get(os.path.basename(key))
		if not old:
			r['accepted'] = ''
			r['judge_notes'] = ''
			r['judged_at'] = ''
			r['judged_hash'] = ''
			continue
		r['accepted'] = old.get('accepted', '')
		r['judge_notes'] = old.get('judge_notes', '')
		r['judged_at'] = old.get('judged_at', '')
		r['judged_hash'] = old.get('judged_hash', '')
		if r['accepted'] in ('pass', 'fail') and r['judged_hash'] != r['_hash']:
			r['accepted'] = 'stale'
			stale += 1
	write_csv(rows, csv_file)
	return rows, stale


def print_table(rows):
	print('%-14s %-22s %-18s %-3s %-8s %-4s %-5s %-4s %-4s %-7s %-8s %-6s' % (
		'scenario', 'model', 'runset', 'run', 'status', 'rnd', 'calls',
		'badt', 'bada', 'seconds', 'cost$', 'accepted'))
	flagged = []
	for r in rows:
		bad_t = len(r.get('_bad_tools', []))
		bad_a = len(r.get('_bad_apis', []))
		if bad_t or bad_a:
			flagged.append(r)
		print('%-14s %-22s %-18s %-3s %-8s %-4s %-5s %-4s %-4s %-7s %-8s %-6s' % (
			r['scenario'], r['model'][:22], r['runset'][:18], r['run'],
			r['status'][:8], r['rounds'] or '-', r['tool_calls'] or '-',
			bad_t or '-', bad_a or '-',
			('%.0f' % r['agent_seconds']) if r['agent_seconds'] != '' else '-',
			('$%.4f' % r['cost_usd']) if r['cost_usd'] not in ('', 0.0) else '$0',
			r['accepted'] or '-'))
	if flagged:
		print('\nhallucinated tools / Vulkan APIs:')
		for r in flagged:
			parts = []
			if r['_bad_tools']:
				parts.append('tools=%s' % ', '.join(r['_bad_tools']))
			if r['_bad_apis']:
				parts.append('apis=%s' % ', '.join(r['_bad_apis']))
			print('  %-52s %s' % (r['result_file'], ' | '.join(parts)))


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('-d', '--dir', '--results-dir', dest='results_dir',
		default=DEFAULT_RESULTS_DIR,
		help='directory containing result JSON files (default: %(default)s)')
	sub = parser.add_subparsers(dest='command')
	jp = sub.add_parser('judge', help='record a judgement for a result file')
	jp.add_argument('result_file')
	jp.add_argument('verdict', choices=['pass', 'fail'])
	jp.add_argument('note', nargs='*', default=[''])
	args = parser.parse_args()

	results_dir = os.path.abspath(args.results_dir)
	csv_file = os.path.join(results_dir, 'results.csv')

	with open(MODELS_FILE) as f:
		models = json.load(f)['models']

	if args.command == 'judge':
		target = os.path.join(results_dir, os.path.basename(args.result_file))
		if not os.path.exists(target):
			target = os.path.join(results_dir, args.result_file.lstrip('/'))
		if not os.path.exists(target):
			print('no such result file: %s' % args.result_file, file=sys.stderr)
			return 2
		rows, _ = rescan(models, results_dir)
		key = result_key_for(results_dir, target)
		for r in rows:
			if r['result_file'] == key:
				r['accepted'] = args.verdict
				r['judge_notes'] = ' '.join(args.note)
				r['judged_at'] = datetime.date.today().isoformat()
				r['judged_hash'] = r['_hash']
				break
		else:
			print('result file not in scan: %s' % key, file=sys.stderr)
			return 2
		write_csv(rows, csv_file)
		print('judged %s: %s' % (key, args.verdict))
		return 0

	rows, stale = rescan(models, results_dir)
	print_table(rows)
	if stale:
		print('%d judgement(s) are stale (result file changed); re-judge them.'
				% stale)
	return 0


if __name__ == '__main__':
	sys.exit(main())
