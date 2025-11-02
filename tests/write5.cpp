#include <limits>
#include <cmath>
#include <vector>
#include <string>
#include <inttypes.h>

#include "util.h"

#include "filewriter.h"
#include "filereader.h"

#include "tests/tests.h"

static uint64_t total = 0;

static inline uint64_t mygettime()
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

static void write_test_pattern_stride(bool actual, int stride, int conseq)
{
	unsigned size = 1024 * 1024 * 8;
	uint64_t offset = 0;
	file_writer file(0);
	std::string name = "test_pattern_stride_" + std::to_string(stride) + "_conseq_" + std::to_string(conseq);
	std::string filename = "write5_" + name + ".bin";
	file.set(filename);
	std::vector<uint8_t> vals1(size, 42); // clone
	std::vector<uint8_t> vals2(size, 42); // real
	char* clone = (char*)vals1.data(); // copy of original
	char* ptr = (char*)vals2.data(); // newly changed buffer
	unsigned changes = 0;
	for (unsigned i = 0; i + conseq < size; i += stride) { for (int j = 0; j < conseq; j++) ptr[i + j] = 43; changes += conseq; }
	uint64_t start = mygettime();
	uint64_t changed = file.write_patch(clone, ptr, offset, size);
	uint64_t end = mygettime();
	total += end - start;
	if (actual) printf("test_pattern_stride_%03d_conseq_%03d   %'12" PRIu64 " (%'8" PRIu64 " bytes stored, ideal would be %'8u)\n", stride, conseq, end - start, changed, changes);
	assert(memcmp(ptr, clone, size) == 0);
	unlink(filename.c_str());
}

static size_t write_test_1()
{
	file_writer file(0);
	file.set("write_5.bin");

	std::vector<uint16_t> vals1(1024 * 1024 * 16, 42); // original
	std::vector<uint16_t> vals2(1024 * 1024 * 16, 42); // updated

	uint64_t offset = 0;
	uint64_t size = vals1.size();
	uint64_t bytesize = size * sizeof(uint16_t);
	char* clone = (char*)vals1.data(); // copy of original
	char* ptr = (char*)vals2.data(); // newly changed buffer
	file.write_memory(ptr, 0, size);
	vals2[1000] = 65535;
	vals2[1001] = 65535;
	vals2[2099] = 65535;
	vals2[3000] = 65535;
	uint64_t changed = file.write_patch(clone, ptr, offset, bytesize);
	printf("write_test_1 changed %lu bytes\n", (unsigned long)changed);
	uint64_t start = mygettime();
	for (unsigned i = 0; i < 20; i++) { changed = file.write_patch(clone, ptr, offset, bytesize); assert(changed == 0); }
	uint64_t end = mygettime();
	printf("write_test_1: %lu\n", (unsigned long)end - start);
	assert(memcmp(ptr, clone, bytesize) == 0);
	return file.uncompressed_bytes;
}

static void read_test_1(size_t bytes)
{
	file_reader t0("write_5.bin", 0, bytes);

	std::vector<uint16_t> vals(1024 * 1024 * 16, 0);

	uint32_t changed = t0.read_patch((char*)vals.data(), vals.size());
	assert(changed == 1024 * 1024 * 16);
	for (unsigned i = 0; i < 10; i++)
	{
		changed = t0.read_patch((char*)vals.data(), vals.size());
		assert(changed <= 4 * sizeof(uint64_t));
	}
}

int main()
{
	size_t bytes = write_test_1();
	sync();
	read_test_1(bytes);
	unlink("write_5.bin");

	// warmup
	for (int i = 2; i <= 16; i++) write_test_pattern_stride(false, i, 1);

	// real
	for (int i = 2; i <= 32; i++) write_test_pattern_stride(true, i, 1);
	write_test_pattern_stride(true, 4, 2);
	write_test_pattern_stride(true, 5, 2);
	write_test_pattern_stride(true, 6, 2);
	write_test_pattern_stride(true, 7, 2);
	write_test_pattern_stride(true, 8, 2);
	write_test_pattern_stride(true, 9, 2);
	write_test_pattern_stride(true, 6, 3);
	write_test_pattern_stride(true, 7, 3);
	write_test_pattern_stride(true, 8, 3);
	write_test_pattern_stride(true, 9, 3);
	write_test_pattern_stride(true, 9, 4);
	write_test_pattern_stride(true, 9, 5);
	write_test_pattern_stride(true, 9, 6);
	write_test_pattern_stride(true, 16, 5);
	write_test_pattern_stride(true, 32, 5);
	write_test_pattern_stride(true, 64, 5);
	write_test_pattern_stride(true, 128, 5);
	write_test_pattern_stride(true, 32, 8);
	write_test_pattern_stride(true, 64, 8);
	write_test_pattern_stride(true, 128, 8);
	write_test_pattern_stride(true, 256, 16);
	printf("Total: %lu\n", (unsigned long)total);

	return 0;
}
