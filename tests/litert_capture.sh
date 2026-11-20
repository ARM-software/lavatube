#!/bin/sh
# Keep skipped captures from leaving a partial trace for the replay fixture.
# An empty layer argument lets a capture launcher configure its own interception.
trace=$1
layer=$2
shift 2
rm -f "$trace"
if [ -n "$layer" ]; then
	export LD_PRELOAD="$layer${LD_PRELOAD:+:$LD_PRELOAD}"
fi
"$@"
result=$?
if [ "$result" -eq 77 ]; then
	rm -f "$trace"
elif [ "$result" -eq 0 ] && [ ! -s "$trace" ]; then
	echo "LiteRT-LM capture succeeded without producing a trace: $trace" >&2
	exit 1
fi
exit "$result"
