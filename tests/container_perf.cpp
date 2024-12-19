#include <atomic>
#include <thread>
#include <inttypes.h>
#include "containers.h"
#include "lavamutex.h"

#include "tbb/concurrent_unordered_map.h"
#include "tbb/concurrent_vector.h"

static const unsigned runsize = 10000;

static inline uint64_t mygettime()
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

struct our_trackable
{
	uint32_t index;
	change_source creation = { 0, 0, 0, 0 };
	change_source destroyed = { 0, 0, 0, 0 };
	change_source last_modified = { 0, 0, 0, 0 };
	uint32_t a;
	uint32_t b;
};

struct track
{
	uint32_t a;
	uint32_t refcount; // required member
};

// --------------

template<typename T>
class tbb_trace_remap
{
public:
	tbb_trace_remap() {}

	inline void set(T obj, uint32_t index) { remapping[obj] = index; }
	inline uint32_t unset(T handle) { if (handle == 0) return CONTAINER_NULL_VALUE; const uint32_t index = remapping.at(handle); remapping[handle] = CONTAINER_INVALID_INDEX; return index; }
	inline uint32_t at(const T handle) { if (handle == 0) return CONTAINER_NULL_VALUE; const uint32_t index = remapping.at(handle); return index; }
	inline bool contains(const T handle) const { return (remapping.count(handle) > 0 && remapping.at(handle) != CONTAINER_INVALID_INDEX); }
	inline void clear() { remapping.clear(); }

private:
	tbb::concurrent_unordered_map<T, uint32_t> remapping;
};

template<typename T>
class tbb_trace_data
{
public:
	tbb_trace_data() {}

	inline T& at(uint32_t index) { return backing.at(index); }
	inline const T& at(uint32_t index) const { return backing.at(index); }
	inline size_t size() const { return backing.size(); }
	inline void push_back(const T& t) { backing.push_back(t); }
	inline void clear() { backing.clear(); }

private:
	tbb::concurrent_vector<T> backing;
};

// --------------

static void test_memory_pool1(bool actual)
{
	uint64_t start = mygettime();
	memory_pool pool;
	for (unsigned i = 0; i < 100000; i++)
	{
		track* ptrack = pool.allocate<track>(10);
		ptrack[0].a = 0;
		ptrack[1].refcount = 1;
		pool.reset();
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "memory_pool_1", end - start);
}

static void test_trace_data_1(bool actual)
{
	trace_data<track> track_index;
	track t{ 0, 0 };
	uint64_t start = mygettime();
	for (unsigned i = 0; i < runsize; i++)
	{
		track_index.push_back(t);
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "trace_data_1::set", end - start);
	start = mygettime();
	for (unsigned i = 0; i < runsize; i++)
	{
		const track& tc = track_index.at(i);
		(void)tc;
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "trace_data_1::get", end - start);
	track_index.clear();
}

static void test_tbb_trace_data_1(bool actual)
{
	tbb_trace_data<track> track_index;
	track t{ 0, 0 };
	uint64_t start = mygettime();
	for (unsigned i = 0; i < runsize; i++)
	{
		track_index.push_back(t);
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "tbb_trace_data_1::set", end - start);
	start = mygettime();
	for (unsigned i = 0; i < runsize; i++)
	{
		const track& tc = track_index.at(i);
		(void)tc;
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "tbb_trace_data_1::get", end - start);
	track_index.clear();
}

static void test_trace_remap_1(bool actual)
{
	trace_remap<uint64_t, our_trackable> remapper;
	uint64_t start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.add(i, change_source{ 0, i, 0, 0 });
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "trace_remap_1::set", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		const uint64_t tc = remapper.at(i)->index;
		(void)tc;
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "trace_remap_1::get", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.unset(i, change_source{ 0, i, 0, 0 });
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "trace_remap_1::unset", end - start);

	remapper.clear();
}

static void test_tbb_trace_remap_1(bool actual)
{
	tbb_trace_remap<uint64_t> remapper;
	uint64_t start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.set(i, i);
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "tbb_trace_remap_1::set", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		const uint64_t tc = remapper.at(i);
		(void)tc;
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "tbb_trace_remap_1::get", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.unset(i);
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "tbb_trace_remap_1::unset", end - start);

	remapper.clear();
}

static void test_replay_remap_1(bool actual)
{
	replay_remap<uint64_t> remapper;
	remapper.resize(runsize);
	uint64_t start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.set(i, i);
	}
	uint64_t end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "replay_remap_1::set", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		const uint64_t tc = remapper.at(i);
		(void)tc;
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "replay_remap_1::get", end - start);

	start = mygettime();
	for (unsigned i = 1; i < runsize; i++)
	{
		remapper.unset(i);
	}
	end = mygettime();
	if (actual) printf("%-30s %'12" PRIu64 "\n", "replay_remap_1::unset", end - start);

	remapper.clear();
}

int main()
{
	setlocale(LC_NUMERIC, "");

	// warm-up
	test_memory_pool1(false);
	test_trace_data_1(false);
	test_trace_remap_1(false);
	test_tbb_trace_data_1(false);
	test_tbb_trace_remap_1(false);
	test_replay_remap_1(false);

	test_memory_pool1(true);
	test_trace_data_1(true);
	test_trace_remap_1(true);
	test_tbb_trace_data_1(true);
	test_tbb_trace_remap_1(true);
	test_replay_remap_1(true);

	return 0;
}
