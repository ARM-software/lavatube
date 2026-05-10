#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>
#include <vector>
#include <string>

#include "vulkan/vulkan.h"
#include "util.h"
#include "read_auto.h"
#include "read.h"
#include "write.h"
#include "write_auto.h"
#include "packfile.h"
#include "util_auto.h"
#include "sandbox.h"
#include "postprocess.h"
#include "json_helpers.h"
#include "replay_callbacks.h"
#include "replay_trace_adapter.h"
#include "markings.h"

extern lava::mutex sync_mutex;

static bool simulate = false;
static bool verbose = false;
static bool report_unused = false;
static bool dump_shaders = false;
static bool dump_host_write_stats = false;
static bool write_output = false;
static int invokation_count = 0;

static inline bool is_update_packet(uint8_t instrtype)
{
	return instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_TENSOR_UPDATE
		|| instrtype == PACKET_IMAGE_UPDATE2 || instrtype == PACKET_BUFFER_UPDATE2;
}

static void sync_output_buffer_memory_flags(VkBuffer buffer)
{
	auto* writer_buffer = lava_writer::instance().records.VkBuffer_index.at(buffer);
	assert(writer_buffer);
	writer_buffer->memory_flags = VkBuffer_index.at(fake_index<VkBuffer>(buffer)).memory_flags;
}

static void sync_output_image_memory_flags(VkImage image)
{
	auto* writer_image = lava_writer::instance().records.VkImage_index.at(image);
	assert(writer_image);
	const trackedimage& reader_image = VkImage_index.at(fake_index<VkImage>(image));
	writer_image->memory_flags = reader_image.memory_flags;
	writer_image->size = reader_image.size;
}

static void sync_output_tensor_memory_flags(VkTensorARM tensor)
{
	auto* writer_tensor = lava_writer::instance().records.VkTensorARM_index.at(tensor);
	assert(writer_tensor);
	writer_tensor->memory_flags = VkTensorARM_index.at(fake_index<VkTensorARM>(tensor)).memory_flags;
}

static void sync_output_datagraph_session_memory_flags(const VkBindDataGraphPipelineSessionMemoryInfoARM& bind_info)
{
	auto* writer_session = lava_writer::instance().records.VkDataGraphPipelineSessionARM_index.at(bind_info.session);
	assert(writer_session);
	const auto& reader_session = VkDataGraphPipelineSessionARM_index.at(fake_index<VkDataGraphPipelineSessionARM>(bind_info.session));
	const auto* reader_binding = reader_session.find_binding(bind_info.bindPoint, bind_info.objectIndex);
	if (!reader_binding) return;
	auto& writer_binding = writer_session->get_binding(bind_info.bindPoint, bind_info.objectIndex);
	writer_binding.memory_flags = reader_binding->memory_flags;
}

static void sync_output_queue_metadata(VkQueue queue)
{
	auto& instance = lava_writer::instance();
	if (instance.records.VkQueue_index.contains(queue)) return;

	const uint32_t original_index = index_to_VkQueue.index(queue);
	const trackedqueue& original_queue = VkQueue_index.at(original_index);
	auto* writer_queue = instance.records.VkQueue_index.add(queue, instance.file_writer().current, original_index);
	*writer_queue = original_queue;
	if (writer_queue->last_modified.frame == UINT32_MAX) writer_queue->last_modified = writer_queue->creation;
	writer_queue->realQueue = queue;
}

static void sync_output_vkBindBufferMemory(callback_context&, VkDevice, VkBuffer buffer, VkDeviceMemory, VkDeviceSize)
{
	sync_output_buffer_memory_flags(buffer);
}

static void sync_output_vkBindImageMemory(callback_context&, VkDevice, VkImage image, VkDeviceMemory, VkDeviceSize)
{
	sync_output_image_memory_flags(image);
}

static void sync_output_vkBindBufferMemory2(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_buffer_memory_flags(pBindInfos[i].buffer);
}

static void sync_output_vkBindImageMemory2(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_image_memory_flags(pBindInfos[i].image);
}

