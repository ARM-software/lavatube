#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>
#include <deque>
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
#include "suballocator.h"

extern lava::mutex sync_mutex;

static bool simulate = false;
static bool verbose = false;
static bool report_unused = false;
static bool dump_shaders = false;
static bool dump_host_write_stats = false;
static bool write_output = false;
static int invokation_count = 0;

struct descriptor_output_marking_key
{
	uint32_t buffer_index = CONTAINER_NULL_VALUE;
	VkDeviceSize offset = 0;
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
};

struct pending_descriptor_payload
{
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	std::vector<uint8_t> bytes;
};

struct output_descriptor_buffer_binding
{
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	VkBufferUsageFlags usage = 0;
};

struct pending_descriptor_buffer_update
{
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkDeviceSize offset = 0;
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	std::vector<uint8_t> bytes;
};

static std::vector<descriptor_output_marking_key> existing_descriptor_output_markings;
static std::vector<descriptor_buffer_payload> descriptor_buffer_payloads_for_output;
static thread_local std::deque<pending_descriptor_payload> pending_descriptor_payloads;
static thread_local std::vector<pending_descriptor_payload> used_descriptor_payloads;
static thread_local std::vector<output_descriptor_buffer_binding> output_descriptor_buffers;
static thread_local std::vector<pending_descriptor_buffer_update> pending_descriptor_buffer_updates;
static thread_local std::vector<pending_descriptor_buffer_update> emitted_descriptor_buffer_updates;

static inline bool is_update_packet(uint8_t instrtype)
{
	return instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_TENSOR_UPDATE
		|| instrtype == PACKET_IMAGE_UPDATE2 || instrtype == PACKET_BUFFER_UPDATE2;
}

static VkObjectType update_packet_object_type(uint8_t instrtype)
{
	switch (instrtype)
	{
	case PACKET_BUFFER_UPDATE:
	case PACKET_BUFFER_UPDATE2:
		return VK_OBJECT_TYPE_BUFFER;
	case PACKET_IMAGE_UPDATE:
	case PACKET_IMAGE_UPDATE2:
		return VK_OBJECT_TYPE_IMAGE;
	case PACKET_TENSOR_UPDATE:
		return VK_OBJECT_TYPE_TENSOR_ARM;
	default:
		return VK_OBJECT_TYPE_UNKNOWN;
	}
}

static void sync_output_buffer_memory_metadata(VkBuffer buffer)
{
	auto* writer_buffer = lava_writer::instance().records.VkBuffer_index.at(buffer);
	assert(writer_buffer);
	const trackedbuffer& reader_buffer = VkBuffer_index.at(fake_index<VkBuffer>(buffer));
	writer_buffer->memory_flags = reader_buffer.memory_flags;
	writer_buffer->backing_index = reader_buffer.backing_index;
	writer_buffer->offset = reader_buffer.offset;
	writer_buffer->req = reader_buffer.req;
}

static void sync_output_image_memory_metadata(VkImage image)
{
	auto* writer_image = lava_writer::instance().records.VkImage_index.at(image);
	assert(writer_image);
	const trackedimage& reader_image = VkImage_index.at(fake_index<VkImage>(image));
	writer_image->memory_flags = reader_image.memory_flags;
	writer_image->size = reader_image.size;
	writer_image->backing_index = reader_image.backing_index;
	writer_image->offset = reader_image.offset;
	writer_image->req = reader_image.req;
}

static void sync_output_tensor_memory_metadata(VkTensorARM tensor)
{
	auto* writer_tensor = lava_writer::instance().records.VkTensorARM_index.at(tensor);
	assert(writer_tensor);
	const trackedtensor& reader_tensor = VkTensorARM_index.at(fake_index<VkTensorARM>(tensor));
	writer_tensor->memory_flags = reader_tensor.memory_flags;
	writer_tensor->backing_index = reader_tensor.backing_index;
	writer_tensor->offset = reader_tensor.offset;
	writer_tensor->req = reader_tensor.req;
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
	sync_output_buffer_memory_metadata(buffer);
}

static void sync_output_vkBindImageMemory(callback_context&, VkDevice, VkImage image, VkDeviceMemory, VkDeviceSize)
{
	sync_output_image_memory_metadata(image);
}

static void sync_output_vkBindBufferMemory2(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_buffer_memory_metadata(pBindInfos[i].buffer);
}

static void sync_output_vkBindImageMemory2(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_image_memory_metadata(pBindInfos[i].image);
}

static void sync_output_vkBindTensorMemoryARM(callback_context&, VkDevice, uint32_t bindInfoCount, const VkBindTensorMemoryInfoARM* pBindInfos)
{
	if (!pBindInfos) return;
	for (uint32_t i = 0; i < bindInfoCount; i++) sync_output_tensor_memory_metadata(pBindInfos[i].tensor);
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
	const output_update_packet& update = reader.current_update_packet;
	const VkObjectType object_type = update_packet_object_type(update.instrtype);
	return std::find_if(reader.rewrite_queue.begin(), reader.rewrite_queue.end(), [&](const address_rewrite& entry)
	{
		if (!same_change_source(entry.source, reader.current)) return false;
		if (entry.object_type == VK_OBJECT_TYPE_UNKNOWN) return true;
		return entry.object_type == object_type && entry.object_index == update.object_index;
	});
}

static std::list<address_rewrite>::iterator find_api_rewrite_entry(lava_file_reader& reader)
{
	return std::find_if(reader.rewrite_queue.begin(), reader.rewrite_queue.end(), [&](const address_rewrite& entry)
	{
		if (!same_change_source(entry.source, reader.current)) return false;
		return entry.object_type == VK_OBJECT_TYPE_UNKNOWN;
	});
}

static std::list<address_rewrite>::iterator find_stage_rewrite_entry(lava_file_reader& reader, VkObjectType object_type, uint32_t object_index, uint32_t stage_index)
{
	return std::find_if(reader.rewrite_queue.begin(), reader.rewrite_queue.end(), [&](const address_rewrite& entry)
	{
		return same_change_source(entry.source, reader.current)
			&& entry.object_type == object_type
			&& entry.object_index == object_index
			&& entry.stage_index == stage_index;
	});
}

static void write_output_update_packet_prefix(lava_file_reader& reader, lava_file_writer& writer, uint64_t packet_start, uint64_t header_start)
{
	const uint64_t packet_payload_start = packet_start + sizeof(uint8_t) + sizeof(uint32_t);
	assert(header_start >= packet_payload_start);
	writer.write_array(reader.stream_data(packet_payload_start), header_start - packet_payload_start);
}

