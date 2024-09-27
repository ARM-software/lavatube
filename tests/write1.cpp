#include <limits>
#include <cmath>
#include <vector>
#include <string>

#include "util.h"
#include "read.h"
#include "write.h"

#include "tests/tests.h"

static int patch(lava_file_writer& file, char* orig, char* change, uint64_t offset, uint64_t size)
{
	return file.write_patch(orig, change, offset, size);
}

static void write_test_1_2()
{
	std::vector<char> big1(65535, 0);
	std::vector<char> big2(65535, 42);
	char* orig = big1.data();
	char* mod = big2.data();

	lava_writer& writer = lava_writer::instance();
	writer.set("write_1_2", 1);
	lava_file_writer& file = writer.file_writer();

	file.write_memory(orig, 0, 65535);
	int changed = patch(file, orig, mod, 1024, 1024);
	assert(orig[1023] == 0);
	assert(orig[1024] == 42);
	assert(mod[1023] == 42);
	assert(mod[1024] == 42);
	assert(changed == 1024); // since is perfectly aligned, should get perfect result

	changed = patch(file, orig, mod, 1024, 1024);
	assert(orig[1023] == 0);
	assert(orig[1024] == 42);
	assert(changed == 0); // since is perfectly aligned, should get perfect result

	changed = patch(file, orig, mod, 4096, 1024);
	assert(orig[1023] == 0);
	assert(changed == 1024); // as above
	changed = patch(file, orig, mod, 4096, 1024);
	assert(changed == 0); // as above

	writer.serialize();
	writer.finish();
}

void read_test_1_2()
{
	std::vector<char> big1(65535, 0);
	char* buf = big1.data();

	lava_reader r("write_1_2.vk");
	lava_file_reader& t0 = r.file_reader(0);
	const uint8_t barrier_packet = t0.read_uint8_t();
	assert(barrier_packet == 3);
	t0.read_barrier();
	int changed = t0.read_patch((char*)buf, 65535); // all zeroes
	assert(changed == 65535);
	assert(buf[0] == 0);
	assert(buf[1024] == 0);

	changed = t0.read_patch((char*)buf, 65535);
	assert(changed == 1024);
	assert(buf[1023] == 0);
	assert(buf[1024] == 42);
	assert(buf[2047] == 42);
	assert(buf[2049] == 0);
	assert(buf[4096] == 0);

	changed = t0.read_patch((char*)buf, 65535);
	assert(changed == 0);

	changed = t0.read_patch((char*)buf, 65535);
	assert(changed == 1024);
	assert(buf[4096] == 42);

	changed = t0.read_patch((char*)buf, 65535);
	assert(changed == 0);
}

static void write_test_1()
{
	std::vector<uint64_t> val64s = { 0,1,2,3,4,5,6,7,8,9 };
	char* clone; // copy of original
	int changed = 0;
	std::vector<char> big(65535, 42);

	lava_writer& writer = lava_writer::instance();
	writer.set("write_1", 1);
	lava_file_writer& file = writer.file_writer();
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

	VkClearColorValue col;
	col.float32[0] = 1.0f;
	col.float32[1] = std::numeric_limits<float>::infinity();
	col.float32[2] = 0.25f;
	col.float32[3] = std::numeric_limits<float>::quiet_NaN();
	file.write_array((const float*)&col, 4);

	const char* const strings[3] = {
		"first", "second", "third"
	};
	file.write_string_array(strings, 3);

	// start testing patching

	std::vector<uint8_t> vals1 = { 1, 2, 3, 4, 5 }; // original
	std::vector<uint8_t> vals2 = { 1, 0, 3, 4, 5 }; // changed
	clone = (char*)vals1.data(); // copy of original
	char* ptr = (char*)vals2.data(); // newly changed buffer
	changed = file.write_patch(clone, ptr, 0, 5);
	assert(changed <= 5 && changed >= 1); // exactly how many bytes we write out depends on implementation details
	// test no change
	ptr = (char*)vals1.data();
	changed = file.write_patch(clone, ptr, 0, 5);
	assert(changed == 0);

	// big data patches

	const uint64_t size = 128;
	const uint64_t offset = 32;
	std::vector<char> cloned(size);
	clone = (char*)cloned.data();
	memcpy(clone, big.data(), size);
	assert(cloned.at(1) == 42);
	big[64] = 77; // make a divergence at byte 128
	big[80] = 78; // make a divergence at byte 160
	assert(big.at(63) == 42);
	assert(big.at(64) == 77);
	assert(big.at(65) == 42);
	assert(big.at(80) == 78);
	assert(big.at(81) == 42);
	const int c2 = file.write_patch(clone, big.data(), offset, size - offset);
	assert(c2 > 0);
	changed = file.write_patch(clone, big.data(), offset, size - offset); // no-op
	assert(changed == 0);

	big[64] = 42;
	big[80] = 42;
	changed = file.write_patch(clone, big.data(), offset, size - offset); // reset back to all 42s
	assert(changed > 0);

	big[0] = 0;
	big[1] = 0;
	big[2] = 0;
	big[3] = 0;
	memset(clone, 0, size);
	changed = file.write_patch(clone, big.data(), 0, 4);
	assert(changed == 0);

	big[0] = 0;
	big[1] = 0;
	big[2] = 42;
	big[3] = 42;
	memset(clone, 0, size);
	changed = file.write_patch(clone, big.data(), 0, 4);
	assert(changed >= 2);

	big[0] = 42;
	big[1] = 42;
	big[2] = 42;
	big[3] = 42;
	memset(clone, 0, size);
	changed = file.write_patch(clone, big.data(), 0, 4);
	assert(changed >= 4);

	big[0] = 0;
	big[1] = 42;
	big[2] = 42;
	big[3] = 0;
	memset(clone, 0, size);
	changed = file.write_patch(clone, big.data(), 0, 4);
	assert(changed >= 2);

	memset(big.data(), 0, size);
	big[1] = 42;
	big[2] = 42;
	big[10] = 42;
	big[12] = 42;
	memset(clone, 0, size);
	changed = file.write_patch(clone, big.data(), 0, 14);
	assert(changed >= 4);

	writer.serialize();
	writer.finish();
}

