#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <vector>
#include <string>

#include "vulkan/vulkan.h"
#include "util.h"
#include "read_auto.h"
#include "read.h"
#include "packfile.h"
#include "util_auto.h"

static lava_reader replayer;

static void usage()
{
	printf("lava-validate %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-o/--debugfile FILE    Output debug output to the given file\n");
	printf("-f/--frames start end  Select a frame range\n");
	printf("-r/--remap-validate    Validate existing device address remappings - abort if we find less or more addresses than already marked\n");
	exit(-1);
}

static inline bool match(const char* in, const char* short_form, const char* long_form, int& remaining)
{
	if (strcmp(in, short_form) == 0 || strcmp(in, long_form) == 0)
	{
		remaining--;
		return true;
	}
	return false;
}

static int get_int(const char* in, int& remaining)
{
	if (remaining == 0)
	{
		usage();
	}
	remaining--;
	return atoi(in);
}

static std::string get_str(const char* in, int& remaining)
{
	if (remaining == 0)
	{
		usage();
	}
	remaining--;
	return in;
}

static void replay_thread(int thread_id)
{
	lava_file_reader& t = replayer.file_reader(thread_id);
	uint8_t instrtype;
	assert(t.run == false);
	for (const auto pair : replayer.device_address_remapping.iter())
	{
		ILOG("Device address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + pair.second->size));
	}
	for (const auto pair : replayer.acceleration_structure_address_remapping.iter())
	{
		ILOG("Acceleration structure address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + pair.second->size));
	}
	while ((instrtype = t.step()))
	{
		if (instrtype == PACKET_VULKAN_API_CALL)
		{
			t.read_apicall();
		}
		else if (instrtype == PACKET_THREAD_BARRIER)
		{
			t.read_barrier();
		}
		else if (instrtype == PACKET_IMAGE_UPDATE)
		{
			const uint32_t device_index = t.read_handle();
			const uint32_t image_index = t.read_handle();
			image_update(t, device_index, image_index);
		}
		else if (instrtype == PACKET_BUFFER_UPDATE)
		{
			const uint32_t device_index = t.read_handle();
			const uint32_t buffer_index = t.read_handle();
			buffer_update(t, device_index, buffer_index);
		}
		t.device = VK_NULL_HANDLE;
		t.physicalDevice = VK_NULL_HANDLE;
		t.self_test();
	}
}

static void run_multithreaded(int n)
{
	for (int i = 0; i < n; i++)
	{
		replayer.threads.emplace_back(replay_thread, i);
	}
	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i].join();
	}
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int heap_size = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename;
	bool validate_remap = false;
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
		else if (match(argv[i], "-o", "--debugfile", remaining))
		{
			if (remaining < 1) usage();
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else if (match(argv[i], "-f", "--frames", remaining))
		{
			if (remaining < 2) usage();
			start = get_int(argv[++i], remaining);
			end = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-r", "-r/--remap-validate", remaining))
		{
			validate_remap = true;
		}
		else if (strcmp(argv[i], "--") == 0) // eg in case you have a file named -f ...
		{
			remaining--;
			filename = get_str(argv[++i], remaining);
			if (remaining > 0) usage();
			break; // stop parsing cmd line options
		}
		else
		{
			filename = get_str(argv[i], remaining);
			if (remaining > 0)
			{
				printf("Options after filename is not valid!\n\n");
				usage();
			}
		}
	}

	if (filename.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}

	replayer.run = false; // do not actually run anything
	replayer.init(filename, heap_size);
	replayer.parameters(start, end, false);
	replayer.remap = validate_remap;

	// Read all thread files
	std::vector<std::string> threadfiles = packed_files(filename, "thread_");
	if (threadfiles.size() == 0) DIE("Failed to find any threads in %s!", filename.c_str());
	run_multithreaded(threadfiles.size());
	if (p__debug_destination) fclose(p__debug_destination);
	return 0;
}
