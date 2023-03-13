#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <thread>
#include <vector>
#include <string>

#include "util.h"
#include "read.h"
#include "write.h"

#include "tests/tests.h"

#define THREADS 50

static void thread_test_stress()
{
	lava_writer& writer = lava_writer::instance();
	if (random() % 5 == 1) usleep((random() % 20) * 1000); // introduce some pseudo-random timings
	lava_file_writer& file = writer.file_writer();
	int tid = file.thread_index();
	assert(tid < THREADS);
	file.write_uint32_t(42);
	if (tid % 2 == 1) usleep((random() % 20) * 1000); // introduce some pseudo-random timings
	file.write_uint32_t(84);
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
}

int main()
{
	lava_writer& writer = lava_writer::instance();
	writer.set("write_3-2", 1);
	thread_test();
	return 0;
}
