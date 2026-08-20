#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "filewriter.h"
#include "packfile.h"
#include "random_access_file_reader.h"
#include "util.h"

#include "tests/tests.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

static std::vector<uint8_t> make_random_access_payload(size_t payload_size)
{
	std::vector<uint8_t> payload(payload_size);
	for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<uint8_t>((i * 37 + i / 7) % 251);
	return payload;
}

static void write_random_access_payload(const std::string& filename, const std::vector<uint8_t>& payload,
	uint_fast8_t compression_algorithm, size_t chunk_size)
{
	p__compression_type = compression_algorithm;
	p__compression_level = 0;
	file_writer writer(0);
	writer.change_default_chunk_size(chunk_size);
	writer.set(filename);
	for (uint8_t value : payload) writer.write_uint8_t(value);
	writer.finalize();
}

static void check_chunk_directory(const random_access_file_reader& reader)
{
	const std::vector<random_access_chunk_info>& chunks = reader.chunks();
	assert(chunks.size() > 3);
	uint64_t expected_uncompressed_offset = 0;
	uint64_t previous_compressed_end = 0;
	for (const random_access_chunk_info& info : chunks)
	{
		assert(info.compressed_size > 0);
		assert(info.uncompressed_size > 0);
		assert(info.uncompressed_offset == expected_uncompressed_offset);
		assert(info.compressed_offset >= previous_compressed_end);
		expected_uncompressed_offset += info.uncompressed_size;
		previous_compressed_end = info.compressed_offset + info.compressed_size;
	}
	assert(expected_uncompressed_offset == reader.size());
}

static void test_direct_random_access(uint_fast8_t compression_algorithm)
{
	const std::string filename = "random_access_reader_" + std::to_string(compression_algorithm) + ".bin";
	const std::vector<uint8_t> payload = make_random_access_payload(4096);
	write_random_access_payload(filename, payload, compression_algorithm, 256);

	{
		random_access_file_reader reader(filename);
		assert(reader.size() == payload.size());
		assert(reader.position() == 0);
		Json::Value legacy_frame_info;
		legacy_frame_info["frames"] = Json::arrayValue;
		reader.load_packet_checkpoints(legacy_frame_info);
		assert(!reader.has_packet_checkpoints());
		reader.seek(123);
		const uint64_t packet = reader.seek_to_packet(1000);
		assert(packet == 0);
		assert(reader.position() == 0);
		assert(reader.remaining() == payload.size());
		assert(reader.compression_algorithm() == compression_algorithm);
		assert(reader.version() == 3);
		assert(reader.decompression_count() == 0);
		check_chunk_directory(reader);

		const uint64_t late_position = payload.size() - 100;
		reader.seek(late_position);
		std::vector<uint8_t> late(64);
		reader.read_array(late.data(), late.size());
		assert(std::equal(late.begin(), late.end(), payload.begin() + late_position));
		assert(reader.decompression_count() == 1);

		reader.seek(late_position + 8);
		const uint32_t stored = reader.read_uint32_t();
		uint32_t expected = 0;
		memcpy(&expected, payload.data() + late_position + 8, sizeof(expected));
		assert(stored == expected);
		assert(reader.decompression_count() == 1);

		reader.seek(17);
		const uint8_t value = reader.read_uint8_t();
		assert(value == payload[17]);
		assert(reader.decompression_count() == 2);

		reader.seek(payload.size());
		assert(reader.remaining() == 0);
	}

	{
		random_access_file_reader reader(filename);
		const random_access_chunk_info& first = reader.chunks().front();
		assert(first.uncompressed_size > 16);
		const uint64_t cross_position = first.uncompressed_offset + first.uncompressed_size - 8;
		reader.seek(cross_position);
		std::vector<uint8_t> crossing(32);
		reader.read_array(crossing.data(), crossing.size());
		assert(std::equal(crossing.begin(), crossing.end(), payload.begin() + cross_position));
		assert(reader.decompression_count() == 2);

		const uint64_t exact_boundary = reader.chunks().at(2).uncompressed_offset;
		reader.seek(exact_boundary);
		const uint8_t value = reader.read_uint8_t();
		assert(value == payload[exact_boundary]);
		assert(reader.decompression_count() == 3);
	}

	unlink(filename.c_str());
}

