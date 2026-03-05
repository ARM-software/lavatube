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
#include "postprocess.h"

extern lava::mutex sync_mutex;

static bool validate = false;
static bool verbose = false;
static bool report_unused = false;
static bool dump_shaders = false;
static bool dump_host_write_stats = false;
static int invokation_count = 0;

// Utility funcs

static bool rewrite_call_less(const address_rewrite& a, const address_rewrite& b)
{
	return a.source.call < b.source.call;
}

static void usage()
{
	printf("lava-tool %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-tool [options] <input filename> [<output filename>]\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-V/--validate          Validate the input trace, abort with an error if anything amiss found instead of just reporting on it\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
#endif
	printf("-f/--frames start end  Select a frame range\n");
	printf("-u/--unused            Find any found unused features and extensions in the trace file; remove them from the output file\n");
	printf("-DS/--dump-shaders     Dump any shaders found to disk\n");
	printf("-hw/--host-write-stats Dump host-side write tracking stats after replay\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
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

static void dump_host_write_stats_report(const char* label)
{
	const host_write_totals buffers = gather_host_write_stats(VkBuffer_index);
	const host_write_totals images = gather_host_write_stats(VkImage_index);
	const host_write_totals tensors = gather_host_write_stats(VkTensorARM_index);
	const host_write_totals accel = gather_host_write_stats(VkAccelerationStructureKHR_index);

	host_write_totals total;
	total.objects = buffers.objects + images.objects + tensors.objects + accel.objects;
	total.objects_with_data = buffers.objects_with_data + images.objects_with_data + tensors.objects_with_data + accel.objects_with_data;
	total.segments = buffers.segments + images.segments + tensors.segments + accel.segments;
	total.bytes = buffers.bytes + images.bytes + tensors.bytes + accel.bytes;

	printf("Host write stats (%s):\n", label);
	printf("  Total: objects=%lu with_data=%lu segments=%lu bytes=%lu\n",
		(unsigned long)total.objects, (unsigned long)total.objects_with_data,
		(unsigned long)total.segments, (unsigned long)total.bytes);
	printf("  Buffers: objects=%lu with_data=%lu segments=%lu bytes=%lu max_segments=%lu highest index=%s\n",
		(unsigned long)buffers.objects, (unsigned long)buffers.objects_with_data,
		(unsigned long)buffers.segments, (unsigned long)buffers.bytes,
		(unsigned long)buffers.max_segments, buffers.max_index == CONTAINER_INVALID_INDEX ? "n/a" : std::to_string(buffers.max_index).c_str());
	printf("  Images: objects=%lu with_data=%lu segments=%lu bytes=%lu max_segments=%lu highest index=%s\n",
		(unsigned long)images.objects, (unsigned long)images.objects_with_data,
		(unsigned long)images.segments, (unsigned long)images.bytes,
		(unsigned long)images.max_segments, images.max_index == CONTAINER_INVALID_INDEX ? "n/a" : std::to_string(buffers.max_index).c_str());
	printf("  Tensors: objects=%lu with_data=%lu segments=%lu bytes=%lu max_segments=%lu highest index=%s\n",
		(unsigned long)tensors.objects, (unsigned long)tensors.objects_with_data,
		(unsigned long)tensors.segments, (unsigned long)tensors.bytes,
		(unsigned long)tensors.max_segments, tensors.max_index == CONTAINER_INVALID_INDEX ? "n/a" : std::to_string(buffers.max_index).c_str());
	printf("  AccelStructs: objects=%lu with_data=%lu segments=%lu bytes=%lu max_segments=%lu highest index=%s\n",
		(unsigned long)accel.objects, (unsigned long)accel.objects_with_data,
		(unsigned long)accel.segments, (unsigned long)accel.bytes,
		(unsigned long)accel.max_segments, accel.max_index == CONTAINER_INVALID_INDEX ? "n/a" : std::to_string(buffers.max_index).c_str());
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
			for (const auto* obj : pair.second)
			{
				ILOG("Device address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + obj->size));
			}
		}
		for (const auto& pair : replayer->acceleration_structure_address_remapping.iter())
		{
			for (const auto* obj : pair.second)
			{
				ILOG("Acceleration structure address range %lu -> %lu", (unsigned long)pair.first, (unsigned long)(pair.first + obj->size));
			}
		}
	}
	while ((instrtype = t.step()))
	{
		switchboard_packet(instrtype, t);
		t.self_test();
	}
	uint64_t worker_local = 0;
	uint64_t runner_local = 0;
	t.stop_measurement(worker_local, runner_local);
}

// We implement these here since we need info from rest of tool code

void postprocess_vkCreateShaderModule(callback_context& cb, VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule)
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

void postprocess_vkDestroyDevice(callback_context& cb, VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	uint32_t device_index = index_to_VkDevice.index(device);
	const auto& device_data = VkDevice_index.at(device_index);
	for (uint32_t i = 0; i < index_to_VkPipeline.size(); i++)
	{
		const auto& pipeline_data = VkPipeline_index.at(i);
		for (const auto& stage : pipeline_data.shader_stages)
		{
			if (stage.device_index == device_data.index) invokation_count += stage.calls;
		}
	}
	for (uint32_t i = 0; i < index_to_VkShaderEXT.size(); i++)
	{
		const auto& shader_data = VkShaderEXT_index.at(i);
		if (shader_data.stage.device_index == device_data.index) invokation_count += shader_data.stage.calls;
	}
}

// Main

static void add_callbacks_for_first_round()
{
#define CALLBACK(x) x ## _callbacks.push_back(postprocess_ ## x);
	CALLBACK(vkCreateShaderModule);
	CALLBACK(vkDestroyDevice);
	CALLBACK(vkSubmitDebugUtilsMessageEXT);
	CALLBACK(vkCmdPushConstants);
	CALLBACK(vkCmdPushConstants2KHR);
	CALLBACK(vkCmdPushConstants2);
	CALLBACK(vkCreateRayTracingPipelinesKHR);
	CALLBACK(vkGetRayTracingShaderGroupHandlesKHR);
	CALLBACK(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR);
	CALLBACK(vkCreateGraphicsPipelines);
	CALLBACK(vkCreateComputePipelines);
	CALLBACK(vkCmdBindPipeline);
	CALLBACK(vkCmdTraceRaysKHR);
	CALLBACK(vkCmdTraceRaysIndirectKHR);
	CALLBACK(vkCmdTraceRaysIndirect2KHR);
	CALLBACK(vkQueueSubmit);
	CALLBACK(vkQueueSubmit2);
	CALLBACK(vkQueueSubmit2KHR);
	CALLBACK(vkCmdBindDescriptorSets2KHR);
	CALLBACK(vkCmdBindDescriptorSets);
	CALLBACK(vkCmdBindDescriptorSets2);
	CALLBACK(vkCmdPushDescriptorSet2KHR);
	CALLBACK(vkCmdPushDescriptorSet2);
	CALLBACK(vkCmdPushDescriptorSetKHR);
	CALLBACK(vkCmdPushDescriptorSet);
	CALLBACK(vkUpdateDescriptorSets);
	CALLBACK(vkCmdUpdateBuffer);
	CALLBACK(vkCmdCopyBuffer);
	CALLBACK(vkCmdCopyBuffer2);
	CALLBACK(vkCreateShadersEXT);
	CALLBACK(vkCmdBindShadersEXT);
#undef CALLBACK
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename_input;
	std::string filename_output;

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
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-u", "--report-unused", remaining))
		{
			report_unused = true;
		}
		else if (match(argv[i], "-DS", "--dump-shaders", remaining))
		{
			dump_shaders = true;
		}
		else if (match(argv[i], "-hw", "--host-write-stats", remaining))
		{
			dump_host_write_stats = true;
		}
		else if (match(argv[i], "-df", "--debugfile", remaining))
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
		replayer.validate = validate; // abort on less serious errors, not just warn
		replayer.pass = 0; // first pass
		replayer.set_frames(start, end);
		replayer.init(filename_input);

		// Add callbacks
		add_callbacks_for_first_round();

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
		sync_mutex.lock(); // threads are stopped here but let's avoid warnings
		rewrite_queue_copy = replayer.global_rewrite_queue;
		sync_mutex.unlock();

		if (dump_host_write_stats) dump_host_write_stats_report("pass1");
		reset_for_tools();
		replayer.finalize(false);
	}

	if (!filename_output.empty() || validate) // run second round to write out or test all changes
	{
		lava_reader replayer;
		replayer.pass = 1; // second pass
		replayer.run = false; // do not actually run anything, again
		replayer.validate = validate; // abort on less serious errors, not just warn
		replayer.init(filename_input);
		replayer.set_frames(start, end);

		// We can probably skip most of the callbacks now. Trying to skip all for now.
		assert(vkCreateShaderModule_callbacks.size() == 0); // Verify that they were cleared

		// Add in the rewrite queue from the previous run, but split by thread and sorted by call number.
		sync_mutex.lock();
		replayer.global_rewrite_queue = rewrite_queue_copy;
		sync_mutex.unlock();
		for (const auto& v : rewrite_queue_copy)
		{
			assert(v.markings != nullptr && v.markings->count > 0);
			ILOG("%s - number of markings: %u", describe_change_source(v.source).c_str(), (unsigned)v.markings->count);
			lava_file_reader& t = replayer.file_reader(v.source.thread); // get the right filereader object
			t.rewrite_queue.push_back(v);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			lava_file_reader& t = replayer.file_reader(i);
			t.rewrite_queue.sort(rewrite_call_less);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++) // TBD sanity test, remove later
		{
			lava_file_reader& t = replayer.file_reader(i);
			assert(i == (unsigned)t.thread_index());
			unsigned last = 0;
			for (const auto& v : t.rewrite_queue)
			{
				assert((int)v.source.thread == (int)t.thread_index());
				assert(last <= v.source.call);
				last = v.source.call;
			}
		}

		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i].join();
		}

		if (dump_host_write_stats) dump_host_write_stats_report("pass2");
		reset_for_tools();
		replayer.finalize(false);
	}

	printf("%d shader invokations executed\n", invokation_count);

	if (p__debug_destination) fclose(p__debug_destination);
	return 0;
}
