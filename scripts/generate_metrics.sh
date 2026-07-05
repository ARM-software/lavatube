#!/bin/bash

percentage()
{
	local part=$1
	local total=$2

	if [ "$total" -eq 0 ]; then
		echo "0.00"
		return
	fi

	awk -v part="$part" -v total="$total" 'BEGIN { printf "%.2f", (part / total) * 100 }'
}

count_lines()
{
	local file=$1

	awk 'END { print NR }' "$file"
}

require_command()
{
	local command_name=$1

	if ! command -v "$command_name" > /dev/null 2>&1; then
		echo "Missing required command: $command_name" >&2
		exit 1
	fi
}

require_readable_file()
{
	local file=$1

	if [ ! -r "$file" ]; then
		echo "Missing readable file: $file" >&2
		exit 1
	fi
}

require_directory()
{
	local directory=$1

	if [ ! -d "$directory" ]; then
		echo "Missing directory: $directory" >&2
		exit 1
	fi
}

require_executable_file()
{
	local file=$1

	if [ ! -x "$file" ]; then
		echo "Missing executable file: $file" >&2
		exit 1
	fi
}

require_positive_integer()
{
	local value=$1
	local label=$2

	case "$value" in
		''|*[!0-9]*)
			echo "$label must be a positive integer, got: $value" >&2
			exit 1
			;;
	esac

	if [ "$value" -le 0 ]; then
		echo "$label must be greater than zero, got: $value" >&2
		exit 1
	fi
}

require_sample_count()
{
	local file=$1
	local expected=$2
	local label=$3
	local actual

	actual=$(count_lines "$file")
	if [ "$actual" -ne "$expected" ]; then
		echo "Expected $expected $label samples, got $actual" >&2
		exit 1
	fi
}

calculate_stats()
{
	local file=$1
	local precision=$2

	sort -n "$file" | awk -v precision="$precision" '
		{
			values[++count] = $1
			sum += $1
			if (count == 1 || $1 < best)
			{
				best = $1
			}
		}
		END {
			if (count == 0)
			{
				exit 1
			}

			if (count % 2 == 1)
			{
				median = values[(count + 1) / 2]
			}
			else
			{
				median = (values[count / 2] + values[(count / 2) + 1]) / 2
			}

			average = sum / count
			printf "%.*f / %.*f / %.*f", precision, median, precision, average, precision, best
		}'
}

verify_perf_output()
{
	local file=$1

	awk -F, '$3 == "power/energy-pkg/" && $1 ~ /^[0-9.]+$/ { found = 1 } END { exit found ? 0 : 1 }' "$file"
}

verify_time_output()
{
	local file=$1

	awk -F, '$1 ~ /^[0-9.]+$/ && $2 ~ /^[0-9]+$/ { found = 1 } END { exit found ? 0 : 1 }' "$file"
}

cleanup_noop()
{
	:
}

cleanup_capture_output()
{
	rm -f -- "$CAPTURE_OUTPUT_FILE"
}

cleanup_on_exit()
{
	if [ -n "$CAPTURE_OUTPUT_DIR" ] && [ -d "$CAPTURE_OUTPUT_DIR" ]; then
		rm -rf -- "$CAPTURE_OUTPUT_DIR"
	fi
}

run_cleanup()
{
	local cleanup_fn=$1

	if [ -n "$cleanup_fn" ]; then
		"$cleanup_fn"
	fi
}

run_common_sanity_checks()
{
	require_command taskset
	require_command perf
	require_command awk
	require_command sort
	require_command mktemp
	require_executable_file /usr/bin/time
	require_executable_file build_release/lava-replay
	require_readable_file traces/anki_simple.api
	require_positive_integer "$REPEATS" "REPEATS"
}

run_benchmark_sanity_checks()
{
	local label=$1
	local cleanup_fn=$2
	shift 2

	local sanity_perf_output
	local sanity_time_output

	sanity_perf_output=$(mktemp)
	sanity_time_output=$(mktemp)

	run_cleanup "$cleanup_fn"
	if ! taskset -c 0-1 perf stat -e power/energy-pkg/ -o "$sanity_perf_output" -x, "$@" > /dev/null; then
		rm -f "$sanity_perf_output" "$sanity_time_output"
		echo "Sanity check failed for $label: perf measurement run failed" >&2
		exit 1
	fi

	if ! verify_perf_output "$sanity_perf_output"; then
		rm -f "$sanity_perf_output" "$sanity_time_output"
		echo "Sanity check failed for $label: perf did not record power/energy-pkg/ output" >&2
		exit 1
	fi

	run_cleanup "$cleanup_fn"
	if ! taskset -c 0-1 /usr/bin/time -o "$sanity_time_output" -f "%e,%M" "$@" > /dev/null; then
		rm -f "$sanity_perf_output" "$sanity_time_output"
		echo "Sanity check failed for $label: time measurement run failed" >&2
		exit 1
	fi

	if ! verify_time_output "$sanity_time_output"; then
		rm -f "$sanity_perf_output" "$sanity_time_output"
		echo "Sanity check failed for $label: /usr/bin/time did not record output" >&2
		exit 1
	fi

	run_cleanup "$cleanup_fn"
	rm -f "$sanity_perf_output" "$sanity_time_output"
}

