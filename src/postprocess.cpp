#include "postprocess.h"

#include "read_auto.h"
#include "read.h"
#include "util_auto.h"
#include "execute_commands.h"
#include <algorithm>
#include <cstring>

#pragma GCC diagnostic ignored "-Wunused-variable"
#if (__clang_major__ > 12) || (!defined(__llvm__) && defined(__GNUC__))
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

static void handle_VkWriteDescriptorSets(uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, bool clear)
{
	(void)clear;
	for (unsigned i = 0; i < descriptorWriteCount; i++)
	{
		const VkDescriptorType type = pDescriptorWrites[i].descriptorType;
		const uint32_t descriptorset_index = index_to_VkDescriptorSet.index(pDescriptorWrites[i].dstSet);
		auto& tds = VkDescriptorSet_index.at(descriptorset_index);
		const uint32_t binding = pDescriptorWrites[i].dstBinding;

		switch (type)
		{
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			for (unsigned j = 0; j < pDescriptorWrites[i].descriptorCount; j++)
			{
				const VkDescriptorImageInfo& info = pDescriptorWrites[i].pImageInfo[j];
				if (info.imageView == VK_NULL_HANDLE && info.sampler == VK_NULL_HANDLE) continue;
				image_access access { nullptr, info.imageLayout };
				if (info.imageView != VK_NULL_HANDLE)
				{
					const uint32_t view_index = index_to_VkImageView.index(info.imageView);
					auto& view_data = VkImageView_index.at(view_index);
					auto& image_data = VkImage_index.at(view_data.image_index);
					access.image_data = &image_data;
				}
				tds.bound_images[binding] = access;
			}
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			for (unsigned j = 0; j < pDescriptorWrites[i].descriptorCount; j++)
			{
				if (pDescriptorWrites[i].pBufferInfo[j].buffer == VK_NULL_HANDLE) continue;
				const uint32_t buffer_index = index_to_VkBuffer.index(pDescriptorWrites[i].pBufferInfo[j].buffer);
				auto& buffer_data = VkBuffer_index.at(buffer_index);
				VkDeviceSize size = pDescriptorWrites[i].pBufferInfo[j].range;
				if (size == VK_WHOLE_SIZE) size = buffer_data.size - pDescriptorWrites[i].pBufferInfo[j].offset;
				tds.bound_buffers[binding] = buffer_access { &buffer_data, pDescriptorWrites[i].pBufferInfo[j].offset, size };
			}
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			for (unsigned j = 0; j < pDescriptorWrites[i].descriptorCount; j++)
			{
				tds.dynamic_buffers[binding] = pDescriptorWrites[i].pBufferInfo[j];
			}
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			for (unsigned j = 0; j < pDescriptorWrites[i].descriptorCount; j++)
			{
				if (pDescriptorWrites[i].pTexelBufferView[j] == VK_NULL_HANDLE) continue;
				const uint32_t bufferview_index = index_to_VkBufferView.index(pDescriptorWrites[i].pTexelBufferView[j]);
				auto& bufferview_data = VkBufferView_index.at(bufferview_index);
				auto& buffer_data = VkBuffer_index.at(bufferview_data.buffer_index);
				VkDeviceSize size = bufferview_data.range;
				if (size == VK_WHOLE_SIZE) size = buffer_data.size - bufferview_data.offset;
				tds.bound_buffers[binding] = buffer_access { &buffer_data, bufferview_data.offset, size };
			}
			break;
		case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: // Provided by VK_VERSION_1_3
			{
				auto* ptr = (VkWriteDescriptorSetInlineUniformBlock*)find_extension(pDescriptorWrites[i].pNext, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
				assert(ptr);
				assert(ptr->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
				// TBD
			}
			break;
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: // Provided by VK_KHR_acceleration_structure
			{
				auto* ptr = (VkWriteDescriptorSetAccelerationStructureKHR *)find_extension(pDescriptorWrites[i].pNext, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
				assert(ptr);
				assert(ptr->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
				assert(ptr->accelerationStructureCount == pDescriptorWrites[i].descriptorCount);
				// TBD
			}
			break;
		case VK_DESCRIPTOR_TYPE_MUTABLE_EXT: // Provided by VK_EXT_mutable_descriptor_type
			ABORT("vkUpdateDescriptorSets using VK_EXT_mutable_descriptor_type not yet implemented");
			break;
		case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM: // Provided by VK_QCOM_image_processing
		case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM: // Provided by VK_QCOM_image_processing
			ABORT("VK_QCOM_image_processing not supported");
			break;
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: // Provided by VK_NV_ray_tracing
			ABORT("VK_NV_ray_tracing not supported");
			break;
		default:
			break;
		case VK_DESCRIPTOR_TYPE_MAX_ENUM:
			ABORT("Bad descriptor type in vkUpdateDescriptorSets");
			break;
		}
	}
}

static void handle_VkCopyDescriptorSets(uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies)
{
	if (descriptorCopyCount == 0 || !pDescriptorCopies) return;

	for (unsigned i = 0; i < descriptorCopyCount; i++)
	{
		const uint32_t src_index = index_to_VkDescriptorSet.index(pDescriptorCopies[i].srcSet);
		auto& src = VkDescriptorSet_index.at(src_index);
		const uint32_t dst_index = index_to_VkDescriptorSet.index(pDescriptorCopies[i].dstSet);
		auto& dst = VkDescriptorSet_index.at(dst_index);
		for (const auto& pair : src.bound_buffers) dst.bound_buffers[pair.first] = pair.second;
		for (const auto& pair : src.bound_images) dst.bound_images[pair.first] = pair.second;
		for (const auto& pair : src.dynamic_buffers) dst.dynamic_buffers[pair.first] = pair.second;
	}
}

void postprocess_vkCmdPushDescriptorSetKHR(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDPUSHDESCRIPTORSETKHR };
	cmd.source = cb.reader.current;
	cmd.data.push_descriptorset.pipelineBindPoint = pipelineBindPoint;
	cmd.data.push_descriptorset.layout = layout;
	cmd.data.push_descriptorset.set = set;
	// TBD handle pDescriptorWrites here
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdPushDescriptorSet(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites)
{
	postprocess_vkCmdPushDescriptorSetKHR(cb, commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}

void postprocess_vkCmdPushDescriptorSet2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfoKHR* pPushDescriptorSetInfo)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);

	// "If stageFlags specifies a subset of all stages corresponding to one or more pipeline bind points, the binding operation still affects all stages corresponding to
	// the given pipeline bind point(s) as if the equivalent original version of this command had been called with the same parameters. For example, specifying a
	// stageFlags value of VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT is equivalent to calling the original version of this
	// command once with VK_PIPELINE_BIND_POINT_GRAPHICS and once with VK_PIPELINE_BIND_POINT_COMPUTE."

	if (pPushDescriptorSetInfo->stageFlags & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT))
	{
		trackedcommand cmd { VKCMDPUSHDESCRIPTORSETKHR };
		cmd.source = cb.reader.current;
		cmd.data.push_descriptorset.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		cmd.data.push_descriptorset.layout = pPushDescriptorSetInfo->layout;
		cmd.data.push_descriptorset.set = pPushDescriptorSetInfo->set;
		cmd.data.push_descriptorset.descriptorWriteCount = pPushDescriptorSetInfo->descriptorWriteCount;
		// TBD handle pDescriptorWrites here
		cmdbuffer_data.commands.push_back(cmd);
	}
	if (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
	{
		trackedcommand cmd { VKCMDPUSHDESCRIPTORSETKHR };
		cmd.source = cb.reader.current;
		cmd.data.push_descriptorset.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
		cmd.data.push_descriptorset.layout = pPushDescriptorSetInfo->layout;
		cmd.data.push_descriptorset.set = pPushDescriptorSetInfo->set;
		cmd.data.push_descriptorset.descriptorWriteCount = pPushDescriptorSetInfo->descriptorWriteCount;
		// TBD handle pDescriptorWrites here
		cmdbuffer_data.commands.push_back(cmd);
	}
}

void postprocess_vkCmdPushDescriptorSet2(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo)
{
	postprocess_vkCmdPushDescriptorSet2KHR(cb, commandBuffer, pPushDescriptorSetInfo);
}

void postprocess_vkUpdateDescriptorSets(callback_context& cb, VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies)
{
	handle_VkWriteDescriptorSets(descriptorWriteCount, pDescriptorWrites, true);
	handle_VkCopyDescriptorSets(descriptorCopyCount, pDescriptorCopies);
}

void postprocess_vkCmdBindDescriptorSets(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
	uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDBINDDESCRIPTORSETS };
	cmd.source = cb.reader.current;
	cmd.data.bind_descriptorsets.pipelineBindPoint = pipelineBindPoint;
	cmd.data.bind_descriptorsets.layout = layout;
	cmd.data.bind_descriptorsets.firstSet = firstSet;
	cmd.data.bind_descriptorsets.descriptorSetCount = descriptorSetCount;
	if (descriptorSetCount > 0)
	{
		cmd.data.bind_descriptorsets.pDescriptorSets = (uint32_t*)malloc(descriptorSetCount * sizeof(uint32_t));
		for (uint32_t i = 0; i < descriptorSetCount; i++)
		{
			const uint32_t descriptorset_index = index_to_VkDescriptorSet.index(pDescriptorSets[i]);
			cmd.data.bind_descriptorsets.pDescriptorSets[i] = descriptorset_index;
		}
	}
	else cmd.data.bind_descriptorsets.pDescriptorSets = nullptr;
	cmd.data.bind_descriptorsets.dynamicOffsetCount = dynamicOffsetCount;
	if (dynamicOffsetCount > 0 && pDynamicOffsets)
	{
		cmd.data.bind_descriptorsets.pDynamicOffsets = (uint32_t*)malloc(dynamicOffsetCount * sizeof(uint32_t));
		memcpy(cmd.data.bind_descriptorsets.pDynamicOffsets, pDynamicOffsets, dynamicOffsetCount * sizeof(uint32_t));
	}
	else cmd.data.bind_descriptorsets.pDynamicOffsets = nullptr;
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdBindDescriptorSets2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfoKHR* pBindDescriptorSetsInfo)
{
	if ((pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_VERTEX_BIT) || (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT))
	{
		postprocess_vkCmdBindDescriptorSets(cb, commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pBindDescriptorSetsInfo->layout,
			pBindDescriptorSetsInfo->firstSet, pBindDescriptorSetsInfo->descriptorSetCount, pBindDescriptorSetsInfo->pDescriptorSets,
			pBindDescriptorSetsInfo->dynamicOffsetCount, pBindDescriptorSetsInfo->pDynamicOffsets);
	}
	if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
	{
		postprocess_vkCmdBindDescriptorSets(cb, commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pBindDescriptorSetsInfo->layout,
			pBindDescriptorSetsInfo->firstSet, pBindDescriptorSetsInfo->descriptorSetCount, pBindDescriptorSetsInfo->pDescriptorSets,
			pBindDescriptorSetsInfo->dynamicOffsetCount, pBindDescriptorSetsInfo->pDynamicOffsets);
	}
}

void postprocess_vkCmdBindDescriptorSets2(callback_context& cb, VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo)
{
	postprocess_vkCmdBindDescriptorSets2KHR(cb, commandBuffer, pBindDescriptorSetsInfo);
}

void postprocess_vkQueueSubmit2(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence)
{
	const uint32_t queue_index = index_to_VkQueue.index(queue);
	auto& queue_data = VkQueue_index.at(queue_index);
	const uint32_t device_index = queue_data.device_index;
	assert(device_index != UINT32_MAX);
	auto& device_data = VkDevice_index.at(device_index);
	for (uint32_t i = 0; i < submitCount; i++)
	{
		for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; j++)
		{
			execute_commands(cb.reader, device_data, pSubmits[i].pCommandBufferInfos[j].commandBuffer);
		}
	}
}