static void write_output_update_packet(lava_file_reader& reader, lava_file_writer& writer, uint64_t packet_start, uint64_t packet_end)
{
	assert(packet_end >= packet_start);
	const uint64_t packet_size = packet_end - packet_start;
	if (packet_size > UINT32_MAX) ABORT("Packet too large to copy: %lu bytes", (unsigned long)packet_size);
	writer.write_raw_packet(reader.stream_data(packet_start), (uint32_t)packet_size);
}

static bool is_flush_markings_source(const change_source& source)
{
	return source.call_id != UINT16_MAX && strcmp(get_function_name(source.call_id), "vkFlushMappedMemoryRanges") == 0;
}

static void write_marked_offsets_extension(lava_file_writer& writer, const VkMarkedOffsetsARM* sptr)
{
	assert(sptr);
	assert(sptr->sType == VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	assert(sptr->pNext == nullptr);
	writer.write_uint32_t((uint32_t)sptr->sType); // write_extension discriminant
	writer.write_uint32_t(sptr->sType);
	writer.write_uint32_t(0); // terminate pNext chain
	writer.write_uint32_t(sptr->count);
	const bool pMarkingTypes_opt = (sptr->pMarkingTypes != nullptr && sptr->count > 0);
	writer.write_uint8_t(pMarkingTypes_opt);
	if (pMarkingTypes_opt) writer.write_array(reinterpret_cast<const char*>(sptr->pMarkingTypes), sptr->count * sizeof(VkMarkingTypeARM));
	const bool pSubTypes_opt = (sptr->pSubTypes != nullptr && sptr->count > 0);
	writer.write_uint8_t(pSubTypes_opt);
	if (pSubTypes_opt) writer.write_array(reinterpret_cast<const char*>(sptr->pSubTypes), sptr->count * sizeof(VkMarkingSubTypeARM));
	const bool pOffsets_opt = (sptr->pOffsets != nullptr && sptr->count > 0);
	writer.write_uint8_t(pOffsets_opt);
	if (pOffsets_opt) writer.write_array(reinterpret_cast<const char*>(sptr->pOffsets), sptr->count * sizeof(VkDeviceSize));
}

static uint64_t marked_offsets_extension_size(const VkMarkedOffsetsARM* sptr)
{
	assert(sptr);
	uint64_t bytes = 0;
	bytes += sizeof(uint32_t); // write_extension discriminant
	bytes += sizeof(uint32_t); // sType
	bytes += sizeof(uint32_t); // pNext terminator
	bytes += sizeof(uint32_t); // count
	bytes += sizeof(uint8_t); // pMarkingTypes_opt
	if (sptr->pMarkingTypes && sptr->count > 0) bytes += (uint64_t)sptr->count * sizeof(VkMarkingTypeARM);
	bytes += sizeof(uint8_t); // pSubTypes_opt
	if (sptr->pSubTypes && sptr->count > 0) bytes += (uint64_t)sptr->count * sizeof(VkMarkingSubTypeARM);
	bytes += sizeof(uint8_t); // pOffsets_opt
	if (sptr->pOffsets && sptr->count > 0) bytes += (uint64_t)sptr->count * sizeof(VkDeviceSize);
	return bytes;
}

static bool maybe_write_rewritten_update_packet(lava_file_reader& reader, lava_file_writer& writer, uint64_t packet_start, uint64_t packet_end)
{
	auto it = find_output_rewrite_entry(reader);
	if (it == reader.rewrite_queue.end()) return false;

	const output_update_packet& update = reader.current_update_packet;
	if (!update.valid) ABORT("Need version2 update packet metadata to inject simulated markings for %s", describe_change_source(reader.current).c_str());
	if (update.instrtype != PACKET_BUFFER_UPDATE2 && update.instrtype != PACKET_IMAGE_UPDATE2 && update.instrtype != PACKET_TENSOR_UPDATE)
	{
		ABORT("Simulated markings injection currently only supports version2 update packets, got packet type %u for %s",
			(unsigned)update.instrtype, describe_change_source(reader.current).c_str());
	}
	if (update.sptr && update.sptr->sType != VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM)
	{
		ABORT("Unsupported update packet pNext chain while injecting simulated markings for %s", describe_change_source(reader.current).c_str());
	}

	VkMarkedOffsetsARM* desired = clone_marked_offsets(it->markings);
	normalize_marked_offsets(desired);
	VkMarkedOffsetsARM* existing = clone_marked_offsets((const VkMarkedOffsetsARM*)update.sptr);
	normalize_marked_offsets(existing);
	const marked_offsets_difference diff = compare_marked_offsets(existing, desired);
	if (diff == marked_offsets_difference::none)
	{
		free_marked_offsets(existing);
		free_marked_offsets(desired);
		free_marked_offsets(it->markings);
		reader.rewrite_queue.erase(it);
		return false;
	}

	if (existing)
	{
		ILOG("Replacing VkMarkedOffsetsARM on %s (%s)", describe_change_source(reader.current).c_str(), marked_offsets_difference_string(diff));
	}
	else
	{
		ILOG("Injecting VkMarkedOffsetsARM on %s (%u markings)", describe_change_source(reader.current).c_str(), (unsigned)desired->count);
	}

	writer.begin_packet(update.instrtype);
	write_output_update_packet_prefix(reader, writer, packet_start, update.header_start);
	const uint64_t payload_bytes = packet_end - update.payload_start;
	const uint64_t new_header_bytes = sizeof(uint16_t) + marked_offsets_extension_size(desired);
	writer.write_uint64_t(payload_bytes + new_header_bytes);
	writer.write_uint16_t(PACKET_FLAG_HAS_PNEXT);
	write_marked_offsets_extension(writer, desired);
	writer.write_array(reader.stream_data(update.payload_start), packet_end - update.payload_start);
	writer.end_packet();

	free_marked_offsets(existing);
	free_marked_offsets(desired);
	free_marked_offsets(it->markings);
	reader.rewrite_queue.erase(it);
	return true;
}

static const VkMarkedOffsetsARM* maybe_patch_push_constants_info(lava_file_reader& reader, VkPushConstantsInfo& patched)
{
	auto it = find_api_rewrite_entry(reader);
	if (it == reader.rewrite_queue.end()) return nullptr;

	if (patched.pNext)
	{
		const VkMarkedOffsetsARM* existing = (const VkMarkedOffsetsARM*)find_extension(&patched, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
		if (!existing)
		{
			ABORT("Unsupported vkCmdPushConstants2 pNext chain while injecting simulated markings for %s",
				describe_change_source(reader.current).c_str());
		}
	}

	VkMarkedOffsetsARM* desired = clone_marked_offsets(it->markings);
	normalize_marked_offsets(desired);
	const VkMarkedOffsetsARM* existing = (const VkMarkedOffsetsARM*)find_extension(&patched, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	VkMarkedOffsetsARM* existing_clone = clone_marked_offsets(existing);
	normalize_marked_offsets(existing_clone);
	const marked_offsets_difference diff = compare_marked_offsets(existing_clone, desired);
	free_marked_offsets(existing_clone);
	if (diff == marked_offsets_difference::none)
	{
		free_marked_offsets(desired);
		free_marked_offsets(it->markings);
		reader.rewrite_queue.erase(it);
		return nullptr;
	}

	ILOG("%s VkMarkedOffsetsARM on %s (%u markings)",
		existing ? "Replacing" : "Injecting",
		describe_change_source(reader.current).c_str(),
		(unsigned)desired->count);
	purge_extension_parent(&patched, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	desired->pNext = patched.pNext;
	patched.pNext = desired;
	free_marked_offsets(it->markings);
	reader.rewrite_queue.erase(it);
	return desired;
}

static const VkMarkedOffsetsARM* patch_pipeline_shader_stage_info(lava_file_reader& reader, std::list<address_rewrite>::iterator it,
	VkPipelineShaderStageCreateInfo& patched, const char* command_name)
{
	if (it == reader.rewrite_queue.end()) return nullptr;
	if (!patched.pSpecializationInfo || !patched.pSpecializationInfo->pData)
	{
		ABORT("Cannot inject simulated specialization-constant markings on %s for %s without specialization data",
			command_name, describe_change_source(reader.current).c_str());
	}

	VkMarkedOffsetsARM* desired = clone_marked_offsets(it->markings);
	normalize_marked_offsets(desired);
	const VkMarkedOffsetsARM* existing = (const VkMarkedOffsetsARM*)find_extension(&patched, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	VkMarkedOffsetsARM* existing_clone = clone_marked_offsets(existing);
	normalize_marked_offsets(existing_clone);
	const marked_offsets_difference diff = compare_marked_offsets(existing_clone, desired);
	free_marked_offsets(existing_clone);
	if (diff == marked_offsets_difference::none)
	{
		free_marked_offsets(desired);
		free_marked_offsets(it->markings);
		reader.rewrite_queue.erase(it);
		return nullptr;
	}

	ILOG("%s VkMarkedOffsetsARM on %s shader stage for %s (%u markings)",
		existing ? "Replacing" : "Injecting",
		command_name,
		describe_change_source(reader.current).c_str(),
		(unsigned)desired->count);
	purge_extension_parent(&patched, VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);
	desired->pNext = patched.pNext;
	patched.pNext = desired;
	free_marked_offsets(it->markings);
	reader.rewrite_queue.erase(it);
	return desired;
}

static bool marked_offsets_fit_specialization_data(const VkMarkedOffsetsARM* markings, const VkPipelineShaderStageCreateInfo& stage)
{
	if (!markings || !stage.pSpecializationInfo || !stage.pSpecializationInfo->pData) return false;
	if (markings->count == 0) return true;
	if (!markings->pOffsets || !markings->pMarkingTypes) return false;
	for (uint32_t i = 0; i < markings->count; i++)
	{
		VkDeviceSize size = 0;
		switch (markings->pMarkingTypes[i])
		{
		case VK_MARKING_TYPE_DEVICE_ADDRESS_ARM:
			size = sizeof(VkDeviceAddress);
			break;
		default:
			return false;
		}
		if (markings->pOffsets[i] > stage.pSpecializationInfo->dataSize
			|| size > stage.pSpecializationInfo->dataSize - markings->pOffsets[i])
		{
			return false;
		}
	}
	return true;
}

static uint32_t pipeline_index_from_output_handle(VkPipeline pipeline)
{
	if (pipeline == VK_NULL_HANDLE) return CONTAINER_INVALID_INDEX;
	return index_to_VkPipeline.index(pipeline);
}

static void output_vkCmdPushConstants2(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfo* pPushConstantsInfo)
{
	assert(pPushConstantsInfo);
	VkPushConstantsInfo patched = *pPushConstantsInfo;
	const VkMarkedOffsetsARM* injected = maybe_patch_push_constants_info(cb.reader, patched);
	prepare_trace_callback(cb);
	trace_vkCmdPushConstants2(commandBuffer, &patched);
	free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(injected));
}

static void output_vkCmdPushConstants2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfo* pPushConstantsInfo)
{
	assert(pPushConstantsInfo);
	VkPushConstantsInfo patched = *pPushConstantsInfo;
	const VkMarkedOffsetsARM* injected = maybe_patch_push_constants_info(cb.reader, patched);
	prepare_trace_callback(cb);
	trace_vkCmdPushConstants2KHR(commandBuffer, &patched);
	free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(injected));
}

static void output_vkCmdPushConstants(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags,
	uint32_t offset, uint32_t size, const void* pValues)
{
	VkPushConstantsInfo patched = {
		.sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
		.pNext = nullptr,
		.layout = layout,
		.stageFlags = stageFlags,
		.offset = offset,
		.size = size,
		.pValues = pValues,
	};
	const VkMarkedOffsetsARM* injected = maybe_patch_push_constants_info(cb.reader, patched);
	prepare_trace_callback(cb);
	if (injected)
	{
		trace_vkCmdPushConstants2KHR(commandBuffer, &patched);
	}
	else
	{
		trace_vkCmdPushConstants(commandBuffer, layout, stageFlags, offset, size, pValues);
	}
	free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(injected));
}

static void output_vkCreateComputePipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	assert(pCreateInfos || createInfoCount == 0);
	std::vector<VkComputePipelineCreateInfo> patched;
	if (createInfoCount > 0) patched.assign(pCreateInfos, pCreateInfos + createInfoCount);
	std::vector<const VkMarkedOffsetsARM*> injected(createInfoCount, nullptr);
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		const uint32_t pipeline_index = pipeline_index_from_output_handle(pPipelines ? pPipelines[i] : VK_NULL_HANDLE);
		if (pipeline_index == CONTAINER_INVALID_INDEX) continue;
		auto rewrite_it = find_stage_rewrite_entry(cb.reader, VK_OBJECT_TYPE_PIPELINE, pipeline_index, 0);
		if (rewrite_it == cb.reader.rewrite_queue.end()) continue;
		if (!marked_offsets_fit_specialization_data(rewrite_it->markings, patched[i].stage))
		{
			ABORT("Simulated specialization-constant markings do not fit vkCreateComputePipelines pipeline=%u stage=0 for %s",
				(unsigned)pipeline_index, describe_change_source(cb.reader.current).c_str());
		}
		injected[i] = patch_pipeline_shader_stage_info(cb.reader, rewrite_it, patched[i].stage, "vkCreateComputePipelines");
	}

	prepare_trace_callback(cb);
	trace_vkCreateComputePipelines(device, pipelineCache, createInfoCount, createInfoCount ? patched.data() : nullptr, pAllocator, pPipelines);

	for (const VkMarkedOffsetsARM* marking : injected)
	{
		free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(marking));
	}
}

