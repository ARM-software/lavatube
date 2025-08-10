// Free-standing file writer test

#include <limits>
#include <cmath>
#include <vector>
#include <string>

#include "filewriter.h"

#include "tests/tests.h"

static void write_test_1()
{
	std::vector<uint64_t> val64s = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19 };
	std::vector<uint16_t> big(65535, 99);

	file_writer file(0);
	file.set("write_4.bin");
	file.write_uint8_t(8);
	file.write_uint16_t(16);
	file.write_uint32_t(32);
	file.write_uint64_t(64);
	file.write_int8_t(8);
	file.write_int16_t(16);
	file.write_int32_t(32);
	file.write_int64_t(64);
	file.write_array(val64s.data(), val64s.size());
	file.write_string("test1");
	file.write_string(std::string("test2"));
	file.write_array(big.data(), big.size());

	const char* const strings[3] = {
		"first", "second", "third"
	};
	file.write_string_array(strings, 3);

	std::vector<uint8_t> vals1 = { 1, 2, 3, 4, 5 }; // original
	std::vector<uint8_t> vals2 = { 1, 0, 3, 4, 5 }; // changed
	int changed = file.write_patch((char*)vals1.data(), (const char*)vals2.data(), 0, 5);
	assert(changed <= 5 && changed >= 1);

	// test no change
	changed = file.write_patch((char*)vals1.data(), (const char*)vals2.data(), 0, 5);
	assert(changed == 0);

	std::vector<uint16_t> cloned(256);
	memcpy(cloned.data(), big.data(), cloned.size());
	big[64] = 77; // make a divergence at byte 128
	big[80] = 78; // make a divergence at byte 160
	assert(big.at(63) == 99);
	assert(big.at(64) == 77);
	assert(big.at(65) == 99);
	assert(big.at(80) == 78);
	changed = file.write_patch((char*)cloned.data(), (const char*)big.data(), 128, 64);
	assert(changed <= 16 && changed >= 1);
	assert(big.at(63) == 99);
	assert(big.at(64) == 77);
	assert(big.at(65) == 99);
	file.write_memory((const char*)big.data(), 128, 64);
	assert(big.at(63) == 99);
	assert(big.at(64) == 77);
	assert(big.at(65) == 99);
	assert(big.at(80) == 78);
}

static void write_test_2()
{
	file_writer file(0);
	file.write_uint8_t(8);
	file.write_uint16_t(16);
	file.write_uint32_t(32);
	file.write_uint64_t(64);
	file.write_int8_t(8);
	file.set("write_4-2.bin");
	file.write_int8_t(8);
}

static void write_test_3()
{
	file_writer file(0);
	file.write_uint8_t(8);
	file.write_uint16_t(16);
	file.write_uint32_t(32);
	file.write_uint64_t(64);
}

static void write_test_4()
{
	std::vector<uint16_t> big(5000, 99);
	file_writer file(0);

	assert(file.count_held_chunks() == 0);
	assert(file.count_compressed_chunks() == 0);
	assert(file.count_uncompressed_chunks() == 0);
	file.change_default_chunk_size(500);
	file.set("write_4_4.bin");
	uint8_t* p8 = file.write_later_uint8_t(8);
	uint16_t* p16 = file.write_later_uint16_t(16);
	uint32_t* p32 = file.write_later_uint32_t(32);
	uint64_t* p64 = file.write_later_uint64_t(64);
	assert(*p8 == 8);
	assert(*p16 == 16);
	assert(*p32 == 32);
	assert(*p64 == 64);
	file.write_array(big.data(), big.size()); // blow out current chunk
	uint64_t* p64_2 = file.write_later_uint64_t();
	assert(*p64_2 == 0);
	*p64_2 = 65;
	file.write_array(big.data(), big.size());
	file.write_uint64_t(64);
	assert(*p8 == 8);
	assert(*p16 == 16);
	assert(*p32 == 32);
	assert(*p64 == 64);
	assert(*p64_2 == 65);
	*p8 = 1;
	*p16 = 2;
	*p32 = 3;
	*p64 = 4;
	assert(file.count_compressed_chunks() == 0);
	assert(file.count_uncompressed_chunks() == 0);
	file.thaw();
	assert(file.count_held_chunks() == 0);
}

int main()
{
	write_test_1();
	write_test_2();
	write_test_3();
	write_test_4();
	return 0;
}
