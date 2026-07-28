#include "filewriter.h"

#include <algorithm>
#include <chrono>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static volatile uint64_t perf_sink = 0;

using scan_function = uint64_t(*)(const char*, const char*, uint64_t, uint64_t);

static uint64_t find_patch_start_word(const char* original, const char* changed, uint64_t offset, uint64_t size)
{
	original += offset;
	changed += offset;
	uint64_t pos = 0;
	while (size - pos >= sizeof(uint64_t))
	{
		if (memcmp(original + pos, changed + pos, sizeof(uint64_t)) != 0) return pos;
		pos += sizeof(uint64_t);
	}
	if (pos < size && memcmp(original + pos, changed + pos, size - pos) != 0) return pos;
	return size;
}

static uint64_t find_patch_start_chunked(const char* original, const char* changed, uint64_t offset, uint64_t size)
{
	static constexpr uint64_t chunk_size = 256;
	original += offset;
	changed += offset;
	uint64_t pos = 0;
	if (size >= sizeof(uint64_t))
	{
		if (memcmp(original, changed, sizeof(uint64_t)) != 0) return 0;
		pos = sizeof(uint64_t);
	}
	while (size - pos >= chunk_size)
	{
		if (memcmp(original + pos, changed + pos, chunk_size) != 0) break;
		pos += chunk_size;
	}
	while (size - pos >= sizeof(uint64_t))
	{
		if (memcmp(original + pos, changed + pos, sizeof(uint64_t)) != 0) return pos;
		pos += sizeof(uint64_t);
	}
	if (pos < size && memcmp(original + pos, changed + pos, size - pos) != 0) return pos;
	return size;
}

static uint64_t get_scale()
{
	const char* value = getenv("LAVATUBE_PATCHSCAN_PERF_SCALE");
	if (!value || value[0] == '\0') return 1;
	const uint64_t scale = strtoull(value, nullptr, 10);
	return scale == 0 ? 1 : scale;
}

static void print_header()
{
	printf("%-30s %12s %14s %12s %12s %12s %14s\n",
	       "name", "operations", "bytes_examined", "time_ns", "ns/op", "GiB/s", "checksum");
}

static void run_case(const char* name, const std::vector<char>& original, const std::vector<char>& changed,
	uint64_t offset, uint64_t size, uint64_t target_bytes, scan_function scan)
{
	const uint64_t expected = scan(original.data(), changed.data(), offset, size);
	const uint64_t bytes_per_operation = expected == size ? size : std::min<uint64_t>(size, expected + sizeof(uint64_t));
	const uint64_t operations = std::min<uint64_t>(10000000, std::max<uint64_t>(16, target_bytes / bytes_per_operation));
	const auto start = std::chrono::steady_clock::now();
	uint64_t checksum = 0;
	for (uint64_t i = 0; i < operations; i++)
	{
		checksum += scan(original.data(), changed.data(), offset, size);
	}
	const auto end = std::chrono::steady_clock::now();
	const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
	const uint64_t bytes_examined = operations * bytes_per_operation;
	const double ns_per_operation = operations ? (double)ns / (double)operations : 0.0;
	const double gib_per_second = ns ? ((double)bytes_examined / (1024.0 * 1024.0 * 1024.0)) / ((double)ns / 1000000000.0) : 0.0;
	perf_sink = (uint64_t)perf_sink + checksum;
	printf("%-30s %12" PRIu64 " %14" PRIu64 " %12" PRIu64 " %12.2f %12.2f %14" PRIu64 "\n",
	       name, operations, bytes_examined, ns, ns_per_operation, gib_per_second, checksum);
}

static void run_comparison(const char* name, const std::vector<char>& original, const std::vector<char>& changed,
	uint64_t offset, uint64_t size, uint64_t target_bytes)
{
	assert(find_patch_start_word(original.data(), changed.data(), offset, size) ==
	       find_patch_start_chunked(original.data(), changed.data(), offset, size));
	char implementation_name[96];
	snprintf(implementation_name, sizeof(implementation_name), "word_%s", name);
	run_case(implementation_name, original, changed, offset, size, target_bytes, find_patch_start_word);
	snprintf(implementation_name, sizeof(implementation_name), "chunked256_%s", name);
	run_case(implementation_name, original, changed, offset, size, target_bytes, find_patch_start_chunked);
}

static void benchmark_size(uint64_t size, uint64_t target_bytes)
{
	std::vector<char> original(size, 0);
	std::vector<char> changed(size, 0);
	char name[64];

	snprintf(name, sizeof(name), "unchanged_%" PRIu64 "K", size / 1024);
	run_comparison(name, original, changed, 0, size, target_bytes);

	changed[0] = 1;
	snprintf(name, sizeof(name), "change_first_%" PRIu64 "K", size / 1024);
	run_comparison(name, original, changed, 0, size, target_bytes);
	changed[0] = 0;

	changed[size / 2] = 1;
	snprintf(name, sizeof(name), "change_middle_%" PRIu64 "K", size / 1024);
	run_comparison(name, original, changed, 0, size, target_bytes);
	changed[size / 2] = 0;

	changed[size - 1] = 1;
	snprintf(name, sizeof(name), "change_last_%" PRIu64 "K", size / 1024);
	run_comparison(name, original, changed, 0, size, target_bytes);
	changed[size - 1] = 0;

	changed[size - 1] = 1;
	snprintf(name, sizeof(name), "unchanged_offset_%" PRIu64 "K", size / 1024);
	run_comparison(name, original, changed, 3, size - 4, target_bytes);
}

int main()
{
	const uint64_t scale = get_scale();
	const uint64_t target_bytes = 1024ull * 1024ull * 1024ull * scale;
	printf("patchscan_perf scale=%" PRIu64 " target_bytes_per_case=%" PRIu64 "\n", scale, target_bytes);
	print_header();
	benchmark_size(4 * 1024, target_bytes);
	benchmark_size(256 * 1024, target_bytes);
	benchmark_size(4 * 1024 * 1024, target_bytes);
	benchmark_size(64 * 1024 * 1024, target_bytes);
	printf("%-30s %12u %14u %12u %12.2f %12.2f %14" PRIu64 "\n", "sink", 0u, 0u, 0u, 0.0, 0.0, (uint64_t)perf_sink);
	return 0;
}