static void output_vkCreateGraphicsPipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	assert(pCreateInfos || createInfoCount == 0);
	std::vector<VkGraphicsPipelineCreateInfo> patched;
	if (createInfoCount > 0) patched.assign(pCreateInfos, pCreateInfos + createInfoCount);
	std::vector<std::vector<VkPipelineShaderStageCreateInfo>> patched_stages(createInfoCount);
	std::vector<const VkMarkedOffsetsARM*> injected;
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		if (pCreateInfos[i].stageCount > 0 && pCreateInfos[i].pStages)
		{
			patched_stages[i].assign(pCreateInfos[i].pStages, pCreateInfos[i].pStages + pCreateInfos[i].stageCount);
			patched[i].pStages = patched_stages[i].data();
		}
		else
		{
			continue;
		}
		const uint32_t pipeline_index = pipeline_index_from_output_handle(pPipelines ? pPipelines[i] : VK_NULL_HANDLE);
		if (pipeline_index == CONTAINER_INVALID_INDEX) continue;
		for (uint32_t stage = 0; stage < patched[i].stageCount; stage++)
		{
			auto rewrite_it = find_stage_rewrite_entry(cb.reader, VK_OBJECT_TYPE_PIPELINE, pipeline_index, stage);
			if (rewrite_it == cb.reader.rewrite_queue.end()) continue;
			if (!marked_offsets_fit_specialization_data(rewrite_it->markings, patched_stages[i][stage]))
			{
				ABORT("Simulated specialization-constant markings do not fit vkCreateGraphicsPipelines pipeline=%u stage=%u for %s",
					(unsigned)pipeline_index, (unsigned)stage, describe_change_source(cb.reader.current).c_str());
			}
			injected.push_back(patch_pipeline_shader_stage_info(cb.reader, rewrite_it, patched_stages[i][stage], "vkCreateGraphicsPipelines"));
		}
	}

	prepare_trace_callback(cb);
	trace_vkCreateGraphicsPipelines(device, pipelineCache, createInfoCount, createInfoCount ? patched.data() : nullptr, pAllocator, pPipelines);

	for (const VkMarkedOffsetsARM* marking : injected)
	{
		free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(marking));
	}
}

