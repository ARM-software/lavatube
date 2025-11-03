// You need to run the write4 test first.

#include <limits>
#include <cmath>
#include <vector>
#include <string>

#include "util.h"
#include "filereader.h"

#include "tests/tests.h"

void read_test_1()
{
	file_reader t0("write_4.bin", 0, 131445, 131445);
	const uint8_t v1 = t0.read_uint8_t();
	assert(v1 == 8);
	const uint16_t v2 = t0.read_uint16_t();
	assert(v2 == 16);
	const uint32_t v3 = t0.read_uint32_t();
	assert(v3 == 32);
	const uint64_t v4 = t0.read_uint64_t();
	assert(v4 == 64);
	const int8_t v5 = t0.read_int8_t();
	assert(v5 == 8);
	const int16_t v6 = t0.read_int16_t();
	assert(v6 == 16);
	const int32_t v7 = t0.read_int32_t();
	assert(v7 == 32);
	const int64_t v8 = t0.read_int64_t();
	assert(v8 == 64);
	std::vector<uint64_t> val64s(20);
	t0.read_array(val64s.data(), val64s.size());
	assert(val64s.at(18) == 18);
	const char *str = t0.read_string();
	assert(strcmp(str, "test1") == 0);
	str = t0.read_string();
	assert(strcmp(str, "test2") == 0);
	std::vector<uint16_t> big(65535, 0);
	t0.read_array(big.data(), big.size());
	assert(big.at(10000) == 99);

	const char* const* strings = t0.read_string_array(3);
	assert(strcmp(strings[0], "first") == 0);
	assert(strcmp(strings[1], "second") == 0);
	assert(strcmp(strings[2], "third") == 0);

	std::vector<uint8_t> vals1 = { 1, 2, 3, 4, 5 };
	t0.read_patch((char*)vals1.data(), vals1.size());
	assert(vals1[0] == 1);
	assert(vals1[1] == 0);
	assert(vals1[2] == 3);
	assert(vals1[3] == 4);
	assert(vals1[4] == 5);

	// test no change
	std::vector<uint8_t> vals2 = { 1, 2, 3, 4, 5 };
	t0.read_patch((char*)vals2.data(), vals2.size());
	assert(vals2[0] == 1);
	assert(vals2[1] == 2);
	assert(vals2[2] == 3);
	assert(vals2[3] == 4);
	assert(vals2[4] == 5);

	t0.read_patch((char*)big.data(), big.size());
	assert(big.at(63) == 99);
	assert(big.at(64) == 77);
	assert(big.at(65) == 99);
	assert(big.at(80) == 78);
	assert(big.at(81) == 99);

	t0.read_patch((char*)big.data(), big.size());
	assert(big.at(63) == 99);
	assert(big.at(64) == 77);
	assert(big.at(65) == 99);
	assert(big.at(80) == 78);
	assert(big.at(81) == 99);
}

int main()
{
	read_test_1();
	unlink("write_4.bin");
	unlink("write_4-2.bin");
	return 0;
}
