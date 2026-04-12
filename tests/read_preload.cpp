#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <unistd.h>

#include "filewriter.h"
#include "filereader.h"
#include "util.h"

#include "tests/tests.h"

static std::vector<uint8_t> make_payload(size_t size)
{
	std::vector<uint8_t> payload(size);
	for (size_t i = 0; i < size; i++) payload[i] = (uint8_t)((i * 37) % 251);
	return payload;
}

static void write_payload(const std::string& filename, const std::vector<uint8_t>& payload, size_t chunk_size)
{
	file_writer file(0);
	file.change_default_chunk_size(chunk_size);
	file.set(filename);
	file.write_array(payload.data(), payload.size());
	file.finalize();
}

static void test_preload0_cross_chunk_read()
{
	const std::string filename = "read_preload_chunks.bin";
	const std::vector<uint8_t> payload = make_payload(4096);

	p__preload = 0;
	p__allow_stalls = 1;
	write_payload(filename, payload, 256);

	{
		file_reader reader(filename, 0, payload.size(), payload.size());
		reader.self_test();

		std::vector<uint8_t> out(payload.size(), 0);
		size_t offset = 0;
		while (offset < out.size())
		{
			const size_t chunk = std::min<size_t>(300, out.size() - offset);
			reader.read_array(out.data() + offset, chunk);
			offset += chunk;
		}

		assert(out == payload);
		reader.self_test();
	}

	unlink(filename.c_str());
}

static void test_start_measurement_caps_wait_to_target()
{
	const std::string filename = "read_preload_target.bin";
	const std::vector<uint8_t> payload = make_payload(4096);
	const size_t wanted = 512;

	p__preload = 1;
	p__allow_stalls = 1;
	write_payload(filename, payload, 256);

	{
		file_reader reader(filename, 0, payload.size(), wanted, false);
		reader.self_test();

		const auto start = std::chrono::steady_clock::now();
		reader.start_measurement();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		assert(elapsed < 1000);

		std::vector<uint8_t> out(wanted, 0);
		reader.read_array(out.data(), out.size());
		assert(std::equal(out.begin(), out.end(), payload.begin()));
		reader.self_test();
	}

	unlink(filename.c_str());
}

int main()
{
	const uint_fast16_t saved_preload = p__preload;
	const uint_fast8_t saved_allow_stalls = p__allow_stalls;

	test_preload0_cross_chunk_read();
	test_start_measurement_caps_wait_to_target();

	p__preload = saved_preload;
	p__allow_stalls = saved_allow_stalls;
	return 0;
}