void postprocess_vkQueueSubmit2KHR(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2KHR* pSubmits, VkFence fence)
{
	postprocess_vkQueueSubmit2(cb, queue, submitCount, pSubmits, fence);
}

void postprocess_vkQueueSubmit(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
	const uint32_t queue_index = index_to_VkQueue.index(queue);
	auto& queue_data = VkQueue_index.at(queue_index);
	const uint32_t device_index = queue_data.device_index;
	assert(device_index != UINT32_MAX);
	auto& device_data = VkDevice_index.at(device_index);
	for (uint32_t i = 0; i < submitCount; i++)
	{
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++)
		{
			execute_commands(cb.reader, device_data, pSubmits[i].pCommandBuffers[j]);
		}
	}
}

void postprocess_vkCmdBindPipeline(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	const uint32_t pipeline_index = index_to_VkPipeline.index(pipeline);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDBINDPIPELINE };
	cmd.source = cb.reader.current;
	cmd.data.bind_pipeline.pipelineBindPoint = pipelineBindPoint;
	cmd.data.bind_pipeline.pipeline_index = pipeline_index;
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_draw_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data)
{
	trackedcommand cmd { VKCMDDRAW };
	cmd.source = cb.reader.current;
	commandbuffer_data.commands.push_back(cmd);
}

