#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <thread>
#include <vector>
#include <string>

#include "util.h"
#include "write.h"

#include "tests/tests.h"

#define THREADS 50

static lava_writer& writer = lava_writer::instance();
static std::atomic_int used[THREADS];

static void thread_test_stress()
{
	set_thread_name("worker");
	if (random() % 5 == 1) usleep(random() % 3 * 10000); // introduce some pseudo-random timings
	lava_file_writer& file = writer.file_writer();
	int tid = file.thread_index();
	assert(tid < THREADS);
	assert(used[tid] == 0);
	used[tid] = 1;
	file.write_uint32_t(42);
	if (tid % 2 == 1) usleep(tid * 10000); // introduce some pseudo-random timings
	file.write_uint32_t(84);
	file.write_string("supercalifragilisticexpialidocious");
	file.write_uint64_t(3);
	file.finalize();
}

static void thread_test()
{
	std::vector<std::thread*> threads(THREADS);
	for (auto& t : threads)
	{
		t = new std::thread(thread_test_stress);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();
}

int main()
{
	for (int i = 0; i < THREADS; i++) used[i] = 0;
	writer.set("write_3");
	thread_test();
	for (int i = 0; i < THREADS; i++)
	{
		assert(used[i] == 1);
	}
	return 0;
}