static void output_vkCreateRayTracingPipelinesKHR(callback_context& cb, VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache,
	uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	assert(pCreateInfos || createInfoCount == 0);
	std::vector<VkRayTracingPipelineCreateInfoKHR> patched;
	if (createInfoCount > 0) patched.assign(pCreateInfos, pCreateInfos + createInfoCount);
	std::vector<std::vector<VkPipelineShaderStageCreateInfo>> patched_stages(createInfoCount);
	std::vector<const VkMarkedOffsetsARM*> injected;
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		if (pCreateInfos[i].stageCount > 0 && pCreateInfos[i].pStages)
		{
			patched_stages[i].assign(pCreateInfos[i].pStages, pCreateInfos[i].pStages + pCreateInfos[i].stageCount);
			patched[i].pStages = patched_stages[i].data();
		}
		else
		{
			continue;
		}
		const uint32_t pipeline_index = pipeline_index_from_output_handle(pPipelines ? pPipelines[i] : VK_NULL_HANDLE);
		if (pipeline_index == CONTAINER_INVALID_INDEX) continue;
		for (uint32_t stage = 0; stage < patched[i].stageCount; stage++)
		{
			auto rewrite_it = find_stage_rewrite_entry(cb.reader, VK_OBJECT_TYPE_PIPELINE, pipeline_index, stage);
			if (rewrite_it == cb.reader.rewrite_queue.end()) continue;
			if (!marked_offsets_fit_specialization_data(rewrite_it->markings, patched_stages[i][stage]))
			{
				ABORT("Simulated specialization-constant markings do not fit vkCreateRayTracingPipelinesKHR pipeline=%u stage=%u for %s",
					(unsigned)pipeline_index, (unsigned)stage, describe_change_source(cb.reader.current).c_str());
			}
			injected.push_back(patch_pipeline_shader_stage_info(cb.reader, rewrite_it, patched_stages[i][stage], "vkCreateRayTracingPipelinesKHR"));
		}
	}

	prepare_trace_callback(cb);
	trace_vkCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, createInfoCount ? patched.data() : nullptr, pAllocator, pPipelines);

	for (const VkMarkedOffsetsARM* marking : injected)
	{
		free_marked_offsets(const_cast<VkMarkedOffsetsARM*>(marking));
	}
}

static bool descriptor_output_marking_exists(uint32_t buffer_index, VkDeviceSize offset, VkDescriptorType type)
{
	return std::any_of(existing_descriptor_output_markings.begin(), existing_descriptor_output_markings.end(),
		[&](const descriptor_output_marking_key& key)
		{
			return key.buffer_index == buffer_index && key.offset == offset && key.type == type;
		});
}

static void cache_existing_descriptor_output_markings(const std::list<address_rewrite>& queue)
{
	existing_descriptor_output_markings.clear();
	for (const address_rewrite& rewrite : queue)
	{
		if (rewrite.object_type != VK_OBJECT_TYPE_BUFFER || !rewrite.markings) continue;
		const VkMarkedOffsetsARM* markings = rewrite.markings;
		if (!markings->pMarkingTypes || !markings->pSubTypes || !markings->pOffsets) continue;
		for (uint32_t i = 0; i < markings->count; i++)
		{
			if (markings->pMarkingTypes[i] != VK_MARKING_TYPE_DESCRIPTOR_ARM) continue;
			existing_descriptor_output_markings.push_back({
				.buffer_index = rewrite.object_index,
				.offset = markings->pOffsets[i],
				.type = markings->pSubTypes[i].descriptorType,
			});
		}
	}
}