void postprocess_raytracing_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data)
{
	if (commandbuffer_data.pending_raytracing_marker)
	{
		commandbuffer_data.pending_raytracing_marker = false;
		return;
	}
	trackedcommand cmd { VKCMDTRACERAYSKHR };
	cmd.source = cb.reader.current;
	cmd.trace_rays_valid = false;
	commandbuffer_data.commands.push_back(cmd);
}

void postprocess_compute_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data)
{
	trackedcommand cmd { VKCMDDISPATCH };
	cmd.source = cb.reader.current;
	commandbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdUpdateBuffer(callback_context& cb, VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData)
{
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDUPDATEBUFFER };
	cmd.source = cb.reader.current;
	cmd.data.update_buffer.size = dataSize;
	cmd.data.update_buffer.offset = dstOffset;
	cmd.data.update_buffer.buffer_index = index_to_VkBuffer.index(dstBuffer);
	cmd.data.update_buffer.values = (char*)malloc(dataSize);
	memcpy(cmd.data.update_buffer.values, pData, dataSize);
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdCopyBuffer(callback_context& cb, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferCopy* pRegions)
{
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDCOPYBUFFER };
	cmd.source = cb.reader.current;
	cmd.data.copy_buffer.src_buffer_index = index_to_VkBuffer.index(srcBuffer);
	cmd.data.copy_buffer.dst_buffer_index = index_to_VkBuffer.index(dstBuffer);
	cmd.data.copy_buffer.regionCount = regionCount;
	cmd.data.copy_buffer.pRegions = (VkBufferCopy*)malloc(regionCount * sizeof(VkBufferCopy));
	memcpy(cmd.data.copy_buffer.pRegions, pRegions, regionCount * sizeof(VkBufferCopy));
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdCopyBuffer2(callback_context& cb, VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo)
{
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDCOPYBUFFER };
	cmd.source = cb.reader.current;
	cmd.data.copy_buffer.src_buffer_index = index_to_VkBuffer.index(pCopyBufferInfo->srcBuffer);
	cmd.data.copy_buffer.dst_buffer_index = index_to_VkBuffer.index(pCopyBufferInfo->dstBuffer);
	cmd.data.copy_buffer.regionCount = pCopyBufferInfo->regionCount;
	cmd.data.copy_buffer.pRegions = (VkBufferCopy*)malloc(pCopyBufferInfo->regionCount * sizeof(VkBufferCopy));
	memcpy(cmd.data.copy_buffer.pRegions, pCopyBufferInfo->pRegions, pCopyBufferInfo->regionCount * sizeof(VkBufferCopy));
	cmdbuffer_data.commands.push_back(cmd);
}