static void sync_output_vkBindTensorMemoryARM(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindTensorMemoryInfoARM* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_tensor_memory_flags(pBindInfos[i].tensor);
}

static void sync_output_vkBindDataGraphPipelineSessionMemoryARM(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindDataGraphPipelineSessionMemoryInfoARM* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_datagraph_session_memory_flags(pBindInfos[i]);
}

static void sync_output_vkGetDeviceQueue(callback_context&, VkDevice, uint32_t, uint32_t, VkQueue* pQueue)
{
	if (pQueue && *pQueue != VK_NULL_HANDLE) sync_output_queue_metadata(*pQueue);
}

static void sync_output_vkGetDeviceQueue2(callback_context&, VkDevice, const VkDeviceQueueInfo2*, VkQueue* pQueue)
{
	if (pQueue && *pQueue != VK_NULL_HANDLE) sync_output_queue_metadata(*pQueue);
}

static std::list<address_rewrite>::iterator find_output_rewrite_entry(lava_file_reader& reader)
{
	auto same_output_phase_source = [&](const address_rewrite& entry)
	{
		if (entry.source.thread != reader.current.thread || entry.source.frame != reader.current.frame || entry.source.call_id != reader.current.call_id) return false;
		return entry.source.call == reader.current.call || entry.source.call == reader.current.call + 1;
	};
	return std::find_if(reader.rewrite_queue.begin(), reader.rewrite_queue.end(), same_output_phase_source);
}

static VkBaseOutStructure* build_flush_markings_chain(const VkMappedMemoryRange& original, VkMarkedOffsetsARM& desired, VkFlushRangesFlagsARM& flags_copy)
{
	VkBaseOutStructure* pNext = (VkBaseOutStructure*)original.pNext;
	if (!pNext)
	{
		desired.pNext = nullptr;
		return (VkBaseOutStructure*)&desired;
	}
	if (pNext->sType == VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM)
	{
		desired.pNext = pNext->pNext;
		return (VkBaseOutStructure*)&desired;
	}
	if (pNext->sType == VK_STRUCTURE_TYPE_FLUSH_RANGES_FLAGS_ARM)
	{
		const VkFlushRangesFlagsARM* flags = (const VkFlushRangesFlagsARM*)pNext;
		flags_copy = *flags;
		VkBaseOutStructure* next = (VkBaseOutStructure*)flags->pNext;
		if (!next)
		{
			desired.pNext = nullptr;
			flags_copy.pNext = (VkBaseOutStructure*)&desired;
			return (VkBaseOutStructure*)&flags_copy;
		}
		if (next->sType == VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM)
		{
			desired.pNext = next->pNext;
			flags_copy.pNext = (VkBaseOutStructure*)&desired;
			return (VkBaseOutStructure*)&flags_copy;
		}
		desired.pNext = next;
		flags_copy.pNext = (VkBaseOutStructure*)&desired;
		return (VkBaseOutStructure*)&flags_copy;
	}
	ABORT("Unsupported VkMappedMemoryRange pNext chain while injecting simulated markings");
}

