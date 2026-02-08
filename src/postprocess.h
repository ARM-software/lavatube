#pragma once

#include "lavatube.h"

void postprocess_vkCmdPushDescriptorSetKHR(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites);
void postprocess_vkCmdPushDescriptorSet(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites);
void postprocess_vkCmdPushDescriptorSet2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfoKHR* pPushDescriptorSetInfo);
void postprocess_vkCmdPushDescriptorSet2(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo);
void postprocess_vkUpdateDescriptorSets(callback_context& cb, VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies);
void postprocess_vkCmdUpdateBuffer(callback_context& cb, VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData);
void postprocess_vkCmdCopyBuffer(callback_context& cb, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferCopy* pRegions);
void postprocess_vkCmdCopyBuffer2(callback_context& cb, VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo);
void postprocess_vkCmdBindDescriptorSets(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
	uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets);
void postprocess_vkCmdBindDescriptorSets2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfoKHR* pBindDescriptorSetsInfo);
void postprocess_vkCmdBindDescriptorSets2(callback_context& cb, VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo);
void postprocess_vkQueueSubmit2(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
void postprocess_vkQueueSubmit2KHR(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo2KHR* pSubmits, VkFence fence);
void postprocess_vkQueueSubmit(callback_context& cb, VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
void postprocess_vkCmdBindPipeline(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline);
void postprocess_vkCmdPushConstants(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues);
void postprocess_vkCmdPushConstants2KHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfoKHR* pPushConstantsInfo);
void postprocess_vkCmdPushConstants2(callback_context& cb, VkCommandBuffer commandBuffer, const VkPushConstantsInfo* pPushConstantsInfo);
void postprocess_vkCreateComputePipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
void postprocess_vkCreateGraphicsPipelines(callback_context& cb, VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
void postprocess_vkCreateRayTracingPipelinesKHR(callback_context& cb, VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache,
	uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
void postprocess_vkGetRayTracingShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData);
void postprocess_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData);
void postprocess_vkCmdTraceRaysKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, uint32_t width, uint32_t height, uint32_t depth);
void postprocess_vkCmdTraceRaysIndirectKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, VkDeviceAddress indirectDeviceAddress);
void postprocess_vkCmdTraceRaysIndirect2KHR(callback_context& cb, VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress);
void postprocess_vkSubmitDebugUtilsMessageEXT(callback_context& cb, VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData);
void postprocess_vkCreateShadersEXT(callback_context& cb, VkDevice device, uint32_t createInfoCount, const VkShaderCreateInfoEXT* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkShaderEXT* pShaders);
void postprocess_vkCmdBindShadersEXT(callback_context& cb, VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits* pStages, const VkShaderEXT* pShaders);

// These three are special. They are called directly from the generated code. TBD: Should come up with a better interface for these.
void postprocess_draw_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data);
void postprocess_raytracing_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data);
void postprocess_compute_command(callback_context& cb, uint32_t commandbuffer_index, trackedcmdbuffer& commandbuffer_data);