static void postprocess_push_constants(callback_context& cb, VkCommandBuffer commandBuffer, uint32_t offset, uint32_t size, const void* pValues)
{
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDPUSHCONSTANTS };
	cmd.source = cb.reader.current;
	cmd.data.push_constants.offset = offset;
	cmd.data.push_constants.size = size;
	cmd.data.push_constants.values = (char*)malloc(size);
	memcpy(cmd.data.push_constants.values, pValues, size);
	cmdbuffer_data.commands.push_back(cmd);
}

void postprocess_vkCmdPushConstants(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues)
{
	postprocess_push_constants(cb, commandBuffer, offset, size, pValues);
}

void postprocess_vkCmdPushConstants2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfoKHR* pPushConstantsInfo)
{
	postprocess_push_constants(cb, commandBuffer, pPushConstantsInfo->offset, pPushConstantsInfo->size, pPushConstantsInfo->pValues);
}

void postprocess_vkCmdPushConstants2(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfo* pPushConstantsInfo)
{
	postprocess_vkCmdPushConstants2KHR(cb, commandBuffer, pPushConstantsInfo);
}

static void copy_shader_stage(const trackedpipeline& pipeline_data, shader_stage& stage, const VkPipelineShaderStageCreateInfo& info)
{
	stage.device_index = pipeline_data.device_index;
	stage.flags = info.flags;
	stage.module = info.module;
	if (stage.module == VK_NULL_HANDLE) // allowed since maintenance 5
	{
		auto* idext = (VkPipelineShaderStageModuleIdentifierCreateInfoEXT*)find_extension(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT);
		assert(!idext); // not yet supported
		auto* smciext = (VkShaderModuleCreateInfo*)find_extension(info.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
		assert(smciext); // must have this if no module is defined!
		if (smciext)
		{
			stage.code.resize(smciext->codeSize / sizeof(uint32_t));
			memcpy(stage.code.data(), smciext->pCode, smciext->codeSize);
		}
	}
	else // old style shader modules
	{
		const uint32_t shader_index = index_to_VkShaderModule.index(stage.module);
		const auto& shader_data = VkShaderModule_index.at(shader_index);
		stage.code = shader_data.code;
	}
	stage.name = info.pName;
	stage.stage = info.stage;
	stage.unique_index = (uint64_t)pipeline_data.index | ((uint64_t)info.stage << 32);
	if (info.pSpecializationInfo)
	{
		stage.specialization_constants.resize(info.pSpecializationInfo->mapEntryCount);
		memcpy(stage.specialization_constants.data(), info.pSpecializationInfo->pMapEntries,
			info.pSpecializationInfo->mapEntryCount * sizeof(VkSpecializationMapEntry));
		stage.specialization_data.resize(info.pSpecializationInfo->dataSize);
		memcpy(stage.specialization_data.data(), info.pSpecializationInfo->pData, info.pSpecializationInfo->dataSize);
	}
	stage.self_test();
}

void postprocess_vkCreateComputePipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		const uint32_t pipeline_index = index_to_VkPipeline.index(pPipelines[i]);
		trackedpipeline& pipeline_data = VkPipeline_index.at(pipeline_index);
		pipeline_data.shader_stages.resize(1);
		pipeline_data.shader_stages[0].index = 0;
		copy_shader_stage(pipeline_data, pipeline_data.shader_stages[0], pCreateInfos[i].stage);
	}
}

