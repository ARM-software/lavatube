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
#include "sandbox.h"

static bool validate = false;
static bool verbose = false;
static bool report_unused = false;
static bool dump_shaders = false;
static int invokation_count = 0;

static void usage()
{
	printf("lava-tool %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-tool [options] <input filename> [<output filename>]\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-V/--validate          Validate the input trace, abort with an error if anything amiss found instead of just reporting on it\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-o/--debugfile FILE    Output debug output to the given file\n");
#endif
	printf("-f/--frames start end  Select a frame range\n");
	printf("-r/--remap-validate    Validate existing device address remappings - abort if we find less or more addresses than already marked\n");
	printf("-u/--unused            Find any found unused features and extensions in the trace file; remove them from the output file\n");
	printf("-DS/--dump-shaders     Dump any shaders found to disk\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
	//printf("-R/--remap-addresses   Adding remapping of device addresses. Replaces existing address remappings. Requires an output file.\n");
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

static void replay_thread(lava_reader* replayer, int thread_id)
{
	if (p__sandbox_level >= 2) sandbox_level_three();
	lava_file_reader& t = replayer->file_reader(thread_id);
	uint8_t instrtype;
	assert(t.run == false);
	if (verbose)
	{
		for (const auto& pair : replayer->device_address_remapping.iter())
		{
			ILOG("Device address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + pair.second->size));
		}
		for (const auto& pair : replayer->acceleration_structure_address_remapping.iter())
		{
			ILOG("Acceleration structure address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + pair.second->size));
		}
	}
	while ((instrtype = t.step()))
	{
		switchboard_packet(instrtype, t);
		t.self_test();
	}
}

static void callback_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule)
{
	static int count = 0;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);

	if (dump_shaders)
	{
		std::string filename = "shader_" + std::to_string(count) + ".spv";
		FILE* fp = fopen(filename.c_str(), "w");
		if (!fp) printf("Failed to open %s: %s\n", filename.c_str(), strerror(errno));
		assert(fp);
		int r = fwrite(pCreateInfo->pCode, pCreateInfo->codeSize, 1, fp);
		assert(r == 1);
		(void)r;
		fclose(fp);
	}

	count++;
}

static void callback_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	uint32_t device_index = index_to_VkDevice.index(device);
	const auto& device_data = VkDevice_index.at(device_index);
	uint32_t num_modules = index_to_VkShaderModule.size();
	for (uint32_t i = 0; i < num_modules; i++)
	{
		const auto& shadermodule_data = VkShaderModule_index.at(i);
		if (shadermodule_data.device_index == device_data.index) invokation_count += shadermodule_data.calls;
	}
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int heap_size = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename_input;
	std::string filename_output;
	bool validate_remap = false;

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
		else if (match(argv[i], "-V", "--validate", remaining))
		{
			validate = true;
			(void)validate; // does not do anything yet...
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-u", "--report-unused", remaining))
		{
			report_unused = true;
			(void)report_unused; // TBD
		}
		else if (match(argv[i], "-DS", "--dump-shaders", remaining))
		{
			dump_shaders = true;
			(void)dump_shaders; // TBD
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
		else if (match(argv[i], "-s", "--sandbox", remaining))
		{
			p__sandbox_level = get_int(argv[++i], remaining);
			if (p__sandbox_level <= 0 || p__sandbox_level > 3) DIE("Invalid sandbox level %d", (int)p__sandbox_level);
		}
		else if (match(argv[i], "-r", "--remap-validate", remaining))
		{
			validate_remap = true;
		}
		else if (strcmp(argv[i], "--") == 0) // eg in case you have a file named -f ...
		{
			remaining--;
			filename_input = get_str(argv[++i], remaining);
			if (remaining) filename_output = get_str(argv[++i], remaining);
			if (remaining > 0) usage();
			break; // stop parsing cmd line options
		}
		else
		{
			filename_input = get_str(argv[i], remaining);
			if (remaining) filename_output = get_str(argv[++i], remaining);
			if (remaining > 0)
			{
				printf("Options after filename is not valid!\n\n");
				usage();
			}
		}
	}

	if (filename_input.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}

	if (!filename_output.empty()) DIE("Output file support still to be done!");

	if (p__sandbox_level >= 3) sandbox_level_two();

	std::list<address_rewrite> rewrite_queue_copy;

	Json::Value meta = packed_json("metadata.json", filename_input);
	Json::Value instance_removed_json = meta["instanceRequested"]["removedExtensions"];
	Json::Value device_removed_json = meta["deviceRequested"]["removedExtensions"];
	if ((verbose || report_unused) && instance_removed_json.size())
	{
		printf("Already removed instance extensions:\n");
		for (auto v : instance_removed_json) printf("\t%s\n", v.asString().c_str());
	}
	if ((verbose || report_unused) && device_removed_json.size())
	{
		printf("Already removed device extensions:\n");
		for (auto v : device_removed_json) printf("\t%s\n", v.asString().c_str());
	}

	// run first round
	{
		lava_reader replayer;
		replayer.run = false; // do not actually run anything
		replayer.set_frames(start, end);
		replayer.init(filename_input, heap_size);
		replayer.remap_scan = validate_remap;

		// Add callbacks
		vkCreateShaderModule_callbacks.push_back(callback_vkCreateShaderModule);
		vkDestroyDevice_callbacks.push_back(callback_vkDestroyDevice);

		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			if (verbose)
			{
				printf("Threads:\n");
				Json::Value frameinfo = packed_json("frames_" + _to_string(i) + ".json", filename_input);
				printf("\t%u : [%s] with %u local frames, %d highest global frame, %u uncompressed size\n", i, frameinfo.get("thread_name", "unknown").asString().c_str(),
					(unsigned)frameinfo["frames"].size(), frameinfo["highest_global_frame"].asInt(), frameinfo["uncompressed_size"].asUInt());
			}
			replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i].join();
		}

		// Copy out the rewrite queue
		if (validate_remap) rewrite_queue_copy = replayer.rewrite_queue;

		reset_for_tools();
		replayer.finalize(false);
	}

	if (validate_remap) // run second round
	{
		lava_reader replayer;
		replayer.run = false; // do not actually run anything
		replayer.init(filename_input, heap_size);
		replayer.set_frames(start, end);
		replayer.remap_scan = false;

		// Add in the rewrite queue from the previous run
		replayer.rewrite_queue = rewrite_queue_copy;

		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i].join();
		}

		reset_for_tools();
		replayer.finalize(false);
	}

	printf("%d shader invokations executed\n", invokation_count);

	if (p__debug_destination) fclose(p__debug_destination);
	return 0;
}
