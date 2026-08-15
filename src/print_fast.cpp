#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "json_helpers.h"
#include "packfile.h"
#include "random_access_file_reader.h"
#include "read.h"
#include "read_auto.h"
#include "sandbox.h"
#include "util.h"

struct fast_frame_boundary
{
	uint64_t position = 0;
	uint32_t frame = 0;
};

static bool verbose = false;

void usage()
{
	printf("lava-print-fast %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-print-fast [options] --thread NUM <input filename>\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
	printf("-f/--frames start end  Select a frame range\n");
	printf("-t/--thread NUM        Print packets from this thread (required)\n");
	printf("--select LIST          Print packet indices, comma-separated; optional :THREAD must match --thread\n");
	printf("-m/--max NUM           Stop after printing this many entries\n");
	printf("--skip-missing-input   Exit with code 77 if the input trace file does not exist\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
	exit(-1);
}

static uint32_t get_selector_uint32(const std::string& text, const char* label)
{
	if (text.empty()) DIE("Invalid empty %s in --select", label);
	char* end = nullptr;
	errno = 0;
	const unsigned long value = strtoul(text.c_str(), &end, 10);
	if (errno != 0 || *end != '\0' || value >= UINT32_MAX)
	{
		DIE("Invalid %s in --select: %s", label, text.c_str());
	}
	return (uint32_t)value;
}

static void parse_selectors(const char* list, std::vector<print_packet_selector>& selectors)
{
	const std::string text = list;
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t comma = text.find(',', start);
		const size_t end = comma == std::string::npos ? text.size() : comma;
		const std::string entry = text.substr(start, end - start);
		if (entry.empty()) DIE("Invalid empty selector in --select");
		const size_t colon = entry.find(':');
		if (colon != std::string::npos && entry.find(':', colon + 1) != std::string::npos)
		{
			DIE("Invalid selector in --select: %s", entry.c_str());
		}
		print_packet_selector selector;
		if (colon == std::string::npos)
		{
			selector.packet = get_selector_uint32(entry, "packet index");
		}
		else
		{
			selector.packet = get_selector_uint32(entry.substr(0, colon), "packet index");
			selector.thread = get_selector_uint32(entry.substr(colon + 1), "thread index");
		}
		selectors.push_back(selector);
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
}

static bool selector_less(const print_packet_selector& left, const print_packet_selector& right)
{
	return left.packet < right.packet;
}

static void normalize_selectors(std::vector<print_packet_selector>& selectors, uint32_t thread)
{
	for (print_packet_selector& selector : selectors)
	{
		if (selector.thread == UINT32_MAX) selector.thread = thread;
		if (selector.thread != thread)
		{
			DIE("Selector thread %u does not match --thread %u", (unsigned)selector.thread, (unsigned)thread);
		}
	}
	std::sort(selectors.begin(), selectors.end(), selector_less);
	std::vector<print_packet_selector> deduplicated;
	for (const print_packet_selector& selector : selectors)
	{
		if (deduplicated.empty() || deduplicated.back().packet != selector.packet)
		{
			deduplicated.push_back(selector);
		}
	}
	selectors.swap(deduplicated);
}

static std::vector<fast_frame_boundary> load_frame_boundaries(const Json::Value& frame_info)
{
	std::vector<fast_frame_boundary> boundaries;
	if (!frame_info.isMember("frames")) return boundaries;
	boundaries.reserve(frame_info["frames"].size());
	for (const Json::Value& value : frame_info["frames"])
	{
		fast_frame_boundary boundary;
		boundary.position = value["position"].asUInt64();
		boundary.frame = value["global_frame"].asUInt() + 1;
		boundaries.push_back(boundary);
	}
	return boundaries;
}

static uint32_t packet_frame(const std::vector<fast_frame_boundary>& boundaries, uint64_t position)
{
	size_t first = 0;
	size_t last = boundaries.size();
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (boundaries[middle].position <= position) first = middle + 1;
		else last = middle;
	}
	return first == 0 ? 0 : boundaries[first - 1].frame;
}

static bool load_packet(random_access_file_reader& source, uint32_t target_packet,
	uint32_t& current_packet, uint64_t& packet_position, std::vector<char>& packet)
{
	while (current_packet <= target_packet && source.remaining() > 0)
	{
		packet_position = source.position();
		if (source.remaining() < sizeof(uint8_t) + sizeof(uint32_t))
		{
			ABORT("Truncated packet header at packet %u", (unsigned)current_packet);
		}
		(void)source.read_uint8_t();
		const uint32_t packet_size = source.read_uint32_t();
		if (packet_size < sizeof(uint8_t) + sizeof(uint32_t)
		    || packet_size > source.size() - packet_position)
		{
			ABORT("Invalid packet size %u at packet %u", (unsigned)packet_size, (unsigned)current_packet);
		}
		if (current_packet == target_packet)
		{
			packet.resize(packet_size);
			source.seek(packet_position);
			source.read_bytes(packet.data(), packet.size());
			return true;
		}
		source.seek(packet_position + packet_size);
		current_packet++;
	}
	return false;
}

static void print_packet(lava_reader& replayer, lava_file_reader*& reader,
	const std::vector<fast_frame_boundary>& boundaries,
	uint32_t thread, uint32_t packet_index, uint64_t packet_position, uint8_t stream_version,
	const std::vector<char>& packet)
{
	const uint32_t frame = packet_frame(boundaries, packet_position);
	DLOG3("Fast print packet %u on thread %u at position %lu in frame %u", (unsigned)packet_index,
		(unsigned)thread, (unsigned long)packet_position, (unsigned)frame);
	if (!replayer.is_frame_selected(frame)) return;

	if (!reader) reader = new lava_file_reader(&replayer, packet.data(), packet.size(), thread, packet_index, frame, stream_version);
	else reader->reset_fixed_packet(packet.data(), packet.size(), packet_index, frame, stream_version);
	const uint8_t instrtype = reader->step();
	if (instrtype == 0) ABORT("Failed to decode packet %u on thread %u", (unsigned)packet_index, (unsigned)thread);
	switchboard_packet(instrtype, *reader);
	if (instrtype != PACKET_VULKAN_API_CALL && replayer.print_packets && !reader->printed_current_packet)
	{
		callback_context cb_context{ *reader };
		print_params_packet(cb_context);
	}
	reader->complete_packet();
}

int main(int argc, char** argv)
{
	int start_frame = 0;
	int end_frame = -1;
	uint32_t thread = UINT32_MAX;
	std::vector<print_packet_selector> selectors;
	uint32_t max_entries = UINT32_MAX;
	int remaining = argc - 1;
	std::string filename_input;
	bool skip_missing_input = false;
	bool have_select = false;

	if (p__debug_destination == stdout) p__debug_destination = stderr;
	if (p__sandbox_level >= 1) sandbox_level_one();

	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-df", "--debugfile", remaining))
		{
			if (remaining < 1) usage();
			const std::string value = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout && p__debug_destination != stderr) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(value.c_str(), "w");
		}
		else if (match(argv[i], "-f", "--frames", remaining))
		{
			if (remaining < 2) usage();
			start_frame = get_int(argv[++i], remaining);
			end_frame = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-t", "--thread", remaining))
		{
			const int value = get_int(argv[++i], remaining);
			if (value < 0) DIE("Invalid thread index %d", value);
			thread = value;
		}
		else if (match(argv[i], nullptr, "--select", remaining))
		{
			if (have_select) DIE("--select can only be specified once");
			if (remaining < 1) usage();
			const std::string value = get_str(argv[++i], remaining);
			parse_selectors(value.c_str(), selectors);
			have_select = true;
		}
		else if (match(argv[i], "-m", "--max", remaining))
		{
			const int value = get_int(argv[++i], remaining);
			if (value < 0) DIE("Invalid max entries %d", value);
			max_entries = value;
		}
		else if (match(argv[i], "-s", "--sandbox", remaining))
		{
			p__sandbox_level = get_int(argv[++i], remaining);
			if (p__sandbox_level <= 0 || p__sandbox_level > 3) DIE("Invalid sandbox level %d", (int)p__sandbox_level);
		}
		else if (match(argv[i], nullptr, "--skip-missing-input", remaining))
		{
			skip_missing_input = true;
		}
		else if (strcmp(argv[i], "--") == 0)
		{
			remaining--;
			filename_input = get_str(argv[++i], remaining);
			if (remaining > 0) usage();
			break;
		}
		else
		{
			filename_input = get_str(argv[i], remaining);
			if (remaining > 0)
			{
				printf("Invalid options\n\n");
				usage();
			}
		}
	}

	if (filename_input.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}
	if (skip_missing_input && access(filename_input.c_str(), R_OK) != 0)
	{
		printf("SKIP: input trace file does not exist or is not readable: %s\n", filename_input.c_str());
		return 77;
	}
	if (thread == UINT32_MAX) DIE("--thread is required");

	normalize_selectors(selectors, thread);
	const Json::Value metadata = packed_json("metadata.json", filename_input);
	const uint32_t trace_threads = metadata["threads"].asUInt();
	if (thread >= trace_threads)
	{
		DIE("Invalid thread index %u for trace with %u threads", (unsigned)thread, (unsigned)trace_threads);
	}
	const Json::Value frame_info = packed_json("frames_" + std::to_string(thread) + ".json", filename_input);
	const std::vector<fast_frame_boundary> boundaries = load_frame_boundaries(frame_info);

	if (p__sandbox_level >= 3) sandbox_level_two();

	lava_reader replayer;
	replayer.run_type = reader_run_type::isolated;
	replayer.print_packets = true;
	replayer.print_thread_index = thread;
	replayer.print_selectors = selectors;
	replayer.print_max_entries = max_entries;
	replayer.create_results_file = false;
	replayer.set_frames(start_frame, end_frame);
	replayer.init_metadata(filename_input);
	if (max_entries == 0) replayer.request_stop();

	random_access_file_reader source(packed_open("thread_" + std::to_string(thread) + ".bin", filename_input));
	source.load_packet_checkpoints(frame_info);
	if (verbose)
	{
		ILOG("Printing thread %u from %s using %zu packet checkpoints", (unsigned)thread,
			filename_input.c_str(), source.packet_checkpoints().size());
	}

	std::vector<char> packet;
	lava_file_reader* packet_reader = nullptr;
	uint64_t packet_position = 0;
	if (selectors.empty())
	{
		uint32_t packet_index = 0;
		source.seek(0);
		while (!replayer.stop_requested() && source.remaining() > 0)
		{
			uint32_t current_packet = packet_index;
			if (!load_packet(source, packet_index, current_packet, packet_position, packet)) break;
			print_packet(replayer, packet_reader, boundaries, thread, packet_index, packet_position, source.version(), packet);
			packet_index++;
		}
	}
	else
	{
		for (const print_packet_selector& selector : selectors)
		{
			if (replayer.stop_requested()) break;
			uint32_t current_packet = source.seek_to_packet(selector.packet);
			if (!load_packet(source, selector.packet, current_packet, packet_position, packet))
			{
				DLOG("Packet %u does not exist on thread %u", (unsigned)selector.packet, (unsigned)thread);
				continue;
			}
			print_packet(replayer, packet_reader, boundaries, thread, selector.packet, packet_position, source.version(), packet);
		}
	}

	delete packet_reader;
	close_debug_destination();
	return replayer.exit_status.load(std::memory_order_acquire);
}