void postprocess_vkCreateGraphicsPipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		const uint32_t pipeline_index = index_to_VkPipeline.index(pPipelines[i]);
		trackedpipeline& pipeline_data = VkPipeline_index.at(pipeline_index);
		pipeline_data.shader_stages.resize(pCreateInfos[i].stageCount);
		for (uint32_t stage = 0; stage < pCreateInfos[i].stageCount; stage++)
		{
			pipeline_data.shader_stages[stage].index = stage;
			copy_shader_stage(pipeline_data, pipeline_data.shader_stages[stage], pCreateInfos[i].pStages[stage]);
		}
	}
}


void postprocess_vkCreateRayTracingPipelinesKHR(callback_context& cb, VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache,
	uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		const uint32_t pipeline_index = index_to_VkPipeline.index(pPipelines[i]);
		trackedpipeline& pipeline_data = VkPipeline_index.at(pipeline_index);
		pipeline_data.shader_stages.resize(pCreateInfos[i].stageCount);
		for (uint32_t stage = 0; stage < pCreateInfos[i].stageCount; stage++)
		{
			pipeline_data.shader_stages[stage].index = stage;
			copy_shader_stage(pipeline_data, pipeline_data.shader_stages[stage], pCreateInfos[i].pStages[stage]);
		}
		pipeline_data.raytracing_group_count = pCreateInfos[i].groupCount;
		pipeline_data.raytracing_groups.resize(pCreateInfos[i].groupCount);
		for (uint32_t group = 0; group < pCreateInfos[i].groupCount; group++)
		{
			const VkRayTracingShaderGroupCreateInfoKHR& src = pCreateInfos[i].pGroups[group];
			raytracing_group& dst = pipeline_data.raytracing_groups[group];
			dst.type = src.type;
			dst.general_shader = src.generalShader;
			dst.closest_hit_shader = src.closestHitShader;
			dst.any_hit_shader = src.anyHitShader;
			dst.intersection_shader = src.intersectionShader;
		}
		pipeline_data.raytracing_group_handle_size = 0;
		pipeline_data.raytracing_group_handles.clear();
	}
}

