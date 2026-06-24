#include "rangetracking.h"

#include "tests/tests.h"

#include <chrono>
#include <inttypes.h>
#include <stdlib.h>

static volatile uint64_t perf_sink = 0;

static void print_header()
{
	printf("%-28s %14s %14s %12s %14s\n", "name", "operations", "time_ns", "ns_per_op", "checksum");
}

static void print_row(const char* name, uint64_t operations, uint64_t ns, double ns_per_op, uint64_t checksum)
{
	printf("%-28s %14" PRIu64 " %14" PRIu64 " %12.2f %14" PRIu64 "\n", name, operations, ns, ns_per_op, checksum);
}

struct benchmark_timer
{
	std::chrono::steady_clock::time_point start;
};

static uint64_t get_scale()
{
	const char* value = getenv("LAVATUBE_RANGETRACK_PERF_SCALE");
	if (!value || value[0] == '\0') return 1;
	const uint64_t scale = strtoull(value, nullptr, 10);
	return (scale == 0) ? 1 : scale;
}

static benchmark_timer start_benchmark()
{
	benchmark_timer timer;
	timer.start = std::chrono::steady_clock::now();
	return timer;
}

static void finish_benchmark(const char* name, uint64_t operations, const benchmark_timer& timer, uint64_t checksum)
{
	const auto end = std::chrono::steady_clock::now();
	const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - timer.start).count();
	const double ns_per_op = operations ? (double)ns / (double)operations : 0.0;
	perf_sink = (uint64_t)perf_sink + checksum;
	print_row(name, operations, ns, ns_per_op, checksum);
}

static void bench_add_append(uint64_t scale)
{
	const uint64_t fragments = 4096;
	const uint64_t repeats = 64 * scale;
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t repeat = 0; repeat < repeats; repeat++)
	{
		exposure r;
		for (uint64_t i = 0; i < fragments; i++) r.add(i * 4, i * 4 + 1);
		assert(r.size() == fragments);
		assert((r.span() == range{ 0, (fragments - 1) * 4 + 1 }));
		checksum += r.size() + r.span().last;
	}
	finish_benchmark("add_append_disjoint", repeats * fragments, timer, checksum);
}

static void bench_add_prepend(uint64_t scale)
{
	const uint64_t fragments = 4096;
	const uint64_t repeats = 64 * scale;
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t repeat = 0; repeat < repeats; repeat++)
	{
		exposure r;
		for (uint64_t i = fragments; i > 0; i--) r.add(i * 4, i * 4 + 1);
		assert(r.size() == fragments);
		assert((r.span() == range{ 4, fragments * 4 + 1 }));
		checksum += r.size() + r.span().first + r.span().last;
	}
	finish_benchmark("add_prepend_disjoint", repeats * fragments, timer, checksum);
}

static void bench_add_tail_merge(uint64_t scale)
{
	const uint64_t fragments = 4096;
	const uint64_t repeats = 64 * scale;
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t repeat = 0; repeat < repeats; repeat++)
	{
		exposure r;
		for (uint64_t i = 0; i < fragments; i++) r.add(i * 2, i * 2 + 1);
		assert(r.size() == 1);
		assert((r.span() == range{ 0, fragments * 2 - 1 }));
		checksum += r.size() + r.span().last;
	}
	finish_benchmark("add_tail_merge", repeats * fragments, timer, checksum);
}

static void build_fragmented_pair(exposure& a, exposure& b, uint64_t fragments)
{
	for (uint64_t i = 0; i < fragments; i++)
	{
		a.add(i * 4, i * 4);
		b.add(i * 4 + 2, i * 4 + 2);
	}
}

static void bench_overlap_span_reject(uint64_t scale)
{
	const uint64_t fragments = 4096;
	const uint64_t iterations = 500000 * scale;
	exposure a;
	exposure b;
	for (uint64_t i = 0; i < fragments; i++)
	{
		a.add(i * 4, i * 4);
		b.add(1000000 + i * 4, 1000000 + i * 4);
	}
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t i = 0; i < iterations; i++)
	{
		range v = a.overlap(b);
		assert(!v.valid());
		checksum += v.first + v.last;
	}
	finish_benchmark("overlap_span_reject", iterations, timer, checksum);
}

static void bench_overlap_fragmented_miss(uint64_t scale)
{
	const uint64_t fragments = 4096;
	const uint64_t iterations = 512 * scale;
	exposure a;
	exposure b;
	build_fragmented_pair(a, b, fragments);
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t i = 0; i < iterations; i++)
	{
		range v = a.overlap(b);
		assert(!v.valid());
		checksum += v.first + v.last;
	}
	finish_benchmark("overlap_fragmented_miss", iterations * fragments, timer, checksum);
}

static void bench_fetch_single_mapped(uint64_t scale)
{
	const uint64_t iterations = 1000000 * scale;
	exposure r;
	r.add(0, 4095);
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t i = 0; i < iterations; i++)
	{
		range v = r.fetch(128, 128, true);
		assert((v == range{ 128, 128 }));
		checksum += v.first + v.last;
	}
	assert(r.size() == 1);
	finish_benchmark("fetch_single_mapped", iterations, timer, checksum);
}

static void bench_fetch_fragmented_split(uint64_t scale)
{
	const uint64_t fragments = 1024;
	const uint64_t repeats = 16 * scale;
	benchmark_timer timer = start_benchmark();
	uint64_t checksum = 0;
	for (uint64_t repeat = 0; repeat < repeats; repeat++)
	{
		exposure r;
		for (uint64_t i = 0; i < fragments; i++) r.add(i * 4, i * 4 + 2);
		for (uint64_t i = 0; i < fragments; i++)
		{
			range v = r.fetch(i * 4 + 1, i * 4 + 1, false);
			assert((v == range{ i * 4 + 1, i * 4 + 1 }));
			checksum += v.first + v.last;
		}
		assert(r.size() == fragments * 2);
		assert(r.bytes() == fragments * 2);
	}
	finish_benchmark("fetch_fragmented_split", repeats * fragments, timer, checksum);
}

int main()
{
	const uint64_t scale = get_scale();
	printf("rangetrack_perf scale=%" PRIu64 "\n", scale);
	print_header();
	bench_add_append(scale);
	bench_add_prepend(scale);
	bench_add_tail_merge(scale);
	bench_overlap_span_reject(scale);
	bench_overlap_fragmented_miss(scale);
	bench_fetch_single_mapped(scale);
	bench_fetch_fragmented_split(scale);
	print_row("sink", 0, 0, 0.0, (uint64_t)perf_sink);
	return 0;
}