static void tool_write_vkFlushMappedMemoryRanges(callback_context& cb, VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
	auto it = find_output_rewrite_entry(cb.reader);
	if (it == cb.reader.rewrite_queue.end())
	{
		prepare_trace_callback(cb);
		(void)trace_vkFlushMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);
		return;
	}

	if (!pMemoryRanges || memoryRangeCount == 0) ABORT("Need a VkMappedMemoryRange to inject simulated markings for %s", describe_change_source(cb.reader.current).c_str());
	if (memoryRangeCount != 1) ABORT("Simulated markings injection for vkFlushMappedMemoryRanges currently only supports a single range");

	VkMarkedOffsetsARM* desired = clone_marked_offsets(it->markings);
	normalize_marked_offsets(desired);

	const VkMappedMemoryRange& original = pMemoryRanges[0];
	const VkMarkedOffsetsARM* existing = (const VkMarkedOffsetsARM*)find_extension(&original, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	VkMarkedOffsetsARM* existing_normalized = clone_marked_offsets(existing);
	normalize_marked_offsets(existing_normalized);
	const marked_offsets_difference diff = compare_marked_offsets(existing_normalized, desired);

	const VkMappedMemoryRange* ranges_to_write = pMemoryRanges;
	VkMappedMemoryRange patched_range = original;
	VkFlushRangesFlagsARM flags_copy{};
	if (diff != marked_offsets_difference::none)
	{
		if (existing)
		{
			ILOG("Replacing VkMarkedOffsetsARM for %s (%s)", describe_change_source(cb.reader.current).c_str(), marked_offsets_difference_string(diff));
		}
		else
		{
			ILOG("Injecting VkMarkedOffsetsARM for %s (%u markings)", describe_change_source(cb.reader.current).c_str(), (unsigned)desired->count);
		}
		patched_range.pNext = build_flush_markings_chain(original, *desired, flags_copy);
		ranges_to_write = &patched_range;
	}

	prepare_trace_callback(cb);
	(void)trace_vkFlushMappedMemoryRanges(device, memoryRangeCount, ranges_to_write);

	free_marked_offsets(existing_normalized);
	free_marked_offsets(desired);
	free_marked_offsets(it->markings);
	cb.reader.rewrite_queue.erase(it);
}

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
	printf("-S/--simulate          Run a simulation to find how memory is used (if no output file, we just validate and abort on any errors)\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
#endif
	printf("-f/--frames start end  Select a frame range\n");
	printf("-u/--unused            Find any found unused features and extensions in the trace file; remove them from the output file\n");
	printf("-DS/--dump-shaders     Dump any shaders found to disk\n");
	printf("-hw/--host-write-stats Dump host-side write tracking stats after replay\n");
	printf("--skip-missing-input   Exit with code 77 if the input trace file does not exist\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
	exit(-1);
}

static inline bool match(const char* in, const char* short_form, const char* long_form, int& remaining)
{
	if ((short_form && strcmp(in, short_form) == 0) || (long_form && strcmp(in, long_form) == 0))
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

static void print_removed_json_list(const char* heading, const Json::Value& values)
{
	if (!values.isArray() || values.empty()) return;
	printf("%s\n", heading);
	for (const Json::Value& value : values) printf("\t%s\n", value.asString().c_str());
}

static void print_removed_feature_lists(const Json::Value& removed_features)
{
	if (!removed_features.isObject()) return;
	std::vector<std::string> names = removed_features.getMemberNames();
	std::sort(names.begin(), names.end());
	for (const std::string& name : names)
	{
		const Json::Value& values = removed_features[name];
		if (!values.isArray() || values.empty()) continue;
		printf("Already removed device features from %s:\n", name.c_str());
		for (const Json::Value& value : values) printf("\t%s\n", value.asString().c_str());
	}
}

static void bootstrap_write_side_state(const std::string& input)
{
	lava_writer& writer = lava_writer::instance();

	frame_mutex.lock();
	writer.json() = packed_json("metadata.json", input);
	writer.json()["lavatube_version_major"] = LAVATUBE_VERSION_MAJOR;
	writer.json()["lavatube_version_minor"] = LAVATUBE_VERSION_MINOR;
	writer.json()["lavatube_version_patch"] = LAVATUBE_VERSION_PATCH;
	writer.json()["vulkan_header_version"] = version_to_string(VK_HEADER_VERSION);
	writer.global_frame.exchange(0);
	frame_mutex.unlock();

	const Json::Value tracking = packed_json("tracking.json", input);
	frame_mutex.lock();
	writer.input_tracking() = tracking;
	frame_mutex.unlock();
	if (!tracking.isMember("VkPhysicalDevice")) return;
	for (const Json::Value& value : tracking["VkPhysicalDevice"])
	{
		trackedphysicaldevice data = trackedphysicaldevice_json(value);
		data.last_modified = data.creation;
		if (data.queueFamilyProperties.empty())
		{
			VkQueueFamilyProperties props = {};
			props.queueCount = 1;
			props.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
			data.queueFamilyProperties.push_back(props);
		}
		auto* add = writer.records.VkPhysicalDevice_index.add(fake_handle<VkPhysicalDevice>(data.index), data.creation, data.index);
		*add = data;
	}
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
	try
	{
		lava_file_writer* output_writer = nullptr;
		if (write_output)
		{
			lava_writer::instance().bind_thread(t.thread_index());
			output_writer = &lava_writer::instance().file_writer();
		}
		while ((instrtype = t.step()))
		{
			uint64_t packet_start = 0;
			uint32_t output_call = 0;
			if (write_output)
			{
				packet_start = t.stream_position() - 1; // include the packet type already consumed by step()
				if (instrtype != PACKET_VULKAN_API_CALL && instrtype != PACKET_THREAD_BARRIER && !is_update_packet(instrtype))
				{
					ABORT("Output mode does not yet support packet type %u on thread %u call %u", (unsigned)instrtype, (unsigned)t.thread_index(), (unsigned)t.current.call);
				}
				if (instrtype == PACKET_VULKAN_API_CALL)
				{
					output_call = output_writer->current.call;
				}
			}
			switchboard_packet(instrtype, t);
			if (write_output && is_update_packet(instrtype))
			{
				if (output_writer->pending_barrier.load(std::memory_order_relaxed))
				{
					frame_mutex.lock();
					output_writer->inject_thread_barrier();
					output_writer->pending_barrier.store(false, std::memory_order_relaxed);
					frame_mutex.unlock();
				}
				const uint64_t packet_end = t.stream_position();
				output_writer->write_array(t.stream_data(packet_start), packet_end - packet_start);
			}
			if (write_output && instrtype == PACKET_VULKAN_API_CALL && output_writer->current.call != output_call + 1)
			{
				ABORT("Output mode does not yet support API call %s on thread %u call %u", get_function_name(t.current.call_id), (unsigned)t.thread_index(), (unsigned)t.current.call);
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

static void add_callbacks_for_first_round(bool enable_simulation, bool enable_submit_analysis)
{
#define CALLBACK(x) x ## _callbacks.push_back(postprocess_ ## x);
	if (dump_shaders) CALLBACK(vkCreateShaderModule);
	if (enable_simulation)
	{
		CALLBACK(vkDestroyDevice);
		CALLBACK(vkSubmitDebugUtilsMessageEXT);
		CALLBACK(vkCmdPushConstants);
		CALLBACK(vkCmdPushConstants2KHR);
		CALLBACK(vkCmdPushConstants2);
		CALLBACK(vkCreateRayTracingPipelinesKHR);
		CALLBACK(vkCreateDataGraphPipelineSessionARM);
		CALLBACK(vkGetDataGraphPipelineSessionMemoryRequirementsARM);
		CALLBACK(vkBindDataGraphPipelineSessionMemoryARM);
		CALLBACK(vkGetRayTracingShaderGroupHandlesKHR);
		CALLBACK(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR);
		CALLBACK(vkCreateGraphicsPipelines);
		CALLBACK(vkCreateComputePipelines);
		CALLBACK(vkCreateDataGraphPipelinesARM);
		CALLBACK(vkCmdBindPipeline);
		CALLBACK(vkCmdDispatchDataGraphARM);
		CALLBACK(vkCmdTraceRaysKHR);
		CALLBACK(vkCmdTraceRaysIndirectKHR);
		CALLBACK(vkCmdTraceRaysIndirect2KHR);
		if (enable_submit_analysis)
		{
			CALLBACK(vkQueueSubmit);
			CALLBACK(vkQueueSubmit2);
			CALLBACK(vkQueueSubmit2KHR);
		}
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
		CALLBACK(vkCmdCopyBuffer2KHR);
		CALLBACK(vkCreateShadersEXT);
		CALLBACK(vkCmdBindShadersEXT);

		// Tool validation runs with reader.run == false, so these callbacks consume stored
		// device addresses from the trace and populate the remappers we use for analysis.
		vkGetBufferDeviceAddress_callbacks.push_back(replay_callback_vkGetBufferDeviceAddress);
		vkGetBufferDeviceAddressKHR_callbacks.push_back(replay_callback_vkGetBufferDeviceAddressKHR);
		vkGetBufferDeviceAddressEXT_callbacks.push_back(replay_callback_vkGetBufferDeviceAddressEXT);
		vkGetAccelerationStructureDeviceAddressKHR_callbacks.push_back(replay_callback_vkGetAccelerationStructureDeviceAddressKHR);
		vkCreateBuffer_callbacks.push_back(replay_callback_vkCreateBuffer);
		vkCreateAccelerationStructureKHR_callbacks.push_back(replay_callback_vkCreateAccelerationStructureKHR);
	}
#undef CALLBACK
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename_input;
	std::string filename_output;
	bool skip_missing_input = false;

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
		else if (match(argv[i], "-S", "--simulate", remaining))
		{
			simulate = true;
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
		else if (match(argv[i], nullptr, "--skip-missing-input", remaining))
		{
			skip_missing_input = true;
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

	if (skip_missing_input && access(filename_input.c_str(), R_OK) != 0)
	{
		printf("SKIP: input trace file does not exist or is not readable: %s\n", filename_input.c_str());
		return 77;
	}
	if (!filename_output.empty() && (end != -1 || report_unused))
	{
		DIE("Output mode currently only supports no-op rewriting; frame selection and unused-feature removal are not supported");
	}

	if (p__sandbox_level >= 3) sandbox_level_two();

	if (report_unused || dump_host_write_stats) simulate = true;

	std::list<address_rewrite> rewrite_queue_copy;

	Json::Value meta = packed_json("metadata.json", filename_input);
	Json::Value instance_removed_json = meta["instanceRequested"]["removedExtensions"];
	Json::Value device_removed_json = meta["deviceRequested"]["removedExtensions"];
	Json::Value device_removed_features_json = meta["deviceRequested"]["removedFeatures"];
	if (verbose || report_unused)
	{
		print_removed_json_list("Already removed instance extensions:", instance_removed_json);
		print_removed_json_list("Already removed device extensions:", device_removed_json);
		print_removed_feature_lists(device_removed_features_json);
	}

	const bool need_first_round = filename_output.empty() || simulate || dump_shaders || dump_host_write_stats;

	if (need_first_round)
	{
		lava_reader replayer;
		replayer.run = false; // do not actually run anything
		replayer.validate = simulate; // abort on less serious errors, not just warn
		replayer.pass = 0; // first pass
		replayer.set_frames(start, end);
		replayer.init(filename_input);

		add_callbacks_for_first_round(simulate, simulate);

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
		replayer.finalize();
	}

	if (simulate && filename_output.empty())
	{
		lava_reader replayer;
		replayer.pass = 1; // second pass
		replayer.run = false; // do not actually run anything, again
		replayer.validate = true; // abort on less serious errors, not just warn
		replayer.init(filename_input);
		replayer.set_frames(start, end);

		assert(vkCreateShaderModule_callbacks.size() == 0); // Verify that they were cleared

		sync_mutex.lock();
		replayer.global_rewrite_queue = rewrite_queue_copy;
		sync_mutex.unlock();
		for (const auto& v : rewrite_queue_copy)
		{
			assert(v.markings != nullptr && v.markings->count > 0);
			ILOG("%s - number of markings: %u", describe_change_source(v.source).c_str(), (unsigned)v.markings->count);
			lava_file_reader& t = replayer.file_reader(v.source.thread);
			t.rewrite_queue.push_back(v);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			lava_file_reader& t = replayer.file_reader(i);
			t.rewrite_queue.sort(rewrite_call_less);
		}
#ifndef NDEBUG
		for (unsigned i = 0; i < replayer.threads.size(); i++)
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
#endif

		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i].join();
		}
		{
			lava::lock_guard lock(sync_mutex);
			if (!replayer.global_rewrite_queue.empty())
			{
				const address_rewrite& missing = replayer.global_rewrite_queue.front();
				DIE("Missing serialized memory markings for %s (%u discovered markings left unmatched)",
					describe_change_source(missing.source).c_str(), (unsigned)missing.markings->count);
			}
		}

		if (dump_host_write_stats) dump_host_write_stats_report("pass2");
		reset_for_tools();
		replayer.finalize();
	}
	else if (!filename_output.empty())
	{
		lava_reader replayer;
		replayer.pass = 1;
		replayer.run = false;
		replayer.write_output = true;
		replayer.validate = false;
		replayer.init(filename_input);
		replayer.set_frames(start, end);
		if (simulate)
		{
			for (const auto& v : rewrite_queue_copy)
			{
				replayer.file_reader(v.source.thread).rewrite_queue.push_back(v);
			}
			for (unsigned i = 0; i < replayer.threads.size(); i++)
			{
				replayer.file_reader(i).rewrite_queue.sort(rewrite_call_less);
			}
		}

		lava_writer& writer = lava_writer::instance();
		writer.run = false;
		writer.set_output(filename_output);
		writer.prepare_threads(replayer.threads.size());
		bootstrap_write_side_state(filename_input);
		add_callbacks_for_output();
		vkBindBufferMemory_callbacks.clear();
		vkBindBufferMemory_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindBufferMemory, sync_output_vkBindBufferMemory>::call);
		vkBindImageMemory_callbacks.clear();
		vkBindImageMemory_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindImageMemory, sync_output_vkBindImageMemory>::call);
		vkGetDeviceQueue_callbacks.clear();
		vkGetDeviceQueue_callbacks.push_back(replay_trace_callback_with_pre<trace_vkGetDeviceQueue, sync_output_vkGetDeviceQueue>::call);
		vkGetDeviceQueue2_callbacks.clear();
		vkGetDeviceQueue2_callbacks.push_back(replay_trace_callback_with_pre<trace_vkGetDeviceQueue2, sync_output_vkGetDeviceQueue2>::call);
		vkFlushMappedMemoryRanges_callbacks.clear();
		vkFlushMappedMemoryRanges_callbacks.push_back(tool_write_vkFlushMappedMemoryRanges);
		vkBindBufferMemory2_callbacks.clear();
		vkBindBufferMemory2_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindBufferMemory2, sync_output_vkBindBufferMemory2>::call);
		vkBindBufferMemory2KHR_callbacks.clear();
		vkBindBufferMemory2KHR_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindBufferMemory2KHR, sync_output_vkBindBufferMemory2>::call);
		vkBindImageMemory2_callbacks.clear();
		vkBindImageMemory2_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindImageMemory2, sync_output_vkBindImageMemory2>::call);
		vkBindImageMemory2KHR_callbacks.clear();
		vkBindImageMemory2KHR_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindImageMemory2KHR, sync_output_vkBindImageMemory2>::call);
		vkBindTensorMemoryARM_callbacks.clear();
		vkBindTensorMemoryARM_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindTensorMemoryARM, sync_output_vkBindTensorMemoryARM>::call);
		vkBindDataGraphPipelineSessionMemoryARM_callbacks.clear();
		vkBindDataGraphPipelineSessionMemoryARM_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindDataGraphPipelineSessionMemoryARM, sync_output_vkBindDataGraphPipelineSessionMemoryARM>::call);
		write_output = true;

		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i] = std::thread(&replay_thread, &replayer, i);
		}
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			replayer.threads[i].join();
		}
		if (simulate)
		{
			for (unsigned i = 0; i < replayer.threads.size(); i++)
			{
				lava_file_reader& t = replayer.file_reader(i);
				if (!t.rewrite_queue.empty())
				{
					const address_rewrite& missing = t.rewrite_queue.front();
					DIE("Missing output injection of simulated memory markings for %s (%u discovered markings left unmatched)",
						describe_change_source(missing.source).c_str(), (unsigned)missing.markings->count);
				}
			}
		}

		write_output = false;
		writer.serialize();
		writer.finish();
		writer.run = true;
		clear_callbacks();
		reset_for_tools();
		replayer.finalize();
	}

	printf("%d shader invokations executed\n", invokation_count);

	close_debug_destination();
	return 0;
}