static void test_packed_random_access()
{
	const std::string directory = "random_access_reader_pack_dir";
	const std::string filename = directory + "/thread_0.bin";
	const std::string pack = "random_access_reader_pack.api";
	const std::vector<uint8_t> payload = make_random_access_payload(2048);

	unlink(filename.c_str());
	rmdir(directory.c_str());
	unlink(pack.c_str());
	const int mkdir_result = mkdir(directory.c_str(), 0777);
	assert(mkdir_result == 0);
	write_random_access_payload(filename, payload, LAVATUBE_COMPRESSION_LZ4, 128);
	const int r = pack_directory(pack, directory, true);
	assert(r);

	{
		random_access_file_reader reader(packed_open("thread_0.bin", pack));
		assert(reader.size() == payload.size());
		reader.seek(1000);
		std::vector<uint8_t> output(400);
		reader.read_array(output.data(), output.size());
		assert(std::equal(output.begin(), output.end(), payload.begin() + 1000));
		reader.seek(3);
		const unsigned v = reader.read_uint8_t();
		assert(v == payload[3]);
	}

	unlink(pack.c_str());
}

static void copy_without_stream_header(const std::string& source, const std::string& destination)
{
	const int source_fd = open(source.c_str(), O_RDONLY | default_file_flags);
	assert(source_fd >= 0);
	struct stat status;
	int r = fstat(source_fd, &status);
	assert(r == 0);
	const size_t header_size = strlen("LAVABIN") + 32;
	assert(status.st_size > static_cast<off_t>(header_size));
	std::vector<char> contents(static_cast<size_t>(status.st_size) - header_size);
	size_t consumed = 0;
	while (consumed < contents.size())
	{
		const ssize_t result = pread(source_fd, contents.data() + consumed, contents.size() - consumed, header_size + consumed);
		if (result < 0 && errno == EINTR) continue;
		assert(result > 0);
		consumed += result;
	}
	r = close(source_fd);
	assert(r == 0);

	const int destination_fd = open(destination.c_str(), O_CREAT | O_TRUNC | O_WRONLY | default_file_flags, 0664);
	assert(destination_fd >= 0);
	size_t written = 0;
	while (written < contents.size())
	{
		const ssize_t result = write(destination_fd, contents.data() + written, contents.size() - written);
		if (result < 0 && errno == EINTR) continue;
		assert(result > 0);
		written += result;
	}
	r = close(destination_fd);
	assert(r == 0);
}

static void test_legacy_headerless_density_stream()
{
	const std::string versioned = "random_access_reader_versioned.bin";
	const std::string legacy = "random_access_reader_legacy.bin";
	const std::vector<uint8_t> payload = make_random_access_payload(1024);
	write_random_access_payload(versioned, payload, LAVATUBE_COMPRESSION_DENSITY, 128);
	copy_without_stream_header(versioned, legacy);

	{
		random_access_file_reader reader(legacy);
		assert(reader.version() == 0);
		assert(reader.compression_algorithm() == LAVATUBE_COMPRESSION_DENSITY);
		reader.seek(333);
		std::vector<uint8_t> output(300);
		reader.read_array(output.data(), output.size());
		assert(std::equal(output.begin(), output.end(), payload.begin() + 333));
	}

	unlink(versioned.c_str());
	unlink(legacy.c_str());
}

int main()
{
	const uint_fast8_t saved_compression_type = p__compression_type;
	const uint_fast16_t saved_compression_level = p__compression_level;

	test_direct_random_access(LAVATUBE_COMPRESSION_UNCOMPRESSED);
	test_direct_random_access(LAVATUBE_COMPRESSION_DENSITY);
	test_direct_random_access(LAVATUBE_COMPRESSION_LZ4);
	test_packed_random_access();
	test_legacy_headerless_density_stream();

	p__compression_type = saved_compression_type;
	p__compression_level = saved_compression_level;
	return 0;
}