void postprocess_vkGetRayTracingShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup,
	uint32_t groupCount, size_t dataSize, void* pData)
{
	if (!pData || groupCount == 0 || dataSize == 0) return;
	const uint32_t pipeline_index = index_to_VkPipeline.index(pipeline);
	trackedpipeline& pipeline_data = VkPipeline_index.at(pipeline_index);
	const size_t handle_size = dataSize / groupCount;
	if (handle_size == 0 || (dataSize % groupCount) != 0)
	{
		DLOG("vkGetRayTracingShaderGroupHandlesKHR has invalid dataSize=%zu for groupCount=%u", dataSize, groupCount);
		return;
	}
	if (pipeline_data.raytracing_group_handle_size == 0)
	{
		pipeline_data.raytracing_group_handle_size = (uint32_t)handle_size;
	}
	else if (pipeline_data.raytracing_group_handle_size != handle_size)
	{
		ABORT("Ray tracing shader group handle size mismatch for pipeline %u", pipeline_index);
	}
	const uint32_t required_groups = std::max(pipeline_data.raytracing_group_count, firstGroup + groupCount);
	pipeline_data.raytracing_group_count = required_groups;
	const size_t required_size = (size_t)required_groups * handle_size;
	if (pipeline_data.raytracing_group_handles.size() < required_size)
	{
		pipeline_data.raytracing_group_handles.resize(required_size);
	}
	std::byte* dst = pipeline_data.raytracing_group_handles.data() + (size_t)firstGroup * handle_size;
	memcpy(dst, pData, dataSize);
}

void postprocess_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup,
	uint32_t groupCount, size_t dataSize, void* pData)
{
	postprocess_vkGetRayTracingShaderGroupHandlesKHR(cb, device, pipeline, firstGroup, groupCount, dataSize, pData);
}

void postprocess_vkCreateShadersEXT(callback_context& cb, VkDevice device, uint32_t createInfoCount, const VkShaderCreateInfoEXT* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkShaderEXT* pShaders)
{
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		uint32_t shader_index = index_to_VkShaderEXT.index(pShaders[i]);
		auto& obj = VkShaderEXT_index.at(shader_index);
		if (pCreateInfos[i].pSpecializationInfo)
		{
			obj.stage.specialization_constants.resize(pCreateInfos[i].pSpecializationInfo->mapEntryCount);
			memcpy(obj.stage.specialization_constants.data(), pCreateInfos[i].pSpecializationInfo->pMapEntries,
				pCreateInfos[i].pSpecializationInfo->mapEntryCount * sizeof(VkSpecializationMapEntry));
			obj.stage.specialization_data.resize(pCreateInfos[i].pSpecializationInfo->dataSize);
			memcpy(obj.stage.specialization_data.data(), pCreateInfos[i].pSpecializationInfo->pData, pCreateInfos[i].pSpecializationInfo->dataSize);
		}
		if (pCreateInfos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && pCreateInfos[i].pCode && pCreateInfos[i].codeSize)
		{
			obj.stage.code.resize(pCreateInfos[i].codeSize / sizeof(uint32_t));
			memcpy(obj.stage.code.data(), pCreateInfos[i].pCode, pCreateInfos[i].codeSize);
		}
	}
}

void postprocess_vkCmdBindShadersEXT(callback_context& cb, VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits* pStages, const VkShaderEXT* pShaders)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDBINDSHADERSEXT };
	cmd.source = cb.reader.current;
	cmd.data.bind_shaders_ext.stageCount = stageCount;
	if (stageCount)
	{
		cmd.data.bind_shaders_ext.shader_types = (VkShaderStageFlagBits*)malloc(sizeof(VkShaderStageFlagBits) * stageCount);
		cmd.data.bind_shaders_ext.shader_objects = (uint32_t*)malloc(sizeof(uint32_t) * stageCount);
		for (uint32_t i = 0; i < stageCount; i++)
		{
			cmd.data.bind_shaders_ext.shader_types[i] = pStages[i];
			if (pShaders[i] == VK_NULL_HANDLE)
			{
				cmd.data.bind_shaders_ext.shader_objects[i] = CONTAINER_NULL_VALUE;
			}
			else
			{
				const uint32_t shader_index = index_to_VkShaderEXT.index(pShaders[i]);
				cmd.data.bind_shaders_ext.shader_objects[i] = shader_index;
			}
		}
	}
	else
	{
		cmd.data.bind_shaders_ext.shader_types = nullptr;
		cmd.data.bind_shaders_ext.shader_objects = nullptr;
	}
	cmdbuffer_data.commands.push_back(cmd);
}

