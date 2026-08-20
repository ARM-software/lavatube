#include <string>
#include <vector>

#include "lavatube.h"
#include "packfile.h"
#include "random_access_file_reader.h"
#include "util.h"
#include "write.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

static uint32_t large_trace_payload_size(uint32_t packet, uint32_t chunk_size)
{
	if (packet % 31 == 0) return chunk_size * 3 + 17 + packet % 29;
	if (packet % 13 == 0) return chunk_size + 5 + packet % 11;
	return 1 + (packet * 37) % 127;
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

static void initialize_large_trace_instance(lava_writer& writer)
{
	const change_source source = { 0, 0, 0, PACKET_THREAD_BARRIER };

	trackable* instance = writer.records.VkInstance_index.add(fake_handle<VkInstance>(0), source, 0);
	instance->enter_created();
}

int main()
{
	const std::string trace_filename = "large_trace.api";
	const uint32_t chunk_size = 1024;
	const uint32_t packet_count = 512;
	const uint_fast8_t saved_compression_type = p__compression_type;
	const uint_fast16_t saved_compression_level = p__compression_level;
	std::vector<uint64_t> packet_positions(packet_count);

	p__compression_type = LAVATUBE_COMPRESSION_LZ4;
	p__compression_level = 1;

	lava_writer& trace_writer = lava_writer::instance();
	initialize_large_trace_instance(trace_writer);
	trace_writer.set_output(trace_filename);
	trace_writer.prepare_threads(1);
	trace_writer.bind_thread(0);

	{
		lava_file_writer& writer = trace_writer.file_writer();
		writer.change_default_chunk_size(chunk_size);

		for (uint32_t packet = 0; packet < packet_count; packet++)
		{
			packet_positions.at(packet) = writer.uncompressed_bytes;
			const uint32_t payload_size = large_trace_payload_size(packet, chunk_size);
			std::vector<char> payload(payload_size);
			for (uint32_t byte = 0; byte < payload_size; byte++)
			{
				payload.at(byte) = static_cast<char>(large_trace_payload_byte(packet, byte));
			}
			writer.begin_packet(PACKET_THREAD_BARRIER);
			writer.write_uint8_t(0);
			writer.write_array(payload.data(), payload.size());
			writer.end_packet();
		}
	}
	trace_writer.serialize();
	trace_writer.finish();

	{
		random_access_file_reader reader(packed_open("thread_0.bin", trace_filename));
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
			const uint8_t packet_type = reader.read_uint8_t();
			assert(packet_type == PACKET_THREAD_BARRIER);
			const uint64_t packet_end = packet_positions.at(packet) + reader.read_uint32_t();
			if (packet_crosses_chunk_boundary(reader, packet_positions.at(packet), packet_end)) crossing_packet_count++;

			const uint8_t thread = reader.read_uint8_t();
			assert(thread == 0);
			for (uint32_t byte = 0; byte < payload_size; byte++)
			{
				const uint8_t value = reader.read_uint8_t();
				assert(value == large_trace_payload_byte(packet, byte));
			}
			assert(reader.position() == packet_end);
		}

		assert(reader.position() == reader.size());
		assert(small_packet_count > 0);
		assert(big_packet_count > 0);
		assert(crossing_packet_count > 0);

		const Json::Value frames = packed_json("frames_0.json", trace_filename);
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
			const uint64_t found_packet = reader.seek_to_packet(packet);
			assert(found_packet == checkpoint.packet);
			assert(reader.position() == checkpoint.position);
		}

		assert(frames["frames"].empty());
		assert(frames["uncompressed_size"].asUInt64() == reader.size());
		assert(frames["uncompressed_sizes"].size() == reader.chunks().size());
		assert(frames["compressed_sizes"].size() == reader.chunks().size());
		assert(frames["highest_global_frame"].asUInt() == 0);
	}

	p__compression_type = saved_compression_type;
	p__compression_level = saved_compression_level;
	return 0;
}
