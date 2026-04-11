#!/bin/sh
# Run a command only if a required file exists, otherwise exit 77 (CTest skip code).
# Usage: run_if_exists.sh <required-file> <command> [args...]
if [ ! -f "$1" ]; then exit 77; fi
shift
exec "$@"
