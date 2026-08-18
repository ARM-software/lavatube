#!/bin/sh

if [ "${LAVATUBE_ANDROID_REPLAY_TESTS:-0}" != 1 ]; then exit 77; fi
exec "$@"
