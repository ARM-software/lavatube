#!/bin/sh
# Run a GPU model test only when explicitly enabled, otherwise exit 77 (CTest skip code).
# Usage: LAVATUBE_MODEL_TESTS=1 run_model_test.sh <command> [args...]
if [ "${LAVATUBE_MODEL_TESTS:-0}" != 1 ]; then exit 77; fi
exec "$@"
