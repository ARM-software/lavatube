#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <string>
#include <vector>

#include "vulkan/vulkan.h"
#include "util.h"
#include "read_auto.h"
#include "read.h"
#include "sandbox.h"
#include "util_auto.h"

static bool verbose = false;

void usage()
{
	printf("lava-print %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-print [options] <input filename>\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
	printf("-f/--frames start end  Select a frame range\n");
	printf("-t/--thread NUM        Only print for this thread (can explicitly override with --select)\n");
	printf("--select LIST          Print selected packets: INDEX or INDEX:THREAD, comma-separated. Set default thread with --thread\n");
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

static bool selector_exists(const std::vector<print_packet_selector>& selectors, uint32_t packet, uint32_t thread)
{
	for (const print_packet_selector& selector : selectors)
	{
		if (selector.packet == packet && selector.thread == thread) return true;
	}
	return false;
}

static void normalize_selectors(std::vector<print_packet_selector>& selectors, uint32_t default_thread)
{
	std::vector<print_packet_selector> deduplicated;
	for (print_packet_selector& selector : selectors)
	{
		if (selector.thread == UINT32_MAX) selector.thread = default_thread;
		if (!selector_exists(deduplicated, selector.packet, selector.thread))
		{
			deduplicated.push_back(selector);
		}
	}
	selectors.swap(deduplicated);
}

static void replay_thread(lava_reader* replayer, int thread_id)
{
	if (p__sandbox_level >= 2) sandbox_level_three();
	lava_file_reader& t = replayer->file_reader(thread_id);
	t.bind_runner_thread();
	t.bind_trace_thread_name();
	t.start_measurement();
	uint8_t instrtype;
	assert(t.run == false);
	try
	{
		while ((instrtype = t.step()))
		{
			switchboard_packet(instrtype, t);
			if (instrtype != PACKET_VULKAN_API_CALL && replayer->print_packets && !t.printed_current_packet)
			{
				callback_context cb_context{ t };
				print_params_packet(cb_context);
			}
			t.self_test();
		}
	}
	catch (const replay_stop_requested&)
	{
	}
	t.terminated.store(true, std::memory_order_release);
	uint64_t worker_local = 0;
	uint64_t runner_local = 0;
	t.stop_measurement(worker_local, runner_local);
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	uint32_t print_thread_index = UINT32_MAX;
	std::vector<print_packet_selector> print_selectors;
	uint32_t print_max_entries = UINT32_MAX;
	int remaining = argc - 1; // zeroth is name of program
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
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout && p__debug_destination != stderr) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else if (match(argv[i], "-f", "--frames", remaining))
		{
			if (remaining < 2) usage();
			start = get_int(argv[++i], remaining);
			end = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-t", "--thread", remaining))
		{
			int thread = get_int(argv[++i], remaining);
			if (thread < 0) DIE("Invalid thread index %d", thread);
			print_thread_index = thread;
		}
		else if (match(argv[i], nullptr, "--select", remaining))
		{
			if (have_select) DIE("--select can only be specified once");
			if (remaining < 1) usage();
			const std::string selectors = get_str(argv[++i], remaining);
			parse_selectors(selectors.c_str(), print_selectors);
			have_select = true;
		}
		else if (match(argv[i], "-m", "--max", remaining))
		{
			int max_entries = get_int(argv[++i], remaining);
			if (max_entries < 0) DIE("Invalid max entries %d", max_entries);
			print_max_entries = max_entries;
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
		else if (strcmp(argv[i], "--") == 0) // eg in case you have a file named -f ...
		{
			remaining--;
			filename_input = get_str(argv[++i], remaining);
			if (remaining > 0) usage();
			break; // stop parsing cmd line options
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

	normalize_selectors(print_selectors, print_thread_index == UINT32_MAX ? 0 : print_thread_index);

	if (print_thread_index != UINT32_MAX || !print_selectors.empty())
	{
		Json::Value meta = packed_json("metadata.json", filename_input);
		const uint32_t trace_threads = meta["threads"].asUInt();
		if (print_thread_index != UINT32_MAX && print_thread_index >= trace_threads)
		{
			printf("Invalid thread index %u for trace with %u threads\n", (unsigned)print_thread_index, (unsigned)trace_threads);
			close_debug_destination();
			return -1;
		}
		for (const print_packet_selector& selector : print_selectors)
		{
			if (selector.thread >= trace_threads)
			{
				printf("Invalid selector thread index %u for trace with %u threads\n", (unsigned)selector.thread, (unsigned)trace_threads);
				close_debug_destination();
				return -1;
			}
		}
	}

	if (p__sandbox_level >= 3) sandbox_level_two();

	lava_reader replayer;
	replayer.run = false;
	replayer.print_packets = true;
	replayer.print_thread_index = print_thread_index;
	replayer.print_selectors = print_selectors;
	replayer.print_max_entries = print_max_entries;
	replayer.create_results_file = false;
	replayer.set_frames(start, end);
	replayer.init(filename_input);
	if (print_max_entries == 0)
	{
		replayer.request_stop();
	}

	if (verbose)
	{
		ILOG("Printing %u threads from %s", (unsigned)replayer.threads.size(), filename_input.c_str());
	}

	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
	}
	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i].join();
	}

	replayer.finalize();
	close_debug_destination();
	return replayer.exit_status.load(std::memory_order_acquire);
}
