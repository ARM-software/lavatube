#include <algorithm>
#include <assert.h>
#include <chrono>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static volatile uint64_t perf_sink = 0;

using patch_function = uint64_t(*)(char*, const char*, uint64_t);

static uint64_t patch_word(char* original, const char* changed, uint64_t size)
{
	uint64_t total_left = size;
	uint64_t written = 0;
	while (total_left)
	{
		while (total_left >= sizeof(uint64_t) && memcmp(original, changed, sizeof(uint64_t)) == 0)
		{
			original += sizeof(uint64_t);
			changed += sizeof(uint64_t);
			total_left -= sizeof(uint64_t);
		}
		while (total_left >= sizeof(uint64_t) && memcmp(original, changed, sizeof(uint64_t)) != 0)
		{
			memcpy(original, changed, sizeof(uint64_t));
			original += sizeof(uint64_t);
			changed += sizeof(uint64_t);
			total_left -= sizeof(uint64_t);
			written += sizeof(uint64_t);
		}
		if (total_left < sizeof(uint64_t))
		{
			if (memcmp(original, changed, total_left) != 0)
			{
				memcpy(original, changed, total_left);
				written += total_left;
			}
			total_left = 0;
		}
	}
	return written;
}

static uint64_t skip_identical_chunked(const char* original, const char* changed, uint64_t size)
{
	static constexpr uint64_t chunk_size = 256;
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

static uint64_t patch_chunked(char* original, const char* changed, uint64_t size)
{
	uint64_t total_left = size;
	uint64_t written = 0;
	while (total_left)
	{
		const uint64_t identical = skip_identical_chunked(original, changed, total_left);
		original += identical;
		changed += identical;
		total_left -= identical;
		while (total_left >= sizeof(uint64_t) && memcmp(original, changed, sizeof(uint64_t)) != 0)
		{
			memcpy(original, changed, sizeof(uint64_t));
			original += sizeof(uint64_t);
			changed += sizeof(uint64_t);
			total_left -= sizeof(uint64_t);
			written += sizeof(uint64_t);
		}
		if (total_left < sizeof(uint64_t))
		{
			if (memcmp(original, changed, total_left) != 0)
			{
				memcpy(original, changed, total_left);
				written += total_left;
			}
			total_left = 0;
		}
	}
	return written;
}

static uint64_t get_scale()
{
	const char* value = getenv("LAVATUBE_PATCHWRITE_PERF_SCALE");
	if (!value || value[0] == '\0') return 1;
	const uint64_t scale = strtoull(value, nullptr, 10);
	return scale == 0 ? 1 : scale;
}

static void print_header()
{
	printf("%-34s %10s %14s %14s %12s %12s %14s\n",
	       "name", "operations", "bytes_input", "bytes_changed", "time_ns", "GiB/s", "checksum");
}

static void run_case(const char* name, const std::vector<char>& first, const std::vector<char>& second,
	uint64_t target_bytes, patch_function patch)
{
	const uint64_t size = first.size();
	const uint64_t operations = std::max<uint64_t>(2, target_bytes / size);
	std::vector<char> clone(size, 0);
	uint64_t changed_bytes = 0;
	const auto start = std::chrono::steady_clock::now();
	for (uint64_t i = 0; i < operations; i++)
	{
		const std::vector<char>& target = (i & 1) ? second : first;
		changed_bytes += patch(clone.data(), target.data(), size);
	}
	const auto end = std::chrono::steady_clock::now();
	const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
	const uint64_t input_bytes = operations * size;
	const double gib_per_second = ns ? ((double)input_bytes / (1024.0 * 1024.0 * 1024.0)) / ((double)ns / 1000000000.0) : 0.0;
	uint64_t checksum = changed_bytes;
	for (uint64_t i = 0; i < size; i += 4096) checksum += (uint8_t)clone[i];
	perf_sink = (uint64_t)perf_sink + checksum;
	printf("%-34s %10" PRIu64 " %14" PRIu64 " %14" PRIu64 " %12" PRIu64 " %12.2f %14" PRIu64 "\n",
	       name, operations, input_bytes, changed_bytes, ns, gib_per_second, checksum);
}

static void compare_case(const char* pattern, uint64_t size, const std::vector<char>& first,
	const std::vector<char>& second, uint64_t target_bytes)
{
	std::vector<char> expected(size, 0);
	std::vector<char> candidate(size, 0);
	const uint64_t expected_written = patch_word(expected.data(), first.data(), size);
	const uint64_t candidate_written = patch_chunked(candidate.data(), first.data(), size);
	if (expected_written != candidate_written || expected != candidate) abort();

	char name[96];
	snprintf(name, sizeof(name), "word_%s_%" PRIu64 "K", pattern, size / 1024);
	run_case(name, first, second, target_bytes, patch_word);
	snprintf(name, sizeof(name), "chunked256_%s_%" PRIu64 "K", pattern, size / 1024);
	run_case(name, first, second, target_bytes, patch_chunked);
}

static void benchmark_size(uint64_t size, uint64_t target_bytes)
{
	std::vector<char> zero(size, 0);
	std::vector<char> first(size, 0);
	std::vector<char> second(size, 0);

	compare_case("unchanged", size, zero, zero, target_bytes);

	memset(first.data(), 0x55, size);
	memset(second.data(), 0xaa, size);
	compare_case("fully_changed", size, first, second, target_bytes);

	std::fill(first.begin(), first.end(), 0);
	std::fill(second.begin(), second.end(), 0);
	memset(first.data(), 0x55, size / 2);
	memset(second.data(), 0xaa, size / 2);
	compare_case("staging_half", size, first, second, target_bytes);

	std::fill(first.begin(), first.end(), 0);
	std::fill(second.begin(), second.end(), 0);
	for (uint64_t i = 0; i < size; i += 4096)
	{
		memset(first.data() + i, 0x55, std::min<uint64_t>(sizeof(uint64_t), size - i));
		memset(second.data() + i, 0xaa, std::min<uint64_t>(sizeof(uint64_t), size - i));
	}
	compare_case("sparse_4K", size, first, second, target_bytes);

	std::fill(first.begin(), first.end(), 0);
	std::fill(second.begin(), second.end(), 0);
	for (uint64_t i = 0; i < size; i += 2 * sizeof(uint64_t))
	{
		memset(first.data() + i, 0x55, std::min<uint64_t>(sizeof(uint64_t), size - i));
		memset(second.data() + i, 0xaa, std::min<uint64_t>(sizeof(uint64_t), size - i));
	}
	compare_case("alternating_8", size, first, second, target_bytes);
}

int main()
{
	const uint64_t scale = get_scale();
	const uint64_t target_bytes = 512ull * 1024ull * 1024ull * scale;
	printf("patchwrite_perf scale=%" PRIu64 " target_bytes_per_case=%" PRIu64 "\n", scale, target_bytes);
	print_header();
	benchmark_size(4 * 1024, target_bytes);
	benchmark_size(256 * 1024, target_bytes);
	benchmark_size(4 * 1024 * 1024, target_bytes);
	benchmark_size(64 * 1024 * 1024, target_bytes);
	printf("%-34s %10u %14u %14u %12u %12.2f %14" PRIu64 "\n",
	       "sink", 0u, 0u, 0u, 0u, 0.0, (uint64_t)perf_sink);
	return 0;
}