collect_benchmark_samples()
{
	local label=$1
	local perf_output=$2
	local time_output=$3
	local cleanup_fn=$4
	shift 4

	rm -f "$perf_output" "$time_output"

	for ((i = 1; i <= REPEATS; i++)); do
		run_cleanup "$cleanup_fn"
		if ! taskset -c 0-1 perf stat --append -e power/energy-pkg/ -o "$perf_output" -x, "$@" > /dev/null; then
			echo "Sample $i/$REPEATS failed during $label perf measurement" >&2
			exit 1
		fi

		run_cleanup "$cleanup_fn"
		if ! taskset -c 0-1 /usr/bin/time -a -o "$time_output" -f "%e,%M" "$@" > /dev/null; then
			echo "Sample $i/$REPEATS failed during $label time measurement" >&2
			exit 1
		fi
	done

	run_cleanup "$cleanup_fn"
}

report_benchmark_stats()
{
	local label=$1
	local perf_output=$2
	local time_output=$3
	local energy_values
	local time_values
	local memory_values
	local energy_stats
	local time_stats
	local memory_stats

	energy_values=$(mktemp)
	time_values=$(mktemp)
	memory_values=$(mktemp)

	awk -F, '$3 == "power/energy-pkg/" && $1 ~ /^[0-9.]+$/ { print $1 }' "$perf_output" > "$energy_values"
	awk -F, '$1 ~ /^[0-9.]+$/ && $2 ~ /^[0-9]+$/ { print $1 }' "$time_output" > "$time_values"
	awk -F, '$1 ~ /^[0-9.]+$/ && $2 ~ /^[0-9]+$/ { print $2 }' "$time_output" > "$memory_values"

	require_sample_count "$energy_values" "$REPEATS" "$label energy"
	require_sample_count "$time_values" "$REPEATS" "$label time"
	require_sample_count "$memory_values" "$REPEATS" "$label memory"

	energy_stats=$(calculate_stats "$energy_values" 2)
	time_stats=$(calculate_stats "$time_values" 2)
	memory_stats=$(calculate_stats "$memory_values" 0)

	echo "$label:"
	echo "Median/average/best energy (J): $energy_stats"
	echo "Median/average/best time to completion (s): $time_stats"
	echo "Median/average/best memory consumption (KiB): $memory_stats"
	echo

	rm -f "$energy_values" "$time_values" "$memory_values"
}

benchmark()
{
	local label=$1
	local perf_output=$2
	local time_output=$3
	local cleanup_fn=$4
	shift 4

	run_benchmark_sanity_checks "$label" "$cleanup_fn" "$@"
	collect_benchmark_samples "$label" "$perf_output" "$time_output" "$cleanup_fn" "$@"
	report_benchmark_stats "$label" "$perf_output" "$time_output"
}

echo "--- Tests ---"
echo
COUNT_TESTS=$(grep -e Testing build/Testing/Temporary/LastTest.log | sed 's/.*\///' | sed 's/ .*//' | head -1)
echo "Number of tests: $COUNT_TESTS"
echo

