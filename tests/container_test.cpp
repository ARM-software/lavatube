#include <atomic>
#include <thread>
#include "containers.h"
#include "lavamutex.h"

#include "tests/tests.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

#define THREADS 20
#define ALLOCATED 400
static uint64_t allocated[ALLOCATED];
static lava::mutex idmutex;
static std::atomic_int count;

struct track
{
	uint32_t a;
	uint32_t refcount; // required member
};

struct big
{
	uint64_t a;
	uint64_t b;
	uint64_t c;
	uint64_t d;
	uint64_t e;
	uint64_t f;
	uint64_t g;
	uint64_t h;
};

struct our_trackable
{
	uint32_t index;
	change_source creation = { 0, 0, 0, 0 };
	change_source destroyed = { 0, 0, 0, 0 };
	change_source last_modified = { 0, 0, 0, 0 };
};

static trace_data<track> mp_tracks;
static trace_remap<uint64_t, our_trackable> static_alloc;

static void pool1()
{
	memory_pool pool;
	char* pchar = pool.allocate<char>(1000);
	pchar = pool.allocate<char>(100);
	int* pint = pool.allocate<int>(1000);
	pchar[1] = 'a';
	pint[1] = 3;
	pool.reset();
	for (unsigned i = 0; i < 1000; i++)
	{
		track* ptrack = pool.allocate<track>(10);
		ptrack[0].a = 0;
		ptrack[1].refcount = 1;
	}
	pool.reset();
}

static void test_trace_data_1()
{
	trace_data<track> track_index;
	track_index.push_back({ 1, 2 });
	assert(track_index.size() == 1);
	assert(track_index.at(0).a == 1);
	assert(track_index.at(0).refcount == 2);
	track t{ 0, 0 };
	for (unsigned i = 0; i < 2048; i++) track_index.push_back(t);
	assert(track_index.size() == 2049);
	assert(track_index.at(0).a == 1);
	assert(track_index.at(0).refcount == 2);
	track_index.emplace_back(t);
	track_index.clear();
	track_index.clear();
}

static void test_trace_data_2()
{
	trace_data<big> track_index;
	track_index.push_back({ 1, 1, 1, 1, 1, 1, 1, 1 });
	assert(track_index.size() == 1);
	assert(track_index.at(0).a == 1);
	assert(track_index.at(0).h == 1);
	for (unsigned i = 2; i < 2048; i++) track_index.push_back({ i, i, i, i, i, i, i, i });
	assert(track_index.size() == 2047);
	for (unsigned i = 0; i < 2047; i++) assert(track_index.at(i).a == i + 1);
	assert(track_index.at(0).b == 1);
	assert(track_index.at(0).h == 1);
	track_index.clear();
}

static void test_trace_1()
{
	trace_remap<uint64_t, our_trackable> trace;
	our_trackable* v = trace.add(1000, change_source{ 0, 0, 0, 0 });
	assert(v->index == 0);
	assert(v->creation.frame == 0);
	v = trace.add(2000, change_source{ 0, 1, 0, 0 });
	assert(v->index == 1);
	assert(v->creation.frame == 1);
	v = trace.at(1000);
	assert(v->index == 0);
	assert(v->creation.frame == 0);
	v = trace.at(2000);
	assert(v->index == 1);
	assert(v->creation.frame == 1);
	v = trace.unset(1000, change_source{ 0, 1, 0, 0 });
	assert(v->index == 0);
	assert(v->creation.frame == 0);
	v = trace.unset(2000, change_source{0, 1, 0, 0 });
	assert(v->index == 1);
	assert(v->creation.frame == 1);
}

static void test_replay_1()
{
	replay_remap<uint64_t> replay;
	replay.resize(1000);
	for (unsigned i = 0; i < 1000; i++)
	{
		replay.set(i, i + 1);
	}
	replay.unset(0);
	(void)replay.at(1);
	replay.unset(1);
	uint32_t idx = replay.at(CONTAINER_NULL_VALUE);
	assert(idx == 0);
	(void)idx;
}