static void fill_trace_rays_regions(trackedcommand& cmd, const VkStridedDeviceAddressRegionKHR* raygen, const VkStridedDeviceAddressRegionKHR* miss,
	const VkStridedDeviceAddressRegionKHR* hit, const VkStridedDeviceAddressRegionKHR* callable)
{
	cmd.data.trace_rays.raygen = raygen ? *raygen : VkStridedDeviceAddressRegionKHR{};
	cmd.data.trace_rays.miss = miss ? *miss : VkStridedDeviceAddressRegionKHR{};
	cmd.data.trace_rays.hit = hit ? *hit : VkStridedDeviceAddressRegionKHR{};
	cmd.data.trace_rays.callable = callable ? *callable : VkStridedDeviceAddressRegionKHR{};
}

void postprocess_vkCmdTraceRaysKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, uint32_t width, uint32_t height, uint32_t depth)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDTRACERAYSKHR };
	cmd.source = cb.reader.current;
	cmd.trace_rays_valid = true;
	cmd.data.trace_rays.mode = trackedcommand::TRACE_RAYS_DIRECT;
	fill_trace_rays_regions(cmd, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable);
	cmd.data.trace_rays.width = width;
	cmd.data.trace_rays.height = height;
	cmd.data.trace_rays.depth = depth;
	cmd.data.trace_rays.indirect_device_address = 0;
	cmdbuffer_data.commands.push_back(cmd);
	cmdbuffer_data.pending_raytracing_marker = true;
}

void postprocess_vkCmdTraceRaysIndirectKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, VkDeviceAddress indirectDeviceAddress)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDTRACERAYSKHR };
	cmd.source = cb.reader.current;
	cmd.trace_rays_valid = true;
	cmd.data.trace_rays.mode = trackedcommand::TRACE_RAYS_INDIRECT;
	fill_trace_rays_regions(cmd, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable);
	cmd.data.trace_rays.width = 0;
	cmd.data.trace_rays.height = 0;
	cmd.data.trace_rays.depth = 0;
	cmd.data.trace_rays.indirect_device_address = indirectDeviceAddress;
	cmdbuffer_data.commands.push_back(cmd);
	cmdbuffer_data.pending_raytracing_marker = true;
}

void postprocess_vkCmdTraceRaysIndirect2KHR(callback_context& cb, VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress)
{
	const uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	trackedcommand cmd { VKCMDTRACERAYSKHR };
	cmd.source = cb.reader.current;
	cmd.trace_rays_valid = true;
	cmd.data.trace_rays.mode = trackedcommand::TRACE_RAYS_INDIRECT2;
	fill_trace_rays_regions(cmd, nullptr, nullptr, nullptr, nullptr);
	cmd.data.trace_rays.width = 0;
	cmd.data.trace_rays.height = 0;
	cmd.data.trace_rays.depth = 0;
	cmd.data.trace_rays.indirect_device_address = indirectDeviceAddress;
	cmdbuffer_data.commands.push_back(cmd);
	cmdbuffer_data.pending_raytracing_marker = true;
}

void postprocess_vkSubmitDebugUtilsMessageEXT(callback_context& cb, VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData)
{
	if (!pCallbackData) return;
	if (pCallbackData->pObjects && pCallbackData->objectCount > 0 && pCallbackData->pMessage)
	{
		trackable& t = object_trackable(pCallbackData->pObjects[0].objectType, pCallbackData->pObjects[0].objectHandle);
		(void)t;
		DLOG("Marker for %s[%d]: " MAKEBLUE("%s"), pretty_print_VkObjectType(pCallbackData->pObjects[0].objectType), (int)t.index, pCallbackData->pMessage);
	}
	else if (pCallbackData->pMessage)
	{
		DLOG("Marker: " MAKEBLUE("%s"), pCallbackData->pMessage);
	}
}
