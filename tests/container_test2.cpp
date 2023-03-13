#include <atomic>
#include <thread>
#include "containers.h"
#include "lavamutex.h"

#include "tests/tests.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

struct our_trackable
{
	uint32_t index;
	int frame_created = 0;
	int frame_destroyed = 0;
	uint32_t a;
	uint32_t b;
};

static trace_remap<uint64_t, our_trackable> remap;
static lava::mutex mutex;

static void insert(unsigned i)
{
	mutex.lock();
	auto* data = remap.add(i, i);
	data->a = i;
	data->b = i;
	mutex.unlock();
}

static void thread_test_1()
{
	for (int i = 6; i <= 9; i++) insert(i);
	for (int i = 0; i < 500; i++) assert(remap.contains(12) == false);
	insert(12);
	for (int i = 0; i < 2000; i++) assert(remap.contains(12) == true);
	insert(500);
	for (int i = 0; i < 2000; i++) assert(remap.contains(500) == true);
	for (int i = 501; i <= 900; i++) insert(i);
}

static void thread_test_2()
{
	for (int i = 0; i < 10000; i++)
	{
		assert(remap.at(1)->frame_created == 1);
		assert(remap.at(1)->a == 1);
		assert(remap.at(1)->b == 1);
	}
	insert(11);
	for (int i = 0; i < 100000; i++)
	{
		assert(remap.at(11)->frame_created == 11);
		assert(remap.at(11)->a == 11);
		assert(remap.at(11)->b == 11);
	}
}

static void thread_test_3()
{
	assert(remap.contains(10) == false);
	for (int i = 0; i < 2500; i++)
	{
		assert(remap.contains(1) == true);
		assert(remap.size() > 0);
	}
	insert(10);
	assert(remap.contains(10) == true);
	insert(14);
	for (int i = 0; i < 250000; i++)
	{
		assert(remap.contains(1) == true);
		assert(remap.contains(10) == true);
		assert(remap.contains(14) == true);
		assert(remap.size() > 0);
	}
}

int main()
{
	insert(1);
	for (int i = 351; i < 499; i++) insert(i);

	std::thread* t1 = new std::thread(thread_test_1);
	std::thread* t2 = new std::thread(thread_test_2);
	std::thread* t3 = new std::thread(thread_test_3);

	insert(2);
	insert(3);
	insert(4);
	insert(5);
	for (int i = 15; i < 350; i++) insert(i);

	t1->join();
	t2->join();
	t3->join();

	delete t1;
	delete t2;
	delete t3;

	for (unsigned i = 0; i < 900; i++)
	{
		if (remap.contains(i))
		{
			assert(remap.at(i)->frame_created == (int)i);
			assert(remap.at(i)->a == i);
			assert(remap.at(i)->b == i);
		}
	}

	remap.clear();

	return 0;
}
