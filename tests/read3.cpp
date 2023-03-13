#include <atomic>
#include <algorithm>
#include <numeric>
#include <thread>
#include <vector>
#include <string>

#include "util.h"
#include "read.h"

#include "tests/tests.h"

#define THREADS 50

static lava_reader* reader = nullptr;
static std::atomic_int read_tid;

void read_test_stress()
{
	const int mytid = read_tid.fetch_add(1);
	lava_file_reader& r = reader->file_reader(mytid);
	const uint8_t barrier_packet = r.read_uint8_t();
	assert(barrier_packet == 3);
	r.read_barrier();
	const uint32_t v1 = r.read_uint32_t();
	assert(v1 == 42);
	const uint32_t v2 = r.read_uint32_t();
	assert(v2 == 84);
	const char* str = r.read_string();
	assert(strcmp(str, "supercalifragilisticexpialidocious") == 0);
	const uint64_t v3 = r.read_uint64_t();
	assert(v3 == 3);

	const char *s = r.make_string("%s", str);
	assert(strcmp(str, s) == 0);
	r.append_string(":%d", (int)v2);
	assert(strcmp(s, "supercalifragilisticexpialidocious:84") == 0);
}

void read_test()
{
	read_tid = 0;
	reader = new lava_reader("write_3.vk");
	std::vector<std::thread*> threads(THREADS);
	for (auto& t : threads)
	{
		t = new std::thread(read_test_stress);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();
	delete reader;
	reader = nullptr;
}

int main()
{
	read_test();
	return 0;
}
