#!/usr/bin/env python3

import json
import sys


def fail(message):
	print(message, file=sys.stderr)
	return 1


def main():
	if len(sys.argv) not in (2, 3):
		return fail("usage: check_ndjson_packets.py <path> [expected-frame]")

	path = sys.argv[1]
	expected_frame = int(sys.argv[2]) if len(sys.argv) == 3 else None
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
			for key in ("name", "thread", "frame", "parameters"):
				if key not in packet:
					return fail(f"{path}:{lineno}: missing {key}")
			if expected_frame is not None and packet["frame"] != expected_frame:
				return fail(f"{path}:{lineno}: expected frame {expected_frame}, got {packet['frame']}")
			count += 1

	if count == 0:
		return fail(f"{path}: no packets printed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