static std::atomic_bool done { false };
static std::vector<unsigned> ids(1024, 0);
static const unsigned itercount = 1024;
static void thread_test_1(trace_remap<uint64_t, our_trackable>* mp_vals)
{
	unsigned idx = 1;
	while (true)
	{
		idmutex.lock();
		auto* v = mp_vals->add(idx, change_source{ 0, 1, 0, 0 });
		ids[idx] = 1;
		idx++;
		idmutex.unlock();
		if (idx == itercount) break;
	}
	idx = 0;
	while (!done.load(std::memory_order_relaxed))
	{
		idmutex.lock();
		mp_vals->unset(idx, change_source{ 0, 1, 0, 0 });
		ids[idx] = 0;
		idx++;
		if (idx == itercount) done.store(true);
		idmutex.unlock();
	}
}
static void thread_test_2(trace_remap<uint64_t, our_trackable>* mp_vals)
{
	unsigned current = 1;
	while (!done.load(std::memory_order_relaxed))
	{
		bool found = false;
		if (current == 0) current++;
		else if (current == 1024) current = 0;

		idmutex.lock();
		found = (ids[current] == 1);
		if (found) assert(mp_vals->at(current)->creation.frame == 1);
		current++;
		idmutex.unlock();
	}
}
static void thread_test_3(trace_remap<uint64_t, our_trackable>* mp_vals) // contains
{
	unsigned current = 1;
	while (!done.load(std::memory_order_relaxed))
	{
		bool found = false;
		if (current == 0) current++;
		else if (current == 1024) current = 0;

		idmutex.lock();
		found = (ids[current] == 1);
		if (found) assert(mp_vals->contains(current) == true);
		current++;
		idmutex.unlock();
	}
}
static void test_trace_remap_mp()
{
	done.store(false);
	trace_remap<uint64_t, our_trackable> mp_vals;
	count = 0;
	memset(allocated, 0, sizeof(allocated));
	int i = 0;
	std::thread* t1 = new std::thread(thread_test_1, &mp_vals);
	std::thread* t2 = new std::thread(thread_test_2, &mp_vals);
	std::thread* t3 = new std::thread(thread_test_3, &mp_vals);
	t1->join();
	t2->join();
	t3->join();
	delete t1;
	delete t2;
	delete t3;
	mp_vals.clear();
}

// -- mp trace_data test --

static void thread_test_trace_data(int delay)
{
	for (unsigned i = 0; i < 3; i++) assert(mp_tracks.at(i).a == i + 1);
	for (unsigned i = 0; i < 3; i++) assert(mp_tracks.at(i).refcount == 2);
	for (unsigned i = 0; i < 3; i++) assert(mp_tracks.at(i).a == i + 1);
}

static void test_trace_data_mp()
{
	assert(mp_tracks.size() == 0);

	mp_tracks.push_back({ 1, 2 });
	mp_tracks.push_back({ 2, 2 });
	mp_tracks.push_back({ 3, 2 });

	std::vector<std::thread*> threads(THREADS);
	int i = 0;
	for (auto& t : threads)
	{
		t = new std::thread(thread_test_trace_data, i);
		i = (i + 1) % 4;
	}

	track t{ 0, 0 };
	for (unsigned j = 0; j < 2048; j++) mp_tracks.push_back(t);

	assert(mp_tracks.size() == 2051);
	assert(mp_tracks.at(0).a == 1);
	assert(mp_tracks.at(0).refcount == 2);
	assert(mp_tracks.at(510).a == 0);

	for (std::thread* th : threads)
	{
		th->join();
		delete th;
	}

	mp_tracks.clear();
	threads.clear();
	assert(mp_tracks.size() == 0);
}

// -- trace_remap test --

static void test_trace_remap_check_range(unsigned max)
{
	trace_remap<uint64_t, our_trackable> remapper;
	assert(remapper.contains(9999) == false);
	for (unsigned i = 1; i < max; i++) remapper.add(i, change_source{ 0, i * 4, 0, 0 });
	for (unsigned i = 1; i < max; i++) assert((unsigned)remapper.at(i)->creation.frame == i * 4);
	for (unsigned i = 1; i < max; i++) assert((unsigned)remapper.unset(i, change_source{ 0, i * 4, 0, 0 })->creation.frame == i * 4);
	for (unsigned i = 1; i < max; i++) assert(remapper.contains(i) == false);
	remapper.clear();
}

