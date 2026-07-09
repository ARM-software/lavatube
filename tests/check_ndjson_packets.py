#!/usr/bin/env python3

import json
import sys


def fail(message):
	print(message, file=sys.stderr)
	return 1


def main():
	if len(sys.argv) < 2:
		return fail("usage: check_ndjson_packets.py <path> [expected-frame] [--expect-frame N] [--expect-index N] [--expect-thread N] [--expect-count N] [--expect-pairs INDEX:THREAD,...] [--expect-thread-barrier-params true]")

	path = sys.argv[1]
	expected_frame = None
	expected_index = None
	expected_thread = None
	expected_count = None
	expected_pairs = None
	expected_thread_barrier_params = False
	seen_pairs = set()
	seen_thread_barrier = False
	args = sys.argv[2:]
	if args and not args[0].startswith("--"):
		expected_frame = int(args[0])
		args = args[1:]
	while args:
		if len(args) < 2:
			return fail("missing value for " + args[0])
		if args[0] == "--expect-frame":
			expected_frame = int(args[1])
		elif args[0] == "--expect-index":
			expected_index = int(args[1])
		elif args[0] == "--expect-thread":
			expected_thread = int(args[1])
		elif args[0] == "--expect-count":
			expected_count = int(args[1])
		elif args[0] == "--expect-pairs":
			expected_pairs = set(args[1].split(","))
		elif args[0] == "--expect-thread-barrier-params":
			expected_thread_barrier_params = args[1].lower() in ("1", "true", "yes", "on")
		else:
			return fail("unknown option " + args[0])
		args = args[2:]
	count = 0
	with open(path, "r", encoding="utf-8") as f:
		for lineno, line in enumerate(f, 1):
			line = line.rstrip("\n")
			if not line:
				return fail(f"{path}:{lineno}: empty line")
			try:
				packet = json.loads(line)
			except json.JSONDecodeError as e:
				return fail(f"{path}:{lineno}: invalid JSON: {e}")
			for key in ("index", "name", "thread", "frame", "parameters"):
				if key not in packet:
					return fail(f"{path}:{lineno}: missing {key}")
			if expected_frame is not None and packet["frame"] != expected_frame:
				return fail(f"{path}:{lineno}: expected frame {expected_frame}, got {packet['frame']}")
			if expected_index is not None and packet["index"] != expected_index:
				return fail(f"{path}:{lineno}: expected index {expected_index}, got {packet['index']}")
			if expected_thread is not None and packet["thread"] != expected_thread:
				return fail(f"{path}:{lineno}: expected thread {expected_thread}, got {packet['thread']}")
			pair = f"{packet['index']}:{packet['thread']}"
			if expected_pairs is not None and pair not in expected_pairs:
				return fail(f"{path}:{lineno}: unexpected packet/thread pair {pair}")
			seen_pairs.add(pair)
			if expected_thread_barrier_params and packet["name"] == "thread_barrier":
				seen_thread_barrier = True
				params = packet["parameters"]
				if "TODO" in params:
					return fail(f"{path}:{lineno}: thread_barrier still has TODO parameters")
				if not isinstance(params.get("thread_count"), int):
					return fail(f"{path}:{lineno}: thread_barrier missing integer thread_count")
				if not isinstance(params.get("targets"), list):
					return fail(f"{path}:{lineno}: thread_barrier missing targets array")
				for target in params["targets"]:
					for key in ("thread", "packet_index", "waited"):
						if key not in target:
							return fail(f"{path}:{lineno}: thread_barrier target missing {key}")
			count += 1

	if expected_thread_barrier_params and not seen_thread_barrier:
		return fail(f"{path}: no thread_barrier packet found")
	if expected_pairs is not None and seen_pairs != expected_pairs:
		return fail(f"{path}: expected packet/thread pairs {sorted(expected_pairs)}, got {sorted(seen_pairs)}")
	if expected_pairs is not None and count != len(expected_pairs):
		return fail(f"{path}: expected {len(expected_pairs)} packets printed, got {count}")
	if expected_count is not None and count != expected_count:
		return fail(f"{path}: expected {expected_count} packets printed, got {count}")
	if expected_count is None and count == 0:
		return fail(f"{path}: no packets printed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