static trackedbuffer* find_output_descriptor_buffer(VkDeviceAddress address, VkDeviceSize size, VkDeviceSize& out_offset)
{
	for (uint32_t i = 0; i < index_to_VkBuffer.size(); i++)
	{
		trackedbuffer& buffer = VkBuffer_index.at(i);
		if (buffer.device_address == 0 || buffer.size == 0) continue;
		if (address < buffer.device_address) continue;
		const VkDeviceSize offset = address - buffer.device_address;
		if (offset + size > buffer.size) continue;
		out_offset = offset;
		return &buffer;
	}
	return nullptr;
}

static bool pop_pending_descriptor_payload(VkDescriptorType type, const std::vector<uint8_t>* captured_bytes, std::vector<uint8_t>& bytes)
{
	auto it = std::find_if(pending_descriptor_payloads.begin(), pending_descriptor_payloads.end(),
		[type, captured_bytes](const pending_descriptor_payload& payload)
		{
			if (payload.type != type) return false;
			if (!captured_bytes) return true;
			return payload.bytes == *captured_bytes;
		});
	if (it == pending_descriptor_payloads.end()) return false;
	bytes = std::move(it->bytes);
	pending_descriptor_payloads.erase(it);
	return true;
}

static bool find_pending_descriptor_payload(VkDescriptorType type, const std::vector<uint8_t>& captured_bytes, std::vector<uint8_t>& bytes)
{
	auto it = std::find_if(pending_descriptor_payloads.begin(), pending_descriptor_payloads.end(),
		[type, &captured_bytes](const pending_descriptor_payload& payload)
		{
			return payload.type == type && payload.bytes == captured_bytes;
		});
	if (it == pending_descriptor_payloads.end()) return false;
	bytes = it->bytes;
	return true;
}

static bool find_unique_pending_descriptor_payload(VkDescriptorType type, std::vector<uint8_t>& bytes)
{
	bool found = false;
	for (const pending_descriptor_payload& payload : pending_descriptor_payloads)
	{
		if (payload.type != type) continue;
		if (!found)
		{
			bytes = payload.bytes;
			found = true;
			continue;
		}
		if (payload.bytes != bytes) return false;
	}
	return found;
}

static void note_used_descriptor_payload(VkDescriptorType type, const std::vector<uint8_t>& bytes)
{
	auto it = std::find_if(used_descriptor_payloads.begin(), used_descriptor_payloads.end(),
		[type, &bytes](const pending_descriptor_payload& payload)
		{
			return payload.type == type && payload.bytes == bytes;
		});
	if (it != used_descriptor_payloads.end()) return;
	used_descriptor_payloads.push_back({
		.type = type,
		.bytes = bytes,
	});
}

