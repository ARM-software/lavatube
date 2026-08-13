#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include <jsoncpp/json/reader.h>

#include "lavatube.h"
#include "random_access_file_reader.h"
#include "util.h"
#include "write.h"

static uint32_t large_trace_payload_size(uint32_t packet, uint32_t chunk_size)
{
	if (packet % 31 == 0) return chunk_size * 3 + 17 + packet % 29;
	if (packet % 13 == 0) return chunk_size + 5 + packet % 11;
	return 1 + (packet * 37) % 127;
}

static uint32_t large_trace_payload_offset(uint32_t packet)
{
	return packet * 4096;
}

static uint8_t large_trace_payload_byte(uint32_t packet, uint32_t byte)
{
	return static_cast<uint8_t>((packet * 17 + byte * 29 + byte / 7) % 251);
}

static bool packet_crosses_chunk_boundary(const random_access_file_reader& reader, uint64_t start, uint64_t end)
{
	for (const random_access_chunk_info& chunk : reader.chunks())
	{
		if (chunk.uncompressed_offset > start && chunk.uncompressed_offset < end) return true;
	}
	return false;
}

static Json::Value read_large_trace_json(const std::string& filename)
{
	std::ifstream stream(filename);
	assert(stream.good());
	Json::Value value;
	Json::Reader reader;
	assert(reader.parse(stream, value, false));
	return value;
}

static void initialize_large_trace_objects(trackeddevice& device, trackedbuffer& buffer)
{
	const change_source source = { 0, 0, 0, PACKET_BUFFER_UPDATE2 };

	device.index = 3;
	device.creation = source;
	device.last_modified = source;
	device.enter_created();

	buffer.index = 7;
	buffer.creation = source;
	buffer.last_modified = source;
	buffer.object_type = VK_OBJECT_TYPE_BUFFER;
	buffer.size = 4 * 1024 * 1024;
	buffer.flags = 0;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.enter_created();
}