echo "--- Autogeneration ---"
echo
HARDCODED_CAPTURE_COMMANDS=$(grep -e "VKAPI_CALL trace_vk" src/hardcode_write.cpp | wc -l)
HARDCODED_REPLAY_COMMANDS=$(grep -e "void retrace_vk" src/hardcode_read.cpp | wc -l)
HARDCODED_CAPTURE_STRUCTS=$(grep -e "void write_Vk" src/hardcode_write.cpp | wc -l)
HARDCODED_REPLAY_STRUCTS=$(grep -e "void read_Vk" src/hardcode_read.cpp | wc -l)
GENERATED_CAPTURE_COMMANDS=$(grep -e "VKAPI_CALL trace_vk" generated/write_auto.cpp | wc -l)
GENERATED_REPLAY_COMMANDS=$(grep -e "void retrace_vk" generated/read_auto.cpp | wc -l)
GENERATED_CAPTURE_STRUCTS=$(grep -e "void write_Vk" generated/struct_write_auto.cpp | wc -l)
GENERATED_REPLAY_STRUCTS=$(grep -e "void read_Vk" generated/struct_read_auto.cpp | wc -l)
echo "Hardcoded capture commands: $HARDCODED_CAPTURE_COMMANDS"
echo "Hardcoded replay commands: $HARDCODED_REPLAY_COMMANDS"
echo "Hardcoded capture structs: $HARDCODED_CAPTURE_STRUCTS"
echo "Hardcoded replay structs: $HARDCODED_REPLAY_STRUCTS"
echo "Autogenerated capture commands: $GENERATED_CAPTURE_COMMANDS"
echo "Autogenerated replay commands: $GENERATED_REPLAY_COMMANDS"
echo "Autogenerated capture structs: $GENERATED_CAPTURE_STRUCTS"
echo "Autogenerated replay structs: $GENERATED_REPLAY_STRUCTS"

TOTAL_HARDCODED_COMMANDS=$((HARDCODED_CAPTURE_COMMANDS + HARDCODED_REPLAY_COMMANDS))
TOTAL_GENERATED_COMMANDS=$((GENERATED_CAPTURE_COMMANDS + GENERATED_REPLAY_COMMANDS))
TOTAL_COMMANDS=$((TOTAL_HARDCODED_COMMANDS + TOTAL_GENERATED_COMMANDS))
TOTAL_HARDCODED_STRUCTS=$((HARDCODED_CAPTURE_STRUCTS + HARDCODED_REPLAY_STRUCTS))
TOTAL_GENERATED_STRUCTS=$((GENERATED_CAPTURE_STRUCTS + GENERATED_REPLAY_STRUCTS))
TOTAL_STRUCTS=$((TOTAL_HARDCODED_STRUCTS + TOTAL_GENERATED_STRUCTS))
TOTAL_GENERATED_ITEMS=$((TOTAL_GENERATED_COMMANDS + TOTAL_GENERATED_STRUCTS))
TOTAL_ITEMS=$((TOTAL_COMMANDS + TOTAL_STRUCTS))

COMMAND_PERCENTAGE=$(percentage "$TOTAL_GENERATED_COMMANDS" "$TOTAL_COMMANDS")
STRUCT_PERCENTAGE=$(percentage "$TOTAL_GENERATED_STRUCTS" "$TOTAL_STRUCTS")
TOTAL_PERCENTAGE=$(percentage "$TOTAL_GENERATED_ITEMS" "$TOTAL_ITEMS")

echo
echo "Total autogenerated commands: $TOTAL_GENERATED_COMMANDS out of $TOTAL_COMMANDS (${COMMAND_PERCENTAGE}%)"
echo "Total autogenerated structs: $TOTAL_GENERATED_STRUCTS out of $TOTAL_STRUCTS (${STRUCT_PERCENTAGE}%)"
echo "Total autogenerated items: $TOTAL_GENERATED_ITEMS out of $TOTAL_ITEMS (${TOTAL_PERCENTAGE}%)"
echo

echo "--- Performance ---"
echo

REPEATS=20
run_common_sanity_checks

CAPTURE_LAYER_DIR="$(pwd)/build_release/implicit_layer.d"
CAPTURE_OUTPUT_DIR=$(mktemp -d)
CAPTURE_OUTPUT_FILE="$CAPTURE_OUTPUT_DIR/anki_simple_capture.api"
trap cleanup_on_exit EXIT

require_executable_file scripts/lava-capture.py
require_directory "$CAPTURE_LAYER_DIR"
require_readable_file "$CAPTURE_LAYER_DIR/VkLayer_lavatube.json"
require_readable_file "$CAPTURE_LAYER_DIR/libVkLayer_lavatube.so"

REPLAY_COMMAND=(build_release/lava-replay -w none traces/anki_simple.api)
CAPTURE_COMMAND=(scripts/lava-capture.py --capture-layer "$CAPTURE_LAYER_DIR" -o "$CAPTURE_OUTPUT_FILE" build_release/lava-replay -w none traces/anki_simple.api)

benchmark "Replay" results.lst results2.list cleanup_noop "${REPLAY_COMMAND[@]}"
benchmark "Capture" capture_results.lst capture_results2.list cleanup_capture_output "${CAPTURE_COMMAND[@]}"
