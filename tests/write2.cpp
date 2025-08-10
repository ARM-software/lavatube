#include <atomic>
#include <algorithm>
#include <numeric>
#include <thread>
#include <vector>
#include <string>

#include "util.h"
#include "read.h"
#include "write.h"

#include "tests/tests.h"

static std::vector<uint32_t> data(65535);
static lava_writer& writer = lava_writer::instance();
static lava_reader* reader = nullptr;
static std::atomic_int read_tid;

static void write_test_stress()
{
	lava_file_writer& file = writer.file_writer();
	file.self_test();
	for (int i = 0; i < 200; i++)
	{
		file.write_uint64_t(0xffe0ffe0);
		file.write_array(data.data(), data.size());
		file.write_uint64_t(0xdeadbeef);
		file.write_array(data.data(), data.size());
		file.write_string("supercalifragilisticexpialidocious");
	}
	file.self_test();
}

static void write_test(const char* name, int num_threads)
{
	ILOG("%s", name);
	writer.set(name, 1);
	std::vector<std::thread*> threads(num_threads);
	for (auto& t : threads)
	{
		t = new std::thread(write_test_stress);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();
	writer.serialize();
	writer.finish();
	writer.self_test();
}

static void read_test_stress()
{
	const int mytid = read_tid.fetch_add(1);
	lava_file_reader& r = reader->file_reader(mytid);
	const uint8_t barrier_packet = r.read_uint8_t();
	assert(barrier_packet == 3);
	r.read_barrier();
	for (int i = 0; i < 200; i++)
	{
		const uint64_t v1 = r.read_uint64_t();
		assert(v1 == 0xffe0ffe0);
		std::vector<uint32_t> data2(data.size());
		r.read_array(data2.data(), data2.size());
		assert(data[100] == data2[100]);
		const uint64_t v2 = r.read_uint64_t();
		assert(v2 == 0xdeadbeef);
		r.read_array(data2.data(), data2.size());
		assert(data[100] == data2[100]);
		const char* str = r.read_string();
		assert(strcmp(str, "supercalifragilisticexpialidocious") == 0);
	}
}

static void read_test(const char* name, int num_threads)
{
	std::string filename = std::string(name) + ".vk";
	read_tid = 0;
	reader = new lava_reader(filename);
	std::vector<std::thread*> threads(num_threads);
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
	// populate with a number series
	std::iota(std::begin(data), std::end(data), 0);

	// simple variant
	write_test("write_2_1", 1);
	read_test("write_2_1", 1);

	write_test("write_2_2", 2);
	read_test("write_2_2", 2);

	// this will generate a lot of threads - 32 * 3!
	write_test("write_2_3", 16);
	read_test("write_2_3", 16);

	return 0;
}
