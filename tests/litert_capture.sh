#!/bin/sh
# Keep skipped captures from leaving a partial trace for the replay fixture.
trace=$1
layer=$2
shift 2
rm -f "$trace"
LD_PRELOAD="$layer${LD_PRELOAD:+:$LD_PRELOAD}" "$@"
result=$?
if [ "$result" -eq 77 ]; then
	rm -f "$trace"
elif [ "$result" -eq 0 ] && [ ! -s "$trace" ]; then
	echo "LiteRT-LM capture succeeded without producing a trace: $trace" >&2
	exit 1
fi
exit "$result"