static void erase_used_descriptor_payloads()
{
	for (const pending_descriptor_payload& used : used_descriptor_payloads)
	{
		for (auto it = pending_descriptor_payloads.begin(); it != pending_descriptor_payloads.end();)
		{
			if (it->type == used.type && it->bytes == used.bytes)
			{
				it = pending_descriptor_payloads.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	used_descriptor_payloads.clear();
}

static bool read_output_descriptor_buffer_payload(uint32_t buffer_index, VkDeviceSize offset, size_t size, std::vector<uint8_t>& bytes)
{
	if (size == 0) return false;
	trackedbuffer& buffer = VkBuffer_index.at(buffer_index);
	if (offset > buffer.size || size > buffer.size - offset) return false;
	if (buffer.parent_device_index == CONTAINER_INVALID_INDEX) return false;
	const trackeddevice& device = VkDevice_index.at(buffer.parent_device_index);
	if (!device.allocator) return false;
	suballoc_location loc = device.allocator->find_buffer_memory(buffer_index);
	if (!loc.mapped) return false;
	bytes.resize(size);
	memcpy(bytes.data(), loc.mapped + offset, size);
	return true;
}

static bool same_descriptor_buffer_update(const pending_descriptor_buffer_update& update,
	uint32_t buffer_index, VkDeviceSize offset, VkDescriptorType type, const std::vector<uint8_t>& bytes)
{
	return update.buffer_index == buffer_index && update.offset == offset && update.type == type && update.bytes == bytes;
}

static bool descriptor_buffer_update_already_queued(uint32_t buffer_index, VkDeviceSize offset, VkDescriptorType type,
	const std::vector<uint8_t>& bytes)
{
	return std::any_of(pending_descriptor_buffer_updates.begin(), pending_descriptor_buffer_updates.end(),
			[&](const pending_descriptor_buffer_update& update)
			{
				return same_descriptor_buffer_update(update, buffer_index, offset, type, bytes);
			})
		|| std::any_of(emitted_descriptor_buffer_updates.begin(), emitted_descriptor_buffer_updates.end(),
			[&](const pending_descriptor_buffer_update& update)
			{
				return same_descriptor_buffer_update(update, buffer_index, offset, type, bytes);
			});
}

static void queue_synthetic_descriptor_buffer_update(uint32_t buffer_index, VkDeviceSize offset, VkDescriptorType type)
{
	std::vector<uint8_t> bytes;
	auto payload_entry = std::find_if(descriptor_buffer_payloads_for_output.begin(), descriptor_buffer_payloads_for_output.end(),
		[&](const descriptor_buffer_payload& payload)
		{
			return payload.buffer_index == buffer_index && payload.offset == offset && payload.type == type;
		});
	if (payload_entry != descriptor_buffer_payloads_for_output.end())
	{
		const uint32_t matching_payload_slots = std::count_if(descriptor_buffer_payloads_for_output.begin(), descriptor_buffer_payloads_for_output.end(),
			[&](const descriptor_buffer_payload& payload)
			{
				return payload.type == type && payload.bytes == payload_entry->bytes;
			});
		if (matching_payload_slots > 1) bytes = payload_entry->bytes;
	}
	if (bytes.empty())
	{
		if (descriptor_output_marking_exists(buffer_index, offset, type)) return;
		std::vector<uint8_t> captured_bytes;
		const std::vector<uint8_t>* captured_bytes_ptr = nullptr;
		auto payload_it = std::find_if(pending_descriptor_payloads.begin(), pending_descriptor_payloads.end(),
			[type](const pending_descriptor_payload& payload)
			{
				return payload.type == type;
			});
		if (payload_it != pending_descriptor_payloads.end()
			&& read_output_descriptor_buffer_payload(buffer_index, offset, payload_it->bytes.size(), captured_bytes))
		{
			captured_bytes_ptr = &captured_bytes;
		}
		const bool found_payload = (captured_bytes_ptr && find_pending_descriptor_payload(type, captured_bytes, bytes))
			|| find_unique_pending_descriptor_payload(type, bytes)
			|| pop_pending_descriptor_payload(type, captured_bytes_ptr, bytes)
			|| (captured_bytes_ptr && pop_pending_descriptor_payload(type, nullptr, bytes));
		if (!found_payload)
		{
			return;
		}
	}
	if (bytes.empty()) return;
	if (descriptor_buffer_update_already_queued(buffer_index, offset, type, bytes)) return;
	note_used_descriptor_payload(type, bytes);
	pending_descriptor_buffer_update update = {
		.buffer_index = buffer_index,
		.offset = offset,
		.type = type,
		.bytes = std::move(bytes),
	};
	emitted_descriptor_buffer_updates.push_back(update);
	pending_descriptor_buffer_updates.push_back(std::move(update));
}

static void write_descriptor_buffer_update_packet(lava_file_writer& writer, uint32_t buffer_index,
	const std::vector<pending_descriptor_buffer_update>& updates)
{
	if (updates.empty()) return;
	const trackedbuffer& reader_buffer = VkBuffer_index.at(buffer_index);
	VkDevice device = index_to_VkDevice.at(reader_buffer.parent_device_index);
	VkBuffer buffer = index_to_VkBuffer.at(buffer_index);
	trackeddevice* writer_device = writer.parent->records.VkDevice_index.at(device);
	trackedbuffer* writer_buffer = writer.parent->records.VkBuffer_index.at(buffer);
	if (!writer_device || !writer_buffer) ABORT("Failed to resolve writer-side descriptor buffer update target");

	std::vector<VkDeviceSize> offsets;
	std::vector<VkMarkingTypeARM> types;
	std::vector<VkMarkingSubTypeARM> subtypes;
	offsets.reserve(updates.size());
	types.reserve(updates.size());
	subtypes.reserve(updates.size());
	for (const pending_descriptor_buffer_update& update : updates)
	{
		VkMarkingSubTypeARM subtype{};
		subtype.descriptorType = update.type;
		offsets.push_back(update.offset);
		types.push_back(VK_MARKING_TYPE_DESCRIPTOR_ARM);
		subtypes.push_back(subtype);
	}
	VkMarkedOffsetsARM markings = {
		.sType = VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM,
		.pNext = nullptr,
		.count = static_cast<uint32_t>(updates.size()),
		.pMarkingTypes = types.data(),
		.pSubTypes = subtypes.data(),
		.pOffsets = offsets.data(),
	};

	writer.begin_packet(PACKET_BUFFER_UPDATE2);
	writer.write_handle(writer_device);
	writer.write_handle(writer_buffer);
	uint64_t* sizeptr = writer.write_later_uint64_t();
	const uint64_t packet_payload_start = writer.uncompressed_bytes;
	writer.write_uint16_t(PACKET_FLAG_HAS_PNEXT);
	write_marked_offsets_extension(writer, &markings);

	std::vector<const pending_descriptor_buffer_update*> sorted;
	sorted.reserve(updates.size());
	for (const pending_descriptor_buffer_update& update : updates) sorted.push_back(&update);
	std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->offset < b->offset; });
	VkDeviceSize cursor = 0;
	uint64_t written = 0;
	for (const pending_descriptor_buffer_update* update : sorted)
	{
		if (update->offset < cursor) ABORT("Overlapping synthetic descriptor buffer updates are not supported");
		writer.write_uint32_t(static_cast<uint32_t>(update->offset - cursor));
		writer.write_uint32_t(static_cast<uint32_t>(update->bytes.size()));
		writer.write_array(reinterpret_cast<const char*>(update->bytes.data()), update->bytes.size());
		cursor = update->offset + update->bytes.size();
		written += update->bytes.size();
	}
	writer.write_uint32_t(0);
	writer.write_uint32_t(0);
	writer_buffer->updates++;
	writer_buffer->written += written;
	*sizeptr = writer.uncompressed_bytes - packet_payload_start;
	writer.end_packet();
}

static void flush_synthetic_descriptor_buffer_updates()
{
	if (pending_descriptor_buffer_updates.empty())
	{
		erase_used_descriptor_payloads();
		return;
	}
	std::sort(pending_descriptor_buffer_updates.begin(), pending_descriptor_buffer_updates.end(),
		[](const pending_descriptor_buffer_update& a, const pending_descriptor_buffer_update& b)
		{
			if (a.buffer_index != b.buffer_index) return a.buffer_index < b.buffer_index;
			return a.offset < b.offset;
		});

	lava_file_writer& writer = lava_writer::instance().file_writer();
	std::vector<pending_descriptor_buffer_update> group;
	uint32_t current_buffer = CONTAINER_INVALID_INDEX;
	for (const pending_descriptor_buffer_update& update : pending_descriptor_buffer_updates)
	{
		if (current_buffer != update.buffer_index)
		{
			write_descriptor_buffer_update_packet(writer, current_buffer, group);
			group.clear();
			current_buffer = update.buffer_index;
		}
		group.push_back(update);
	}
	write_descriptor_buffer_update_packet(writer, current_buffer, group);
	pending_descriptor_buffer_updates.clear();
	erase_used_descriptor_payloads();
}

static void output_note_descriptor_buffer_offsets(VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
	const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets)
{
	const uint32_t layout_index = index_to_VkPipelineLayout.index(layout);
	if (layout_index == CONTAINER_INVALID_INDEX) return;
	const trackedpipelinelayout& layout_data = VkPipelineLayout_index.at(layout_index);
	for (uint32_t i = 0; i < setCount; i++)
	{
		const uint32_t set = firstSet + i;
		if (set >= layout_data.descriptor_set_layout_count()) continue;
		const uint32_t descriptor_buffer_index = pBufferIndices ? pBufferIndices[i] : 0;
		if (descriptor_buffer_index >= output_descriptor_buffers.size()) continue;
		const output_descriptor_buffer_binding& descriptor_buffer = output_descriptor_buffers[descriptor_buffer_index];
		if (descriptor_buffer.buffer_index == CONTAINER_INVALID_INDEX) continue;
		const VkDeviceSize set_offset = pOffsets ? pOffsets[i] : 0;
		uint32_t set_layout_index = CONTAINER_INVALID_INDEX;
		if (set < layout_data.layout_indices.size()) set_layout_index = layout_data.layout_indices[set];
		if (set_layout_index == CONTAINER_INVALID_INDEX && set < layout_data.layouts.size())
		{
			set_layout_index = index_to_VkDescriptorSetLayout.index(layout_data.layouts[set]);
		}
		if (set_layout_index == CONTAINER_INVALID_INDEX) continue;
		const trackeddescriptorsetlayout& set_layout_data = VkDescriptorSetLayout_index.at(set_layout_index);
		for (const auto& binding_pair : set_layout_data.binding_types)
		{
			const uint32_t binding = binding_pair.first;
			const auto offset_it = set_layout_data.offsets.find(binding);
			const VkDeviceSize binding_offset = offset_it != set_layout_data.offsets.end() ? offset_it->second : 0;
			queue_synthetic_descriptor_buffer_update(descriptor_buffer.buffer_index,
				descriptor_buffer.offset + set_offset + binding_offset, binding_pair.second);
		}
	}
}

static void output_vkGetDescriptorEXT(callback_context& cb, VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo,
	size_t dataSize, void* pDescriptor)
{
	prepare_trace_callback(cb);
	trace_vkGetDescriptorEXT(device, pDescriptorInfo, dataSize, pDescriptor);
	if (!pDescriptorInfo || !pDescriptor || dataSize == 0) return;
	pending_descriptor_payload payload;
	payload.type = pDescriptorInfo->type;
	payload.bytes.resize(dataSize);
	memcpy(payload.bytes.data(), pDescriptor, dataSize);
	pending_descriptor_payloads.push_back(std::move(payload));
}

static void note_descriptor_payload(callback_context& cb, VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo,
	size_t dataSize, void* pDescriptor)
{
	(void)device;
	if (!pDescriptorInfo || !pDescriptor || dataSize == 0) return;
	descriptor_rewrite payload;
	payload.type = pDescriptorInfo->type;
	payload.capture_bytes.resize(dataSize);
	payload.bytes.resize(dataSize);
	payload.source = cb.reader.current;
	memcpy(payload.capture_bytes.data(), pDescriptor, dataSize);
	memcpy(payload.bytes.data(), pDescriptor, dataSize);
	lava::lock_guard lock(sync_mutex);
	cb.reader.parent->pending_descriptor_rewrites.push_back(std::move(payload));
}

static void output_vkCmdBindDescriptorBuffersEXT(callback_context& cb, VkCommandBuffer commandBuffer, uint32_t bufferCount,
	const VkDescriptorBufferBindingInfoEXT* pBindingInfos)
{
	prepare_trace_callback(cb);
	trace_vkCmdBindDescriptorBuffersEXT(commandBuffer, bufferCount, pBindingInfos);

	output_descriptor_buffers.clear();
	output_descriptor_buffers.resize(bufferCount);
	if (!pBindingInfos) return;
	for (uint32_t i = 0; i < bufferCount; i++)
	{
		VkDeviceSize buffer_offset = 0;
		trackedbuffer* buffer = find_output_descriptor_buffer(pBindingInfos[i].address, 1, buffer_offset);
		if (!buffer) continue;
		output_descriptor_buffers[i] = {
			.buffer_index = buffer->index,
			.offset = buffer_offset,
			.size = buffer->size - buffer_offset,
			.usage = pBindingInfos[i].usage,
		};
	}
}

static void output_vkCmdSetDescriptorBufferOffsetsEXT(callback_context& cb, VkCommandBuffer commandBuffer,
	VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
	const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets)
{
	prepare_trace_callback(cb);
	trace_vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets);
	output_note_descriptor_buffer_offsets(layout, firstSet, setCount, pBufferIndices, pOffsets);
}

