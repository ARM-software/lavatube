#pragma once

#include "read_auto.h"

void replay_callback_vkCreateInstance(callback_context& cb, const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
void replay_callback_vkDestroyInstance(callback_context& cb, VkInstance instance, const VkAllocationCallbacks* pAllocator);
void replay_callback_vkQueuePresentKHR(callback_context& cb, VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
void replay_callback_vkAcquireNextImageKHR(callback_context& cb, VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
void replay_callback_vkAcquireNextImage2KHR(callback_context& cb, VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex);
void replay_callback_vkGetBufferDeviceAddress(callback_context& cb, VkDevice device, const VkBufferDeviceAddressInfo* pInfo);
void replay_callback_vkGetBufferDeviceAddressKHR(callback_context& cb, VkDevice device, const VkBufferDeviceAddressInfoKHR* pInfo);
void replay_callback_vkGetBufferDeviceAddressEXT(callback_context& cb, VkDevice device, const VkBufferDeviceAddressInfo* pInfo);
void replay_callback_vkGetAccelerationStructureDeviceAddressKHR(callback_context& cb, VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo);
void replay_callback_vkCreateBuffer(callback_context& cb, VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer);
void replay_callback_vkCreateAccelerationStructureKHR(callback_context& cb, VkDevice device, const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkAccelerationStructureKHR* pAccelerationStructure);
void replay_callback_vkSubmitDebugUtilsMessageEXT(callback_context& cb, VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData);
void replay_callback_vkGetAccelerationStructureBuildSizesKHR(callback_context& cb, VkDevice device, VkAccelerationStructureBuildTypeKHR buildType,
	const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo, const uint32_t* pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo);
void replay_callback_vkCreateDescriptorUpdateTemplate(callback_context& cb, VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);
void replay_callback_vkCreateDescriptorUpdateTemplateKHR(callback_context& cb, VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);
void replay_callback_vkGetDataGraphPipelineSessionMemoryRequirementsARM(callback_context& cb, VkDevice device,
	const VkDataGraphPipelineSessionMemoryRequirementsInfoARM* pInfo, VkMemoryRequirements2* pMemoryRequirements);
void replay_callback_vkBindDataGraphPipelineSessionMemoryARM(callback_context& cb, VkDevice device, uint32_t bindInfoCount,
	const VkBindDataGraphPipelineSessionMemoryInfoARM* pBindInfos);

void replay_track_vkCmdBindPipeline(callback_context& cb, VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline);
void replay_track_vkGetRayTracingShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount,
	size_t dataSize, void* pData);
void replay_track_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(callback_context& cb, VkDevice device, VkPipeline pipeline, uint32_t firstGroup,
	uint32_t groupCount, size_t dataSize, void* pData);
void replay_fixup_vkCmdTraceRaysKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, uint32_t width, uint32_t height, uint32_t depth);
void replay_fixup_vkCmdTraceRaysIndirectKHR(callback_context& cb, VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
	const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, VkDeviceAddress indirectDeviceAddress);
void replay_fixup_vkCmdTraceRaysIndirect2KHR(callback_context& cb, VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress);