void read_test_1()
{
	lava_reader r("write_1.vk");
	lava_file_reader& t0 = r.file_reader(0);
	const uint8_t barrier_packet = t0.read_uint8_t();
	assert(barrier_packet == 3);
	t0.read_barrier();
	std::vector<char> big(65535, 0);
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
	std::vector<uint64_t> val64s(10);
	t0.read_array(val64s.data(), val64s.size());
	assert(val64s.at(9) == 9);
	const char *str = t0.read_string();
	assert(strcmp(str, "test1") == 0);
	str = t0.read_string();
	assert(strcmp(str, "test2") == 0);
	t0.read_array(big.data(), big.size());
	assert(big.at(100) == 42);

	VkClearColorValue wcol;
	wcol.float32[0] = 1.0f;
	wcol.float32[1] = std::numeric_limits<float>::infinity();
	wcol.float32[2] = 0.25f;
	wcol.float32[3] = std::numeric_limits<float>::quiet_NaN();
	VkClearColorValue rcol;
	t0.read_array(rcol.float32, 4);
	assert(wcol.float32[0] == rcol.float32[0]);
	assert(wcol.float32[2] == rcol.float32[2]);
	assert(wcol.uint32[0] == rcol.uint32[0]);
	assert(wcol.uint32[2] == rcol.uint32[2]);
	assert(std::isinf(rcol.float32[1]));
	assert(std::isnan(rcol.float32[3]));

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

	// big data patches
	t0.read_patch((char*)big.data(), big.size());
	assert(big.at(63) == 42);
	assert(big.at(64) == 77);
	assert(big.at(65) == 42);
	assert(big.at(80) == 78);
	assert(big.at(81) == 42);

	t0.read_patch((char*)big.data(), big.size()); // no change
	assert(big.at(63) == 42);
	assert(big.at(64) == 77);
	assert(big.at(65) == 42);
	assert(big.at(80) == 78);
	assert(big.at(81) == 42);

	t0.read_patch((char*)big.data(), big.size()); // revert to all 42s
	assert(big.at(63) == 42);
	assert(big.at(64) == 42);
	assert(big.at(65) == 42);
	assert(big.at(80) == 42);
	assert(big.at(81) == 42);

	memset(big.data(), 0, big.size());
	t0.read_patch((char*)big.data(), 4);
	assert(big.at(0) == 0);
	assert(big.at(1) == 0);
	assert(big.at(2) == 0);
	assert(big.at(3) == 0);

	memset(big.data(), 0, big.size());
	t0.read_patch((char*)big.data(), 4);
	assert(big.at(0) == 0);
	assert(big.at(1) == 0);
	assert(big.at(2) == 42);
	assert(big.at(3) == 42);

	memset(big.data(), 0, big.size());
	t0.read_patch((char*)big.data(), 4);
	assert(big.at(0) == 42);
	assert(big.at(1) == 42);
	assert(big.at(2) == 42);
	assert(big.at(3) == 42);

	memset(big.data(), 0, big.size());
	t0.read_patch((char*)big.data(), 4);
	assert(big.at(0) == 0);
	assert(big.at(1) == 42);
	assert(big.at(2) == 42);
	assert(big.at(3) == 0);

	memset(big.data(), 0, big.size());
	t0.read_patch((char*)big.data(), 14);
	assert(big.at(0) == 0);
	assert(big.at(1) == 42);
	assert(big.at(2) == 42);
	assert(big.at(3) == 0);
	assert(big.at(9) == 0);
	assert(big.at(10) == 42);
	assert(big.at(11) == 0);
	assert(big.at(12) == 42);
	assert(big.at(13) == 0);
}

int main()
{
	ILOG("write_test_1");
	write_test_1();
	sync();
	read_test_1();

	ILOG("write_test_1_2");
	write_test_1_2();
	sync();
	read_test_1_2();
	return 0;
}