int main()
{
	const std::string directory = "large_trace_test_data";
	const std::string stream_filename = directory + "/thread_0.bin";
	const std::string frames_filename = directory + "/frames_0.json";
	const uint32_t chunk_size = 1024;
	const uint32_t packet_count = 512;
	const uint32_t second_frame_packet = packet_count / 2;
	const uint_fast8_t saved_compression_type = p__compression_type;
	const uint_fast16_t saved_compression_level = p__compression_level;
	std::vector<uint64_t> packet_positions(packet_count);

	unlink(stream_filename.c_str());
	unlink(frames_filename.c_str());
	rmdir(directory.c_str());
	assert(mkdir(directory.c_str(), 0777) == 0);

	p__compression_type = LAVATUBE_COMPRESSION_LZ4;
	p__compression_level = 1;

	trackeddevice device;
	trackedbuffer buffer;
	initialize_large_trace_objects(device, buffer);

	{
		lava_file_writer writer(0, &lava_writer::instance(), false);
		writer.change_default_chunk_size(chunk_size);
		frame_mutex.lock();
		writer.set(directory);
		writer.new_frame(0);
		frame_mutex.unlock();

		for (uint32_t packet = 0; packet < packet_count; packet++)
		{
			if (packet == second_frame_packet)
			{
				frame_mutex.lock();
				writer.new_frame(1);
				frame_mutex.unlock();
			}

			packet_positions.at(packet) = writer.uncompressed_bytes;
			const uint32_t payload_size = large_trace_payload_size(packet, chunk_size);
			std::vector<char> payload(payload_size);
			for (uint32_t byte = 0; byte < payload_size; byte++)
			{
				payload.at(byte) = static_cast<char>(large_trace_payload_byte(packet, byte));
			}
			const uint64_t written = write_object_update_packet(writer, &device, &buffer,
				large_trace_payload_offset(packet), payload.data(), payload_size);
			assert(written == payload_size);
		}
	}

	{
		random_access_file_reader reader(stream_filename);
		assert(reader.chunks().size() > 32);
		uint32_t small_packet_count = 0;
		uint32_t big_packet_count = 0;
		uint32_t crossing_packet_count = 0;

		for (uint32_t packet = 0; packet < packet_count; packet++)
		{
			const uint32_t payload_size = large_trace_payload_size(packet, chunk_size);
			if (payload_size < chunk_size) small_packet_count++;
			else big_packet_count++;

			assert(reader.position() == packet_positions.at(packet));
			assert(reader.read_uint8_t() == PACKET_BUFFER_UPDATE2);
			const uint64_t packet_end = packet_positions.at(packet) + reader.read_uint32_t();
			if (packet_crosses_chunk_boundary(reader, packet_positions.at(packet), packet_end)) crossing_packet_count++;

			assert(reader.read_uint32_t() == device.index);
			assert(reader.read_int8_t() == device.last_modified.thread);
			assert(reader.read_uint32_t() == device.last_modified.packet);
			assert(reader.read_uint32_t() == buffer.index);
			assert(reader.read_int8_t() == buffer.last_modified.thread);
			assert(reader.read_uint32_t() == buffer.last_modified.packet);
			assert(reader.read_uint64_t() == payload_size + 18);
			assert(reader.read_uint16_t() == 0);
			assert(reader.read_uint32_t() == large_trace_payload_offset(packet));
			assert(reader.read_uint32_t() == payload_size);
			for (uint32_t byte = 0; byte < payload_size; byte++)
			{
				assert(reader.read_uint8_t() == large_trace_payload_byte(packet, byte));
			}
			assert(reader.read_uint32_t() == 0);
			assert(reader.read_uint32_t() == 0);
			assert(reader.position() == packet_end);
		}

		assert(reader.position() == reader.size());
		assert(small_packet_count > 0);
		assert(big_packet_count > 0);
		assert(crossing_packet_count > 0);

		const Json::Value frames = read_large_trace_json(frames_filename);
		reader.load_packet_checkpoints(frames);
		assert(reader.has_packet_checkpoints());
		assert(reader.packet_checkpoints().size() > 32);
		assert(reader.packet_checkpoints().size() < packet_count);
		size_t checkpoint_index = 0;
		for (uint32_t packet = 0; packet < packet_count; packet++)
		{
			while (checkpoint_index + 1 < reader.packet_checkpoints().size()
			       && reader.packet_checkpoints().at(checkpoint_index + 1).packet <= packet)
			{
				checkpoint_index++;
			}
			const packet_checkpoint& checkpoint = reader.packet_checkpoints().at(checkpoint_index);
			assert(checkpoint.packet <= packet);
			assert(checkpoint.position == packet_positions.at(checkpoint.packet));
			assert(reader.seek_to_packet(packet) == checkpoint.packet);
			assert(reader.position() == checkpoint.position);
		}

		assert(frames["frames"].size() == 2);
		assert(frames["frames"][0]["global_frame"].asInt() == 0);
		assert(frames["frames"][0]["local_frame"].asInt() == -1);
		assert(frames["frames"][0]["position"].asUInt64() == packet_positions.at(0));
		assert(frames["frames"][0]["packet"].asUInt() == 0);
		assert(frames["frames"][1]["global_frame"].asInt() == 1);
		assert(frames["frames"][1]["local_frame"].asInt() == 0);
		assert(frames["frames"][1]["position"].asUInt64() == packet_positions.at(second_frame_packet));
		assert(frames["frames"][1]["packet"].asUInt() == second_frame_packet);
		assert(frames["uncompressed_size"].asUInt64() == reader.size());
		assert(frames["uncompressed_sizes"].size() == reader.chunks().size());
		assert(frames["compressed_sizes"].size() == reader.chunks().size());
		assert(frames["highest_global_frame"].asUInt() == 1);
	}

	p__compression_type = saved_compression_type;
	p__compression_level = saved_compression_level;
	assert(unlink(stream_filename.c_str()) == 0);
	assert(unlink(frames_filename.c_str()) == 0);
	assert(rmdir(directory.c_str()) == 0);
	return 0;
}
