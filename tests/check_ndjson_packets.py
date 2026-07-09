#!/usr/bin/env python3

import json
import sys


def fail(message):
	print(message, file=sys.stderr)
	return 1


def main():
	if len(sys.argv) < 2:
		return fail("usage: check_ndjson_packets.py <path> [expected-frame] [--expect-frame N] [--expect-index N] [--expect-thread N] [--expect-count N]")

	path = sys.argv[1]
	expected_frame = None
	expected_index = None
	expected_thread = None
	expected_count = None
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
			count += 1

	if expected_count is not None and count != expected_count:
		return fail(f"{path}: expected {expected_count} packets printed, got {count}")
	if expected_count is None and count == 0:
		return fail(f"{path}: no packets printed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
