#!/bin/sh
# Remove a stale output path before running a command.
# Usage: remove_then_run.sh <path-to-remove> <command> [args...]
rm -f "$1"
shift
exec "$@"