static void test_trace_remap_test1()
{
	trace_remap<uint64_t, our_trackable> remapper;
	remapper.add(1, change_source{ 0, 1, 0, 0 });
	assert(remapper.contains(999) == false);
	assert(remapper.contains(9999) == false);
	assert(remapper.at(1)->index == 0);
	for (unsigned i = 2; i < 2048; i++) remapper.add(i, change_source{ 0, i, 0, 0 });
	for (unsigned i = 2; i < 2048; i++) assert(remapper.at(i)->creation.frame == i);
	remapper.clear();

	static_alloc.add(1, change_source{ 0, 1, 0, 0 });
	assert(static_alloc.at(1)->index == 0);
	static_alloc.unset(1, change_source{ 0, 1, 0, 0 });
}

static void test_trace_remap_test2()
{
	trace_remap<uint64_t, our_trackable> remapper;
	for (unsigned i = 1; i < 2048; i++) assert(remapper.contains(i) == false);
	for (unsigned i = 1; i < 2048; i++) remapper.add(i, change_source{ 0, i, 0, 0 });
	for (unsigned i = 1; i < 2048; i++) remapper.unset(i, change_source{ 0, i, 0, 0 });
	for (unsigned i = 1; i < 2048; i++) assert(remapper.contains(i) == false);
	for (unsigned i = 1; i < 2048; i++) remapper.add(i, change_source{ 0, i, 0, 0 });
	for (unsigned i = 1; i < 2048; i++) assert(remapper.at(i)->creation.frame == i);
	remapper.clear();
}

static void test_address_remapper()
{
	address_remapper r;
	const auto end = r.end();

	// test empty set
	assert(r.iter_by_address(0) == end);
	assert(r.iter_by_address(50) == end);
	assert(r.get_by_address(0) == nullptr);
	assert(r.get_by_address(500) == nullptr);
	assert(r.translate_address(0) == 0);
	assert(r.translate_address(500) == 0);
	assert(r.get_by_range(0, 100).size() == 0);
	assert(r.get_by_range(100, 100).size() == 0);
	assert(r.is_candidate(0) == false);
	assert(r.is_candidate(100) == false);

	// test non-overlapping ranges
	r.add(100, 1100, 50);
	r.add(200, 1200, 50);
	r.add(300, 1300, 50);
	assert(r.iter_by_address(50) == end);
	assert(r.smallest_by_address(50) == end);
	assert(r.get_by_address(50) == nullptr);
	assert(r.get_by_address(299) == nullptr);
	assert(r.translate_address(50) == 0);
	assert(r.translate_address(450) == 0);
	assert(r.translate_address(199) == 0);
	assert(r.translate_address(100) == 1100);
	assert(r.translate_address(149) == 1149);
	assert(r.translate_address(135) == 1135);
	assert(r.is_candidate(0) == false);
	assert(r.is_candidate(1) == false);
	assert(r.is_candidate(100) == true);
	assert(r.is_candidate((uint64_t)100 << 32) == true);
	assert(r.is_candidate(199) == false);
	assert(r.get_by_address(100)->new_address == 1100);
	assert(r.get_by_address(200)->new_address == 1200);
	assert(r.get_by_address(300)->new_address == 1300);
	assert(r.get_by_address(149)->new_address == 1100);
	assert(r.get_by_address(150) == nullptr);

	// test overlapping ranges, shall return smallest range if multiple hits
	r.add(110, 3110, 20);
	r.add(190, 4190, 10);
	assert(r.translate_address(110) == 3110);
	assert(r.translate_address(135) == 1135);
	assert(r.translate_address(190) == 4190);
	assert(r.translate_address(120) == 3120);
	assert(r.translate_address(195) == 4195);
	assert(r.translate_address(109) == 1109);
	assert(r.is_candidate(100) == true);
	assert(r.is_candidate(115) == true);
	assert(r.get_by_range(100, 5).size() == 1);
	assert(r.get_by_range(100, 5)[0].new_address == 1100);
	assert(r.get_by_range(110, 10).size() == 2);
}

int main()
{
	// single threaded
	pool1();
	test_replay_1();
	test_trace_1();
	test_trace_data_1();
	test_trace_data_2();
	test_trace_remap_check_range(16);
	test_trace_remap_check_range(32);
	test_trace_remap_check_range(99);
	test_trace_remap_check_range(64);
	test_trace_remap_check_range(100);
	test_trace_remap_check_range(11);
	test_trace_remap_check_range(999);
	test_trace_remap_test1();
	test_trace_remap_test2();

	test_address_remapper();

	// multi threaded
	test_trace_remap_mp();
	test_trace_data_mp();

	return 0;
}