static void output_vkCmdSetDescriptorBufferOffsets2EXT(callback_context& cb, VkCommandBuffer commandBuffer,
	const VkSetDescriptorBufferOffsetsInfoEXT* pSetDescriptorBufferOffsetsInfo)
{
	prepare_trace_callback(cb);
	trace_vkCmdSetDescriptorBufferOffsets2EXT(commandBuffer, pSetDescriptorBufferOffsetsInfo);
	if (!pSetDescriptorBufferOffsetsInfo) return;
	output_note_descriptor_buffer_offsets(pSetDescriptorBufferOffsetsInfo->layout, pSetDescriptorBufferOffsetsInfo->firstSet,
		pSetDescriptorBufferOffsetsInfo->setCount, pSetDescriptorBufferOffsetsInfo->pBufferIndices,
		pSetDescriptorBufferOffsetsInfo->pOffsets);
}

static void output_vkQueueSubmit(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
	flush_synthetic_descriptor_buffer_updates();
	prepare_trace_callback(cb);
	trace_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

static void output_vkQueueSubmit2(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence)
{
	flush_synthetic_descriptor_buffer_updates();
	prepare_trace_callback(cb);
	trace_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

static void output_vkQueueSubmit2KHR(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence)
{
	flush_synthetic_descriptor_buffer_updates();
	prepare_trace_callback(cb);
	trace_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

// Utility funcs

static bool rewrite_call_less(const address_rewrite& a, const address_rewrite& b)
{
	return a.source.packet < b.source.packet;
}

void usage()
{
	printf("lava-tool %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-tool [options] <input filename> [<output filename>]\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-S/--simulate          Run a simulation and write discovered memory markings to the output trace\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
	printf("-f/--frames start end  Select a frame range\n");
	printf("-u/--unused            Find any found unused features and extensions in the trace file; remove them from the output file\n");
	printf("-DS/--dump-shaders     Dump any shaders found to disk\n");
	printf("-hw/--host-write-stats Dump host-side write tracking stats after replay\n");
	printf("--skip-missing-input   Exit with code 77 if the input trace file does not exist\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
	exit(-1);
}

static void discard_ignored_flush_rewrites(std::list<address_rewrite>& queue)
{
	for (auto it = queue.begin(); it != queue.end();)
	{
		if (!is_flush_markings_source(it->source))
		{
			++it;
			continue;
		}
		ILOG("Skipping uninjected VkMarkedOffsetsARM on %s", describe_change_source(it->source).c_str());
		free_marked_offsets(it->markings);
		it = queue.erase(it);
	}
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
			uint32_t output_packet = 0;
			if (write_output)
			{
				t.current_update_packet.clear();
				packet_start = t.packet_start();
				if (instrtype == PACKET_VULKAN_API_CALL || is_update_packet(instrtype)) output_writer->activate_thread_barriers();
				if (instrtype != PACKET_VULKAN_API_CALL && instrtype != PACKET_THREAD_BARRIER && !is_update_packet(instrtype))
				{
					ABORT("Output mode does not yet support packet type %u on thread %u packet %u", (unsigned)instrtype, (unsigned)t.thread_index(), (unsigned)t.current.packet);
				}
				if (instrtype == PACKET_VULKAN_API_CALL)
				{
					output_packet = output_writer->current.packet;
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
				const uint64_t packet_end = t.packet_end();
				if (!simulate || !maybe_write_rewritten_update_packet(t, *output_writer, packet_start, packet_end))
				{
					write_output_update_packet(t, *output_writer, packet_start, packet_end);
				}
			}
			if (write_output && instrtype == PACKET_VULKAN_API_CALL && output_writer->current.packet == output_packet)
			{
				write_output_update_packet(t, *output_writer, packet_start, t.packet_end());
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
		CALLBACK(vkCmdBuildAccelerationStructuresKHR);
		CALLBACK(vkCmdBuildAccelerationStructuresIndirectKHR);
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
		CALLBACK(vkCreateDescriptorSetLayout);
		CALLBACK(vkCreatePipelineLayout);
		CALLBACK(vkGetDescriptorSetLayoutBindingOffsetEXT);
		CALLBACK(vkCmdBindDescriptorBuffersEXT);
		CALLBACK(vkCmdSetDescriptorBufferOffsetsEXT);
		CALLBACK(vkCmdSetDescriptorBufferOffsets2EXT);
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
		vkGetDescriptorEXT_callbacks.push_back(note_descriptor_payload);
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
	bool simulate_requested = false;

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
			simulate_requested = true;
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
	if (simulate_requested && filename_output.empty())
	{
		DIE("-S/--simulate requires an output filename; input-only simulation validation has been removed");
	}

	if (p__sandbox_level >= 3) sandbox_level_two();

	if (report_unused || dump_host_write_stats) simulate = true;

	std::list<address_rewrite> output_rewrite_queue_copy;

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
		replayer.simulate = simulate;
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
		output_rewrite_queue_copy = replayer.global_output_rewrite_queue;
		descriptor_buffer_payloads_for_output = replayer.descriptor_buffer_payloads;
		sync_mutex.unlock();

		if (dump_host_write_stats) dump_host_write_stats_report("pass1");
		reset_for_tools();
		replayer.finalize();
	}

	if (!filename_output.empty())
	{
		lava_reader replayer;
		replayer.pass = 1;
		replayer.run = false;
		replayer.write_output = true;
		replayer.validate = false;
		replayer.simulate = false;
		replayer.init(filename_input);
		replayer.set_frames(start, end);
		if (simulate)
		{
			for (const auto& v : output_rewrite_queue_copy)
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
		vkCreateDescriptorSetLayout_callbacks.push_back(postprocess_vkCreateDescriptorSetLayout);
		vkCreatePipelineLayout_callbacks.push_back(postprocess_vkCreatePipelineLayout);
		vkGetDescriptorSetLayoutBindingOffsetEXT_callbacks.push_back(postprocess_vkGetDescriptorSetLayoutBindingOffsetEXT);
		vkGetBufferDeviceAddress_callbacks.push_back(replay_callback_vkGetBufferDeviceAddress);
		vkGetBufferDeviceAddressKHR_callbacks.push_back(replay_callback_vkGetBufferDeviceAddressKHR);
		vkGetBufferDeviceAddressEXT_callbacks.push_back(replay_callback_vkGetBufferDeviceAddressEXT);
		vkBindBufferMemory_callbacks.clear();
		vkBindBufferMemory_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindBufferMemory, sync_output_vkBindBufferMemory>::call);
		vkBindImageMemory_callbacks.clear();
		vkBindImageMemory_callbacks.push_back(replay_trace_callback_with_pre<trace_vkBindImageMemory, sync_output_vkBindImageMemory>::call);
		vkGetDeviceQueue_callbacks.clear();
		vkGetDeviceQueue_callbacks.push_back(replay_trace_callback_with_pre<trace_vkGetDeviceQueue, sync_output_vkGetDeviceQueue>::call);
		vkGetDeviceQueue2_callbacks.clear();
		vkGetDeviceQueue2_callbacks.push_back(replay_trace_callback_with_pre<trace_vkGetDeviceQueue2, sync_output_vkGetDeviceQueue2>::call);
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
		vkCmdPushConstants_callbacks.clear();
		vkCmdPushConstants_callbacks.push_back(output_vkCmdPushConstants);
		vkCmdPushConstants2_callbacks.clear();
		vkCmdPushConstants2_callbacks.push_back(output_vkCmdPushConstants2);
		vkCmdPushConstants2KHR_callbacks.clear();
		vkCmdPushConstants2KHR_callbacks.push_back(output_vkCmdPushConstants2KHR);
		if (simulate)
		{
			cache_existing_descriptor_output_markings(output_rewrite_queue_copy);
			vkCreateGraphicsPipelines_callbacks.clear();
			vkCreateGraphicsPipelines_callbacks.push_back(output_vkCreateGraphicsPipelines);
			vkCreateComputePipelines_callbacks.clear();
			vkCreateComputePipelines_callbacks.push_back(output_vkCreateComputePipelines);
			vkCreateRayTracingPipelinesKHR_callbacks.clear();
			vkCreateRayTracingPipelinesKHR_callbacks.push_back(output_vkCreateRayTracingPipelinesKHR);
			vkGetDescriptorEXT_callbacks.clear();
			vkGetDescriptorEXT_callbacks.push_back(output_vkGetDescriptorEXT);
			vkCmdBindDescriptorBuffersEXT_callbacks.clear();
			vkCmdBindDescriptorBuffersEXT_callbacks.push_back(output_vkCmdBindDescriptorBuffersEXT);
			vkCmdSetDescriptorBufferOffsetsEXT_callbacks.clear();
			vkCmdSetDescriptorBufferOffsetsEXT_callbacks.push_back(output_vkCmdSetDescriptorBufferOffsetsEXT);
			vkCmdSetDescriptorBufferOffsets2EXT_callbacks.clear();
			vkCmdSetDescriptorBufferOffsets2EXT_callbacks.push_back(output_vkCmdSetDescriptorBufferOffsets2EXT);
			vkQueueSubmit_callbacks.clear();
			vkQueueSubmit_callbacks.push_back(output_vkQueueSubmit);
			vkQueueSubmit2_callbacks.clear();
			vkQueueSubmit2_callbacks.push_back(output_vkQueueSubmit2);
			vkQueueSubmit2KHR_callbacks.clear();
			vkQueueSubmit2KHR_callbacks.push_back(output_vkQueueSubmit2KHR);
		}
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
				discard_ignored_flush_rewrites(t.rewrite_queue);
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
