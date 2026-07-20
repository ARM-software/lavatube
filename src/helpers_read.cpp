#include "helpers_read.h"
#include "read_auto.h"
#include "window.h"

VkInstance stored_instance = VK_NULL_HANDLE;
VkPhysicalDevice selected_physical_device = VK_NULL_HANDLE;
uint32_t selected_queue_family_index = 0xdeadbeef;
bool has_pipeline_feedback = false;
bool has_pipeline_control = false;
bool has_debug_report = false;
bool has_debug_utils = false;
bool host_has_frame_boundary = false;

#ifndef VK_ANDROID_FRAME_BOUNDARY_EXTENSION_NAME
#define VK_ANDROID_FRAME_BOUNDARY_EXTENSION_NAME "VK_ANDROID_frame_boundary"
#endif

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#if (__clang_major__ > 12) || (!defined(__llvm__) && defined(__GNUC__))
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#ifndef __clang__
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

uint64_t debug_object_lookup(VkDebugReportObjectTypeEXT type, uint32_t index)
{
	switch (type)
	{
	case VK_DEBUG_REPORT_OBJECT_TYPE_INSTANCE_EXT: return (uint64_t)stored_instance;
	case VK_DEBUG_REPORT_OBJECT_TYPE_PHYSICAL_DEVICE_EXT: return (uint64_t)selected_physical_device;
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT: return (uint64_t)index_to_VkDevice.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT: return (uint64_t)index_to_VkQueue.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT: return 0;
	case VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT: return (uint64_t)index_to_VkSemaphore.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_BUFFER_EXT: return (uint64_t)index_to_VkCommandBuffer.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FENCE_EXT: return (uint64_t)index_to_VkFence.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT: return (uint64_t)index_to_VkBuffer.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT: return (uint64_t)index_to_VkImage.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_EVENT_EXT: return (uint64_t)index_to_VkEvent.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUERY_POOL_EXT: return (uint64_t)index_to_VkQueryPool.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_VIEW_EXT: return (uint64_t)index_to_VkBufferView.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT: return (uint64_t)index_to_VkImageView.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT: return (uint64_t)index_to_VkShaderModule.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_CACHE_EXT: return (uint64_t)index_to_VkPipelineCache.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT: return (uint64_t)index_to_VkPipelineLayout.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT: return (uint64_t)index_to_VkRenderPass.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT: return (uint64_t)index_to_VkPipeline.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT_EXT: return (uint64_t)index_to_VkDescriptorSetLayout.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_EXT: return (uint64_t)index_to_VkSampler.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_POOL_EXT: return (uint64_t)index_to_VkDescriptorPool.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT: return (uint64_t)index_to_VkDescriptorSet.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT: return (uint64_t)index_to_VkFramebuffer.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_POOL_EXT: return (uint64_t)index_to_VkCommandPool.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SURFACE_KHR_EXT: return (uint64_t)index_to_VkSurfaceKHR.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SWAPCHAIN_KHR_EXT: return (uint64_t)index_to_VkSwapchainKHR.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_EXT: return (uint64_t)index_to_VkDescriptorUpdateTemplate.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_KHR_EXT: return (uint64_t)index_to_VkDisplayKHR.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_MODE_KHR_EXT: return (uint64_t)index_to_VkDisplayModeKHR.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR_EXT: return (uint64_t)index_to_VkAccelerationStructureKHR.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_VALIDATION_CACHE_EXT: return (uint64_t)index_to_VkValidationCacheEXT.at(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR_EXT: return (uint64_t)index_to_VkSamplerYcbcrConversion.at(index);
	// these are not supported:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_MODULE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_FUNCTION_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_MODULE_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_FUNCTION_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_MAX_ENUM_EXT: assert(false); return 0;
	}
	return 0;
}

uint64_t debug_object_lookup_output(VkDebugReportObjectTypeEXT type, uint32_t index)
{
	switch (type)
	{
	case VK_DEBUG_REPORT_OBJECT_TYPE_INSTANCE_EXT: return (uint64_t)fake_handle<VkInstance>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PHYSICAL_DEVICE_EXT: return (uint64_t)fake_handle<VkPhysicalDevice>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT: return (uint64_t)fake_handle<VkDevice>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT: return queue_lookup_fake_handle(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT: return (uint64_t)fake_handle<VkDeviceMemory>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT: return (uint64_t)fake_handle<VkSemaphore>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_BUFFER_EXT: return (uint64_t)fake_handle<VkCommandBuffer>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FENCE_EXT: return (uint64_t)fake_handle<VkFence>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT: return (uint64_t)fake_handle<VkBuffer>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT: return (uint64_t)fake_handle<VkImage>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_EVENT_EXT: return (uint64_t)fake_handle<VkEvent>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUERY_POOL_EXT: return (uint64_t)fake_handle<VkQueryPool>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_VIEW_EXT: return (uint64_t)fake_handle<VkBufferView>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT: return (uint64_t)fake_handle<VkImageView>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT: return (uint64_t)fake_handle<VkShaderModule>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_CACHE_EXT: return (uint64_t)fake_handle<VkPipelineCache>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT: return (uint64_t)fake_handle<VkPipelineLayout>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT: return (uint64_t)fake_handle<VkRenderPass>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT: return (uint64_t)fake_handle<VkPipeline>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT_EXT: return (uint64_t)fake_handle<VkDescriptorSetLayout>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_EXT: return (uint64_t)fake_handle<VkSampler>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_POOL_EXT: return (uint64_t)fake_handle<VkDescriptorPool>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT: return (uint64_t)fake_handle<VkDescriptorSet>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT: return (uint64_t)fake_handle<VkFramebuffer>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_POOL_EXT: return (uint64_t)fake_handle<VkCommandPool>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SURFACE_KHR_EXT: return (uint64_t)fake_handle<VkSurfaceKHR>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SWAPCHAIN_KHR_EXT: return (uint64_t)fake_handle<VkSwapchainKHR>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_EXT: return (uint64_t)fake_handle<VkDescriptorUpdateTemplate>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_KHR_EXT: return (uint64_t)fake_handle<VkDisplayKHR>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_MODE_KHR_EXT: return (uint64_t)fake_handle<VkDisplayModeKHR>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR_EXT: return (uint64_t)fake_handle<VkAccelerationStructureKHR>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_VALIDATION_CACHE_EXT: return (uint64_t)fake_handle<VkValidationCacheEXT>(index);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR_EXT: return (uint64_t)fake_handle<VkSamplerYcbcrConversion>(index);
	// these are not supported:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_MODULE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_FUNCTION_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_MODULE_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_FUNCTION_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_MAX_ENUM_EXT: assert(false); return 0;
	}
	return 0;
}

uint64_t object_lookup(VkObjectType type, uint32_t index)
{
	switch (type)
	{
	case VK_OBJECT_TYPE_INSTANCE: return (uint64_t)stored_instance;
	case VK_OBJECT_TYPE_PHYSICAL_DEVICE: return (uint64_t)selected_physical_device;
	case VK_OBJECT_TYPE_DEVICE: return (uint64_t)index_to_VkDevice.at(index);
	case VK_OBJECT_TYPE_QUEUE: return (uint64_t)index_to_VkQueue.at(index);
	case VK_OBJECT_TYPE_DEVICE_MEMORY: return 0;
	case VK_OBJECT_TYPE_SEMAPHORE: return (uint64_t)index_to_VkSemaphore.at(index);
	case VK_OBJECT_TYPE_COMMAND_BUFFER: return (uint64_t)index_to_VkCommandBuffer.at(index);
	case VK_OBJECT_TYPE_FENCE: return (uint64_t)index_to_VkFence.at(index);
	case VK_OBJECT_TYPE_BUFFER: return (uint64_t)index_to_VkBuffer.at(index);
	case VK_OBJECT_TYPE_IMAGE: return (uint64_t)index_to_VkImage.at(index);
	case VK_OBJECT_TYPE_EVENT: return (uint64_t)index_to_VkEvent.at(index);
	case VK_OBJECT_TYPE_QUERY_POOL: return (uint64_t)index_to_VkQueryPool.at(index);
	case VK_OBJECT_TYPE_BUFFER_VIEW: return (uint64_t)index_to_VkBufferView.at(index);
	case VK_OBJECT_TYPE_IMAGE_VIEW: return (uint64_t)index_to_VkImageView.at(index);
	case VK_OBJECT_TYPE_SHADER_MODULE: return (uint64_t)index_to_VkShaderModule.at(index);
	case VK_OBJECT_TYPE_PIPELINE_CACHE: return (uint64_t)index_to_VkPipelineCache.at(index);
	case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return (uint64_t)index_to_VkPipelineLayout.at(index);
	case VK_OBJECT_TYPE_RENDER_PASS: return (uint64_t)index_to_VkRenderPass.at(index);
	case VK_OBJECT_TYPE_PIPELINE: return (uint64_t)index_to_VkPipeline.at(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return (uint64_t)index_to_VkDescriptorSetLayout.at(index);
	case VK_OBJECT_TYPE_SAMPLER: return (uint64_t)index_to_VkSampler.at(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return (uint64_t)index_to_VkDescriptorPool.at(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET: return (uint64_t)index_to_VkDescriptorSet.at(index);
	case VK_OBJECT_TYPE_FRAMEBUFFER: return (uint64_t)index_to_VkFramebuffer.at(index);
	case VK_OBJECT_TYPE_COMMAND_POOL: return (uint64_t)index_to_VkCommandPool.at(index);
	case VK_OBJECT_TYPE_SURFACE_KHR: return (uint64_t)index_to_VkSurfaceKHR.at(index);
	case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return (uint64_t)index_to_VkSwapchainKHR.at(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return (uint64_t)index_to_VkDescriptorUpdateTemplate.at(index);
	case VK_OBJECT_TYPE_DISPLAY_KHR: return (uint64_t)index_to_VkDisplayKHR.at(index);
	case VK_OBJECT_TYPE_DISPLAY_MODE_KHR: return (uint64_t)index_to_VkDisplayModeKHR.at(index);
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return (uint64_t)index_to_VkAccelerationStructureKHR.at(index);
	case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT: return (uint64_t)index_to_VkValidationCacheEXT.at(index);
	case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR: return (uint64_t)index_to_VkDeferredOperationKHR.at(index);
	case VK_OBJECT_TYPE_MICROMAP_EXT: return (uint64_t)index_to_VkMicromapEXT.at(index);
	case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT: return (uint64_t)index_to_VkPrivateDataSlot.at(index);
	case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR: return (uint64_t)index_to_VkSamplerYcbcrConversion.at(index);
	case VK_OBJECT_TYPE_VIDEO_SESSION_KHR: return (uint64_t)index_to_VkVideoSessionKHR.at(index);
	case VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR: return (uint64_t)index_to_VkVideoSessionParametersKHR.at(index);
	case VK_OBJECT_TYPE_SHADER_EXT: return (uint64_t)index_to_VkShaderEXT.at(index);
	case VK_OBJECT_TYPE_SHADER_INSTRUMENTATION_ARM: return (uint64_t)index_to_VkShaderInstrumentationARM.at(index);
	case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT: return (uint64_t)index_to_VkDebugReportCallbackEXT.at(index);
	case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT: return (uint64_t)index_to_VkDebugUtilsMessengerEXT.at(index);
	case VK_OBJECT_TYPE_TENSOR_ARM: return (uint64_t)index_to_VkTensorARM.at(index);
	case VK_OBJECT_TYPE_TENSOR_VIEW_ARM: return (uint64_t)index_to_VkTensorViewARM.at(index);
	case VK_OBJECT_TYPE_DATA_GRAPH_PIPELINE_SESSION_ARM: return (uint64_t)index_to_VkDataGraphPipelineSessionARM.at(index);

	// these are not supported:
	case VK_OBJECT_TYPE_GPA_SESSION_AMD:
	case VK_OBJECT_TYPE_EXTERNAL_COMPUTE_QUEUE_NV:
	case VK_OBJECT_TYPE_OPTICAL_FLOW_SESSION_NV:
	case VK_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV:
	case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL:
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
	case VK_OBJECT_TYPE_CU_FUNCTION_NVX:
	case VK_OBJECT_TYPE_CU_MODULE_NVX:
	case VK_OBJECT_TYPE_PIPELINE_BINARY_KHR:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT:
	case VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT:
	case VK_OBJECT_TYPE_UNKNOWN:
	case VK_OBJECT_TYPE_MAX_ENUM: assert(false); return 0;
	}
	return 0;
}

uint64_t object_lookup_output(VkObjectType type, uint32_t index)
{
	switch (type)
	{
	case VK_OBJECT_TYPE_INSTANCE: return (uint64_t)fake_handle<VkInstance>(index);
	case VK_OBJECT_TYPE_PHYSICAL_DEVICE: return (uint64_t)fake_handle<VkPhysicalDevice>(index);
	case VK_OBJECT_TYPE_DEVICE: return (uint64_t)fake_handle<VkDevice>(index);
	case VK_OBJECT_TYPE_QUEUE: return queue_lookup_fake_handle(index);
	case VK_OBJECT_TYPE_DEVICE_MEMORY: return (uint64_t)fake_handle<VkDeviceMemory>(index);
	case VK_OBJECT_TYPE_SEMAPHORE: return (uint64_t)fake_handle<VkSemaphore>(index);
	case VK_OBJECT_TYPE_COMMAND_BUFFER: return (uint64_t)fake_handle<VkCommandBuffer>(index);
	case VK_OBJECT_TYPE_FENCE: return (uint64_t)fake_handle<VkFence>(index);
	case VK_OBJECT_TYPE_BUFFER: return (uint64_t)fake_handle<VkBuffer>(index);
	case VK_OBJECT_TYPE_IMAGE: return (uint64_t)fake_handle<VkImage>(index);
	case VK_OBJECT_TYPE_EVENT: return (uint64_t)fake_handle<VkEvent>(index);
	case VK_OBJECT_TYPE_QUERY_POOL: return (uint64_t)fake_handle<VkQueryPool>(index);
	case VK_OBJECT_TYPE_BUFFER_VIEW: return (uint64_t)fake_handle<VkBufferView>(index);
	case VK_OBJECT_TYPE_IMAGE_VIEW: return (uint64_t)fake_handle<VkImageView>(index);
	case VK_OBJECT_TYPE_SHADER_MODULE: return (uint64_t)fake_handle<VkShaderModule>(index);
	case VK_OBJECT_TYPE_PIPELINE_CACHE: return (uint64_t)fake_handle<VkPipelineCache>(index);
	case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return (uint64_t)fake_handle<VkPipelineLayout>(index);
	case VK_OBJECT_TYPE_RENDER_PASS: return (uint64_t)fake_handle<VkRenderPass>(index);
	case VK_OBJECT_TYPE_PIPELINE: return (uint64_t)fake_handle<VkPipeline>(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return (uint64_t)fake_handle<VkDescriptorSetLayout>(index);
	case VK_OBJECT_TYPE_SAMPLER: return (uint64_t)fake_handle<VkSampler>(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return (uint64_t)fake_handle<VkDescriptorPool>(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET: return (uint64_t)fake_handle<VkDescriptorSet>(index);
	case VK_OBJECT_TYPE_FRAMEBUFFER: return (uint64_t)fake_handle<VkFramebuffer>(index);
	case VK_OBJECT_TYPE_COMMAND_POOL: return (uint64_t)fake_handle<VkCommandPool>(index);
	case VK_OBJECT_TYPE_SURFACE_KHR: return (uint64_t)fake_handle<VkSurfaceKHR>(index);
	case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return (uint64_t)fake_handle<VkSwapchainKHR>(index);
	case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return (uint64_t)fake_handle<VkDescriptorUpdateTemplate>(index);
	case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT: return (uint64_t)fake_handle<VkPrivateDataSlot>(index);
	case VK_OBJECT_TYPE_DISPLAY_KHR: return (uint64_t)fake_handle<VkDisplayKHR>(index);
	case VK_OBJECT_TYPE_DISPLAY_MODE_KHR: return (uint64_t)fake_handle<VkDisplayModeKHR>(index);
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return (uint64_t)fake_handle<VkAccelerationStructureKHR>(index);
	case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT: return (uint64_t)fake_handle<VkValidationCacheEXT>(index);
	case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR: return (uint64_t)fake_handle<VkDeferredOperationKHR>(index);
	case VK_OBJECT_TYPE_MICROMAP_EXT: return (uint64_t)fake_handle<VkMicromapEXT>(index);
	case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR: return (uint64_t)fake_handle<VkSamplerYcbcrConversion>(index);
	case VK_OBJECT_TYPE_VIDEO_SESSION_KHR: return (uint64_t)fake_handle<VkVideoSessionKHR>(index);
	case VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR: return (uint64_t)fake_handle<VkVideoSessionParametersKHR>(index);
	case VK_OBJECT_TYPE_SHADER_EXT: return (uint64_t)fake_handle<VkShaderEXT>(index);
	case VK_OBJECT_TYPE_SHADER_INSTRUMENTATION_ARM: return (uint64_t)fake_handle<VkShaderInstrumentationARM>(index);
	case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT: return (uint64_t)fake_handle<VkDebugReportCallbackEXT>(index);
	case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT: return (uint64_t)fake_handle<VkDebugUtilsMessengerEXT>(index);
	case VK_OBJECT_TYPE_TENSOR_ARM: return (uint64_t)fake_handle<VkTensorARM>(index);
	case VK_OBJECT_TYPE_TENSOR_VIEW_ARM: return (uint64_t)fake_handle<VkTensorViewARM>(index);
	case VK_OBJECT_TYPE_DATA_GRAPH_PIPELINE_SESSION_ARM: return (uint64_t)fake_handle<VkDataGraphPipelineSessionARM>(index);

	// these are not supported:
	case VK_OBJECT_TYPE_GPA_SESSION_AMD:
	case VK_OBJECT_TYPE_EXTERNAL_COMPUTE_QUEUE_NV:
	case VK_OBJECT_TYPE_OPTICAL_FLOW_SESSION_NV:
	case VK_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV:
	case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL:
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
	case VK_OBJECT_TYPE_CU_FUNCTION_NVX:
	case VK_OBJECT_TYPE_CU_MODULE_NVX:
	case VK_OBJECT_TYPE_PIPELINE_BINARY_KHR:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT:
	case VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT:
	case VK_OBJECT_TYPE_UNKNOWN:
	case VK_OBJECT_TYPE_MAX_ENUM: assert(false); return 0;
	}
	return 0;
}

trackable& object_trackable(VkObjectType type, uint64_t handle)
{
	static trackable dummy;
	switch (type)
	{
	case VK_OBJECT_TYPE_INSTANCE: return VkInstance_index.at(index_to_VkInstance.index(stored_instance));
	case VK_OBJECT_TYPE_PHYSICAL_DEVICE: return VkPhysicalDevice_index.at(index_to_VkPhysicalDevice.index(selected_physical_device));
	case VK_OBJECT_TYPE_DEVICE: return VkDevice_index.at(index_to_VkDevice.index((VkDevice)handle));
	case VK_OBJECT_TYPE_QUEUE: return VkQueue_index.at(index_to_VkQueue.index((VkQueue)handle));
	case VK_OBJECT_TYPE_DEVICE_MEMORY: assert(false); return dummy;
	case VK_OBJECT_TYPE_SEMAPHORE: return VkSemaphore_index.at(index_to_VkSemaphore.index((VkSemaphore)handle));
	case VK_OBJECT_TYPE_COMMAND_BUFFER: return VkCommandBuffer_index.at(index_to_VkCommandBuffer.index((VkCommandBuffer)handle));
	case VK_OBJECT_TYPE_FENCE: return VkFence_index.at(index_to_VkFence.index((VkFence)handle));
	case VK_OBJECT_TYPE_BUFFER: return VkBuffer_index.at(index_to_VkBuffer.index((VkBuffer)handle));
	case VK_OBJECT_TYPE_IMAGE: return VkImage_index.at(index_to_VkImage.index((VkImage)handle));
	case VK_OBJECT_TYPE_EVENT: return VkEvent_index.at(index_to_VkEvent.index((VkEvent)handle));
	case VK_OBJECT_TYPE_QUERY_POOL: return VkQueryPool_index.at(index_to_VkQueryPool.index((VkQueryPool)handle));
	case VK_OBJECT_TYPE_BUFFER_VIEW: return VkBufferView_index.at(index_to_VkBufferView.index((VkBufferView)handle));
	case VK_OBJECT_TYPE_IMAGE_VIEW: return VkImageView_index.at(index_to_VkImageView.index((VkImageView)handle));
	case VK_OBJECT_TYPE_SHADER_MODULE: return VkShaderModule_index.at(index_to_VkShaderModule.index((VkShaderModule)handle));
	case VK_OBJECT_TYPE_PIPELINE_CACHE: return VkPipelineCache_index.at(index_to_VkPipelineCache.index((VkPipelineCache)handle));
	case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return VkPipelineLayout_index.at(index_to_VkPipelineLayout.index((VkPipelineLayout)handle));
	case VK_OBJECT_TYPE_RENDER_PASS: return VkRenderPass_index.at(index_to_VkRenderPass.index((VkRenderPass)handle));
	case VK_OBJECT_TYPE_PIPELINE: return VkPipeline_index.at(index_to_VkPipeline.index((VkPipeline)handle));
	case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return VkDescriptorSetLayout_index.at(index_to_VkDescriptorSetLayout.index((VkDescriptorSetLayout)handle));
	case VK_OBJECT_TYPE_SAMPLER: return VkSampler_index.at(index_to_VkSampler.index((VkSampler)handle));
	case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return VkDescriptorPool_index.at(index_to_VkDescriptorPool.index((VkDescriptorPool)handle));
	case VK_OBJECT_TYPE_DESCRIPTOR_SET: return VkDescriptorSet_index.at(index_to_VkDescriptorSet.index((VkDescriptorSet)handle));
	case VK_OBJECT_TYPE_FRAMEBUFFER: return VkFramebuffer_index.at(index_to_VkFramebuffer.index((VkFramebuffer)handle));
	case VK_OBJECT_TYPE_COMMAND_POOL: return VkCommandPool_index.at(index_to_VkCommandPool.index((VkCommandPool)handle));
	case VK_OBJECT_TYPE_SURFACE_KHR: return VkSurfaceKHR_index.at(index_to_VkSurfaceKHR.index((VkSurfaceKHR)handle));
	case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return VkSwapchainKHR_index.at(index_to_VkSwapchainKHR.index((VkSwapchainKHR)handle));
	case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return VkDescriptorUpdateTemplate_index.at(index_to_VkDescriptorUpdateTemplate.index((VkDescriptorUpdateTemplate)handle));
	case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT: return VkPrivateDataSlot_index.at(index_to_VkPrivateDataSlot.index((VkPrivateDataSlot)handle));
	case VK_OBJECT_TYPE_DISPLAY_KHR: return VkDisplayKHR_index.at(index_to_VkDisplayKHR.index((VkDisplayKHR)handle));
	case VK_OBJECT_TYPE_DISPLAY_MODE_KHR: return VkDisplayModeKHR_index.at(index_to_VkDisplayModeKHR.index((VkDisplayModeKHR)handle));
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return VkAccelerationStructureKHR_index.at(index_to_VkAccelerationStructureKHR.index((VkAccelerationStructureKHR)handle));
	case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT: return VkValidationCacheEXT_index.at(index_to_VkValidationCacheEXT.index((VkValidationCacheEXT)handle));
	case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR: return VkDeferredOperationKHR_index.at(index_to_VkDeferredOperationKHR.index((VkDeferredOperationKHR)handle));
	case VK_OBJECT_TYPE_MICROMAP_EXT: return VkMicromapEXT_index.at(index_to_VkMicromapEXT.index((VkMicromapEXT)handle));
	case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR: return VkSamplerYcbcrConversion_index.at(index_to_VkSamplerYcbcrConversion.index((VkSamplerYcbcrConversion)handle));
	case VK_OBJECT_TYPE_VIDEO_SESSION_KHR: return VkVideoSessionKHR_index.at(index_to_VkVideoSessionKHR.index((VkVideoSessionKHR)handle));
	case VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR: return VkVideoSessionParametersKHR_index.at(index_to_VkVideoSessionParametersKHR.index((VkVideoSessionParametersKHR)handle));
	case VK_OBJECT_TYPE_SHADER_EXT: return VkShaderEXT_index.at(index_to_VkShaderEXT.index((VkShaderEXT)handle));
	case VK_OBJECT_TYPE_SHADER_INSTRUMENTATION_ARM: return VkShaderInstrumentationARM_index.at(index_to_VkShaderInstrumentationARM.index((VkShaderInstrumentationARM)handle));
	case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT: return VkDebugReportCallbackEXT_index.at(index_to_VkDebugReportCallbackEXT.index((VkDebugReportCallbackEXT)handle));
	case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT: return VkDebugUtilsMessengerEXT_index.at(index_to_VkDebugUtilsMessengerEXT.index((VkDebugUtilsMessengerEXT)handle));
	case VK_OBJECT_TYPE_TENSOR_ARM: return VkTensorARM_index.at(index_to_VkTensorARM.index((VkTensorARM)handle));
	case VK_OBJECT_TYPE_TENSOR_VIEW_ARM: return VkTensorViewARM_index.at(index_to_VkTensorViewARM.index((VkTensorViewARM)handle));
	case VK_OBJECT_TYPE_DATA_GRAPH_PIPELINE_SESSION_ARM: return VkDataGraphPipelineSessionARM_index.at(index_to_VkDataGraphPipelineSessionARM.index((VkDataGraphPipelineSessionARM)handle));

	// these are not supported:
	case VK_OBJECT_TYPE_GPA_SESSION_AMD:
	case VK_OBJECT_TYPE_EXTERNAL_COMPUTE_QUEUE_NV:
	case VK_OBJECT_TYPE_OPTICAL_FLOW_SESSION_NV:
	case VK_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV:
	case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL:
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
	case VK_OBJECT_TYPE_CU_FUNCTION_NVX:
	case VK_OBJECT_TYPE_CU_MODULE_NVX:
	case VK_OBJECT_TYPE_PIPELINE_BINARY_KHR:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT:
	case VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT:
	case VK_OBJECT_TYPE_UNKNOWN:
	case VK_OBJECT_TYPE_MAX_ENUM: assert(false); return dummy;
	}
	assert(false);
	return dummy;
}

uint64_t queue_lookup_fake_handle(uint32_t index)
{
	const trackedqueue& queue_data = VkQueue_index.at(index);
	assert(queue_data.queueFamily != UINT32_MAX);
	assert(queue_data.queueIndex != UINT32_MAX);
	return (uint64_t)fake_handle<VkQueue>((queue_data.queueFamily << 16) + queue_data.queueIndex);
}

VkQueue queue_lookup_output_handle(uint32_t index, VkQueue fallback)
{
	const trackedqueue& queue_data = VkQueue_index.at(index);
	if (queue_data.queueFamily == UINT32_MAX || queue_data.queueIndex == UINT32_MAX) return fallback;
	return fake_handle<VkQueue>((queue_data.queueFamily << 16) + queue_data.queueIndex);
}

void memory_report_callback(const VkDeviceMemoryReportCallbackDataEXT* pCallbackData, void* pUserData)
{
	// TBD
}

VkBool32 VKAPI_PTR messenger_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT           messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT                  messageTypes,
	const VkDebugUtilsMessengerCallbackDataEXT*      pCallbackData,
	void*                                            pUserData)
{
	if (!is_debug() && (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT || messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)) return VK_TRUE;
	ILOG("messenger (s%d, t%d): %s", (int)messageSeverity, (int)messageTypes, pCallbackData->pMessage);
	return VK_TRUE;
}

VkBool32 VKAPI_PTR debug_report_callback(
	VkDebugReportFlagsEXT                       flags,
	VkDebugReportObjectTypeEXT                  objectType,
	uint64_t                                    object,
	size_t                                      location,
	int32_t                                     messageCode,
	const char*                                 pLayerPrefix,
	const char*                                 pMessage,
	void*                                       pUserData)
{
	if (((flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) || (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT)) && !is_debug()) return VK_TRUE;
	ILOG("%s (%d): %s", pLayerPrefix, messageCode, pMessage);
	return VK_TRUE;
}

// for completeness - but this part of the API is never used
const char* const* device_layers(lava_file_reader& reader, uint32_t& len)
{
	const char* const* retval = reader.read_string_array(len);
	return retval;
}

const char* const* instance_layers(lava_file_reader& reader, uint32_t& len)
{
	static std::vector<const char *> dst;
	static std::vector<std::string> backing;
	const char* const* retval = reader.read_string_array(len);
	if (!reader.run) return retval;

	backing.clear();
	dst.clear();

	// Add validation layers, if requested
	uint32_t propertyCount = 0;
	VkResult result = wrap_vkEnumerateInstanceLayerProperties(&propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	std::vector<VkLayerProperties> supported_layers(propertyCount);
	result = wrap_vkEnumerateInstanceLayerProperties(&propertyCount, supported_layers.data());
	assert(result == VK_SUCCESS);
	DLOG("Supported instance layers on replay host:");
	for (const VkLayerProperties& s : supported_layers)
	{
		DLOG("\t%s - %s", s.layerName, s.description);
		if (is_validation() && strcmp(s.layerName, "VK_LAYER_KHRONOS_validation") == 0)
		{
			backing.push_back(s.layerName);
			ILOG("Enabling validation layer");
		}
	}

	len = backing.size();

	// Resize everything else to match
	dst.resize(len);
	for (uint32_t i = 0; i < backing.size(); i++)
	{
		dst[i] = backing[i].data();
	}

	ILOG("Enabling %u layers:", len);
	for (auto l_name : backing)
	{
		ILOG("\t %s", l_name.c_str());
	}

	return dst.data();
}

const char* const* device_extensions(VkDeviceCreateInfo* sptr, lava_file_reader& reader, uint32_t& len)
{
	bool trace_has_frame_boundary = false;
	bool trace_has_swapchain = false;
	bool host_has_pipeline_executable_properties = false;
	bool host_has_pipeline_executable_info = false;
	bool host_has_memory_budget = false;
	bool host_has_shader_instrumentation = false;
	bool host_has_maintenance5 = false;
	static std::vector<const char *> dst;
	static std::vector<std::string> backing;
	const char* const* stored = reader.read_string_array(len); // all extensions used in original
	const uint32_t stored_len = len;
	const uint32_t metadata_len = reader.parent->stored_device_requested_extensions.size();
	const bool use_stored_metadata = reader.run && !p__skip_remove_unused && reader.parent->has_stored_device_requested_extensions;
	reader.parent->cli_pipeline_executable_stats_enabled.store(false, std::memory_order_release);
	reader.parent->cli_memory_budget_enabled.store(false, std::memory_order_release);
	reader.parent->cli_shader_instrumentation_enabled.store(false, std::memory_order_release);
	if (!reader.run) return stored;
	const std::vector<const char*> do_not_copy = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME,
		VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
		VK_TRACETOOLTEST_OBJECT_PROPERTY_EXTENSION_NAME, VK_EXT_TOOLING_INFO_EXTENSION_NAME,
		VK_ARM_TRACE_HELPERS_EXTENSION_NAME, VK_ARM_EXPLICIT_HOST_UPDATES_EXTENSION_NAME,
		VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, "VK_ANDROID_external_memory_android_hardware_buffer",
		VK_ANDROID_FRAME_BOUNDARY_EXTENSION_NAME
	};

	dst.clear();
	backing.clear();
	if (use_stored_metadata && stored_len != metadata_len)
	{
		DLOG("Replacing packet device extension list (%u entries) with metadata list (%u entries)", stored_len, metadata_len);
	}

	// Copy over all except platform-specific extensions and potential duplicates
	for (uint32_t i = 0; i < (use_stored_metadata ? metadata_len : stored_len); i++)
	{
		const char* ext_name = use_stored_metadata ? reader.parent->stored_device_requested_extensions[i].c_str() : stored[i];
		bool nocopy = false;
		for (unsigned j = 0; j < do_not_copy.size(); j++)
		{
			if (strcmp(ext_name, do_not_copy[j]) == 0)
			{
				nocopy = true;
				break;
			}
		}

		if (strcmp(ext_name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
		{
			trace_has_swapchain = true;
			nocopy = true; // add it later
		}

		if (strcmp(ext_name, VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME) == 0 || strcmp(ext_name, VK_ANDROID_FRAME_BOUNDARY_EXTENSION_NAME) == 0)
		{
			trace_has_frame_boundary = true;
			nocopy = true; // add it later
		}

		// Sanity check
		if (is_noscreen() && strcmp(ext_name, "VK_KHR_display_swapchain") == 0)
		{
			ABORT("Cannot use VK_KHR_display_swapchain with none wsi yet");
		}

		if (!nocopy)
		{
			backing.push_back(ext_name);
		}
	}

	// Find supported extensions
	uint32_t propertyCount = 0;
	VkResult result = wrap_vkEnumerateDeviceExtensionProperties(selected_physical_device, nullptr, &propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = wrap_vkEnumerateDeviceExtensionProperties(selected_physical_device, nullptr, &propertyCount, supported_extensions.data());
	assert(result == VK_SUCCESS);
	bool has_swapchain = false;
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) has_swapchain = true;
		if (strcmp(s.extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0) has_pipeline_feedback = true;
		if (strcmp(s.extensionName, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) == 0) has_pipeline_control = true;
		if (strcmp(s.extensionName, VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME) == 0) host_has_frame_boundary = true;
		if (strcmp(s.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0) host_has_pipeline_executable_properties = true;
		if (strcmp(s.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) host_has_memory_budget = true;
		if (strcmp(s.extensionName, VK_ARM_SHADER_INSTRUMENTATION_EXTENSION_NAME) == 0) host_has_shader_instrumentation = true;
		if (strcmp(s.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME) == 0) host_has_maintenance5 = true;
	}
	if (trace_has_swapchain && !has_swapchain) ABORT("No swapchain extension found - cannot proceed!");

	if (host_has_pipeline_executable_properties)
	{
		VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipeline_executable_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
			nullptr,
			VK_FALSE
		};
		VkPhysicalDeviceFeatures2 features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			&pipeline_executable_features
		};
		wrap_vkGetPhysicalDeviceFeatures2(selected_physical_device, &features);
		host_has_pipeline_executable_info = pipeline_executable_features.pipelineExecutableInfo == VK_TRUE;
	}

	if (!host_has_frame_boundary && trace_has_frame_boundary)
	{
		ILOG("Replay host does not have frame boundary but trace does -- removing it from the replay!");
		purge_extension_parent(sptr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT);
	}
	else if (trace_has_frame_boundary)
	{
		backing.push_back(VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME);
	}

	// Add device extensions
	if (trace_has_swapchain)
	{
		backing.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}

	if (has_pipeline_feedback)
	{
		backing.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
		DLOG("Enabling pipeline creation feedback extension");
	}

	if (has_pipeline_control)
	{
		backing.push_back(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
		DLOG("Enabling pipeline creation cache control extension");
	}

	bool has_pipeline_executable_properties = false;
	bool has_memory_budget = false;
	for (const std::string& extension_name : backing)
	{
		if (extension_name == VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)
		{
			has_pipeline_executable_properties = true;
		}
		if (extension_name == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)
		{
			has_memory_budget = true;
		}
	}

	if (reader.parent->cli_pipeline_executable_stats_requested)
	{
		if (host_has_pipeline_executable_properties && host_has_pipeline_executable_info)
		{
			if (!has_pipeline_executable_properties)
			{
				backing.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
				has_pipeline_executable_properties = true;
			}
			DLOG("Enabling pipeline executable properties extension for lava-cli");
		}
		else
		{
			DLOG("Pipeline executable properties extension is not supported for lava-cli");
		}
	}

	reader.parent->cli_pipeline_executable_stats_enabled.store(
		has_pipeline_executable_properties && host_has_pipeline_executable_properties && host_has_pipeline_executable_info,
		std::memory_order_release);

	if (reader.parent->cli_memory_budget_requested)
	{
		if (host_has_memory_budget)
		{
			if (!has_memory_budget)
			{
				backing.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
				has_memory_budget = true;
			}
			DLOG("Enabling memory budget extension for lava-cli");
		}
		else
		{
			DLOG("Memory budget extension is not supported for lava-cli");
		}
	}

	reader.parent->cli_memory_budget_enabled.store(has_memory_budget && host_has_memory_budget, std::memory_order_release);

	bool has_shader_instrumentation = false;
	bool has_maintenance5 = false;
	for (const std::string& extension_name : backing)
	{
		if (extension_name == VK_ARM_SHADER_INSTRUMENTATION_EXTENSION_NAME) has_shader_instrumentation = true;
		if (extension_name == VK_KHR_MAINTENANCE_5_EXTENSION_NAME) has_maintenance5 = true;
	}
	if (reader.parent->cli_shader_instrumentation_requested && host_has_shader_instrumentation && host_has_maintenance5)
	{
		VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
			nullptr,
			VK_FALSE
		};
		VkPhysicalDeviceShaderInstrumentationFeaturesARM instrumentation_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INSTRUMENTATION_FEATURES_ARM,
			&maintenance5_features,
			VK_FALSE
		};
		VkPhysicalDeviceFeatures2 features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			&instrumentation_features
		};
		wrap_vkGetPhysicalDeviceFeatures2(selected_physical_device, &features);
		if (instrumentation_features.shaderInstrumentation == VK_TRUE && maintenance5_features.maintenance5 == VK_TRUE)
		{
			if (!has_shader_instrumentation) backing.push_back(VK_ARM_SHADER_INSTRUMENTATION_EXTENSION_NAME);
			if (!has_maintenance5) backing.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
			has_shader_instrumentation = true;
			has_maintenance5 = true;
			DLOG("Enabling shader instrumentation extension for lava-cli");
		}
		else
		{
			DLOG("Shader instrumentation feature is not supported for lava-cli");
		}
	}
	else if (reader.parent->cli_shader_instrumentation_requested)
	{
		DLOG("Shader instrumentation extension is not supported for lava-cli");
	}
	reader.parent->cli_shader_instrumentation_enabled.store(
		reader.parent->cli_shader_instrumentation_requested && has_shader_instrumentation && has_maintenance5
		&& host_has_shader_instrumentation && host_has_maintenance5,
		std::memory_order_release);

	dst.resize(backing.size());
	for (uint32_t i = 0; i < backing.size(); i++)
	{
		dst[i] = backing[i].data();
	}
	len = backing.size();

	DLOG("Enabling %u device extensions:", len);
	for (auto ext_name : backing)
	{
		DLOG("\t %s", ext_name.c_str());
	}

	return dst.data();
}

const char* const* instance_extensions(lava_file_reader& reader, uint32_t& len)
{
	static std::vector<const char *> dst;
	static std::vector<std::string> backing;
	const std::vector<const char*> do_not_copy = {
		VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", VK_KHR_DISPLAY_EXTENSION_NAME,
		"VK_KHR_wayland_surface", "VK_KHR_mir_surface", "VK_KHR_android_surface", "VK_KHR_win32_surface",
		"VK_EXT_headless_surface"
	};
	const char* const* stored = reader.read_string_array(len);
	const uint32_t stored_len = len;
	const uint32_t metadata_len = reader.parent->stored_instance_requested_extensions.size();
	const bool use_stored_metadata = reader.run && !p__skip_remove_unused && reader.parent->has_stored_instance_requested_extensions;
	if (!reader.run) return stored;

	bool host_has_surface = false;
	backing.clear();
	dst.clear();
	if (use_stored_metadata && stored_len != metadata_len)
	{
		DLOG("Replacing packet instance extension list (%u entries) with metadata list (%u entries)", stored_len, metadata_len);
	}

	uint32_t propertyCount = 0;
	VkResult result = wrap_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = wrap_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, supported_extensions.data());
	assert(result == VK_SUCCESS);
	DLOG("Supported instance extensions on replay host:");
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) host_has_surface = true;
		if (strcmp(s.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) has_debug_report = true;
		if (strcmp(s.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) has_debug_utils = true;
		DLOG("\t%s", s.extensionName);
	}
	if (!has_debug_utils) DLOG("Replay host lacks %s; debug utils calls will be handled by lavatube", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (!has_debug_report && is_debug()) ELOG("Warning: Debug report extension missing - debug mode will not be fully operational!");
	if (!has_debug_report && is_validation()) ELOG("Warning: Debug report extension missing - validation layer will not be able to report anything!");

	// Copy over all except platform-specific extensions and potential duplicates
	for (uint32_t i = 0; i < (use_stored_metadata ? metadata_len : stored_len); i++)
	{
		const char* ext_name = use_stored_metadata ? reader.parent->stored_instance_requested_extensions[i].c_str() : stored[i];
		bool nocopy = false;
		for (unsigned j = 0; j < do_not_copy.size(); j++)
		{
			if (strcmp(ext_name, do_not_copy[j]) == 0)
			{
				nocopy = true;
				break;
			}
		}

		if (is_noscreen() && strcmp(ext_name, "VK_KHR_display") == 0)
		{
			ABORT("Cannot use VK_KHR_display with none wsi yet");
		}

		if (!has_debug_utils && strcmp(ext_name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
		{
			nocopy = true;
		}

		if (!nocopy)
		{
			backing.push_back(ext_name);
		}
	}

	// Add instance extensions
	if (!is_noscreen() || host_has_surface)
	{
		backing.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	}
	if (is_debug() || is_validation())
	{
		if (has_debug_report) backing.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
	}
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	if (!is_noscreen()) backing.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
	const char* winsys = window_winsys();
#ifdef VK_USE_PLATFORM_XCB_KHR
	if (strcmp(winsys, "xcb") == 0 && !is_noscreen())
	{
		backing.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
	}
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
	if (strcmp(winsys, "wayland") == 0 && !is_noscreen())
	{
		backing.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
	}
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
	if (strcmp(winsys, "x11") == 0 && !is_noscreen())
	{
		backing.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
	}
#endif
	if (strcmp(winsys, "headless") == 0 && !is_noscreen())
	{
		backing.push_back("VK_EXT_headless_surface");
	}
#endif

	dst.resize(backing.size());
	for (uint32_t i = 0; i < backing.size(); i++)
	{
		dst[i] = backing[i].data();
	}
	len = backing.size();

	DLOG("Enabling %u instance extensions:", len);
	for (auto ext_name : backing)
	{
		DLOG("\t %s", ext_name.c_str());
	}

	return dst.data();
}
