#include "helpers_write.h"

trackable* debug_object_trackable(trace_records& r, VkDebugReportObjectTypeEXT type, uint64_t object)
{
	switch (type)
	{
	case VK_DEBUG_REPORT_OBJECT_TYPE_INSTANCE_EXT: return r.VkInstance_index.at((const VkInstance)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PHYSICAL_DEVICE_EXT:
		if (r.VkPhysicalDevice_index.contains((VkPhysicalDevice)object)) return r.VkPhysicalDevice_index.at((VkPhysicalDevice)object);
		else return nullptr;
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT: return r.VkDevice_index.at((VkDevice)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT: return (p__virtualqueues) ? (trackable*)object : r.VkQueue_index.at((const VkQueue)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT: return r.VkDeviceMemory_index.at((const VkDeviceMemory)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT: return r.VkSemaphore_index.at((const VkSemaphore)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_BUFFER_EXT: return r.VkCommandBuffer_index.at((const VkCommandBuffer)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FENCE_EXT: return r.VkFence_index.at((const VkFence)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT: return r.VkBuffer_index.at((const VkBuffer)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT: return r.VkImage_index.at((const VkImage)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_EVENT_EXT: return r.VkEvent_index.at((const VkEvent)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_QUERY_POOL_EXT: return r.VkQueryPool_index.at((const VkQueryPool)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_VIEW_EXT: return r.VkBufferView_index.at((const VkBufferView)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT: return r.VkImageView_index.at((const VkImageView)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT: return r.VkShaderModule_index.at((const VkShaderModule)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_CACHE_EXT: return r.VkPipelineCache_index.at((const VkPipelineCache)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT: return r.VkPipelineLayout_index.at((const VkPipelineLayout)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT: return r.VkRenderPass_index.at((const VkRenderPass)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT: return r.VkPipeline_index.at((const VkPipeline)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT_EXT: return r.VkDescriptorSetLayout_index.at((const VkDescriptorSetLayout)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_EXT: return r.VkSampler_index.at((const VkSampler)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_POOL_EXT: return r.VkDescriptorPool_index.at((const VkDescriptorPool)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT: return r.VkDescriptorSet_index.at((const VkDescriptorSet)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT: return r.VkFramebuffer_index.at((const VkFramebuffer)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_POOL_EXT: return r.VkCommandPool_index.at((const VkCommandPool)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SURFACE_KHR_EXT: return r.VkSurfaceKHR_index.at((const VkSurfaceKHR)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SWAPCHAIN_KHR_EXT: return r.VkSwapchainKHR_index.at((const VkSwapchainKHR)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_KHR_EXT: return r.VkDescriptorUpdateTemplate_index.at((const VkDescriptorUpdateTemplate)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_KHR_EXT: return r.VkDisplayKHR_index.at((const VkDisplayKHR)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_DISPLAY_MODE_KHR_EXT: return r.VkDisplayModeKHR_index.at((const VkDisplayModeKHR)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_VALIDATION_CACHE_EXT: return r.VkValidationCacheEXT_index.at((const VkValidationCacheEXT)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_EXT: return r.VkSamplerYcbcrConversion_index.at((const VkSamplerYcbcrConversion)object);
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR_EXT: return r.VkAccelerationStructureKHR_index.at((const VkAccelerationStructureKHR)object);
	// not supported:
	case VK_DEBUG_REPORT_OBJECT_TYPE_DEBUG_REPORT_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_FUNCTION_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CU_MODULE_NVX_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_MODULE_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_CUDA_FUNCTION_NV_EXT:
	case VK_DEBUG_REPORT_OBJECT_TYPE_MAX_ENUM_EXT: assert(false); return nullptr;
	}
	return nullptr;
}

trackable* object_trackable(const trace_records& r, VkObjectType type, uint64_t object)
{
	switch (type)
	{
	case VK_OBJECT_TYPE_INSTANCE: return r.VkInstance_index.at((VkInstance)object);
	case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
		if (r.VkPhysicalDevice_index.contains((VkPhysicalDevice)object)) return r.VkPhysicalDevice_index.at((VkPhysicalDevice)object);
		else return nullptr;
	case VK_OBJECT_TYPE_DEVICE: return r.VkDevice_index.at((VkDevice)object);
	case VK_OBJECT_TYPE_QUEUE: return (p__virtualqueues) ? (trackable*)object : r.VkQueue_index.at((const VkQueue)object);
	case VK_OBJECT_TYPE_DEVICE_MEMORY: return r.VkDeviceMemory_index.at((const VkDeviceMemory)object);
	case VK_OBJECT_TYPE_SEMAPHORE: return r.VkSemaphore_index.at((const VkSemaphore)object);
	case VK_OBJECT_TYPE_COMMAND_BUFFER: return r.VkCommandBuffer_index.at((const VkCommandBuffer)object);
	case VK_OBJECT_TYPE_FENCE: return r.VkFence_index.at((const VkFence)object);
	case VK_OBJECT_TYPE_BUFFER: return r.VkBuffer_index.at((const VkBuffer)object);
	case VK_OBJECT_TYPE_IMAGE: return r.VkImage_index.at((const VkImage)object);
	case VK_OBJECT_TYPE_EVENT: return r.VkEvent_index.at((const VkEvent)object);
	case VK_OBJECT_TYPE_QUERY_POOL: return r.VkQueryPool_index.at((const VkQueryPool)object);
	case VK_OBJECT_TYPE_BUFFER_VIEW: return r.VkBufferView_index.at((const VkBufferView)object);
	case VK_OBJECT_TYPE_IMAGE_VIEW: return r.VkImageView_index.at((const VkImageView)object);
	case VK_OBJECT_TYPE_SHADER_MODULE: return r.VkShaderModule_index.at((const VkShaderModule)object);
	case VK_OBJECT_TYPE_PIPELINE_CACHE: return r.VkPipelineCache_index.at((const VkPipelineCache)object);
	case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return r.VkPipelineLayout_index.at((const VkPipelineLayout)object);
	case VK_OBJECT_TYPE_RENDER_PASS: return r.VkRenderPass_index.at((const VkRenderPass)object);
	case VK_OBJECT_TYPE_PIPELINE: return r.VkPipeline_index.at((const VkPipeline)object);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return r.VkDescriptorSetLayout_index.at((const VkDescriptorSetLayout)object);
	case VK_OBJECT_TYPE_SAMPLER: return r.VkSampler_index.at((const VkSampler)object);
	case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return r.VkDescriptorPool_index.at((const VkDescriptorPool)object);
	case VK_OBJECT_TYPE_DESCRIPTOR_SET: return r.VkDescriptorSet_index.at((const VkDescriptorSet)object);
	case VK_OBJECT_TYPE_FRAMEBUFFER: return r.VkFramebuffer_index.at((const VkFramebuffer)object);
	case VK_OBJECT_TYPE_COMMAND_POOL: return r.VkCommandPool_index.at((const VkCommandPool)object);
	case VK_OBJECT_TYPE_SURFACE_KHR: return r.VkSurfaceKHR_index.at((const VkSurfaceKHR)object);
	case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return r.VkSwapchainKHR_index.at((const VkSwapchainKHR)object);
	case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return r.VkDescriptorUpdateTemplate_index.at((const VkDescriptorUpdateTemplate)object);
	case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT: return r.VkPrivateDataSlot_index.at((const VkPrivateDataSlot)object);
	case VK_OBJECT_TYPE_DISPLAY_KHR: return r.VkDisplayKHR_index.at((const VkDisplayKHR)object);
	case VK_OBJECT_TYPE_DISPLAY_MODE_KHR: return r.VkDisplayModeKHR_index.at((const VkDisplayModeKHR)object);
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return r.VkAccelerationStructureKHR_index.at((const VkAccelerationStructureKHR)object);
	case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT: return r.VkValidationCacheEXT_index.at((const VkValidationCacheEXT)object);
	case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR: return r.VkDeferredOperationKHR_index.at((const VkDeferredOperationKHR)object);
	case VK_OBJECT_TYPE_MICROMAP_EXT: return r.VkMicromapEXT_index.at((const VkMicromapEXT)object);
	case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR: return r.VkSamplerYcbcrConversion_index.at((const VkSamplerYcbcrConversion)object);
	case VK_OBJECT_TYPE_SHADER_EXT: return r.VkShaderEXT_index.at((const VkShaderEXT)object);
	case VK_OBJECT_TYPE_VIDEO_SESSION_KHR: return r.VkVideoSessionKHR_index.at((const VkVideoSessionKHR)object);
	case VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR: return r.VkVideoSessionParametersKHR_index.at((const VkVideoSessionParametersKHR)object);
	case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT: return r.VkDebugReportCallbackEXT_index.at((const VkDebugReportCallbackEXT)object);
	case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT: return r.VkDebugUtilsMessengerEXT_index.at((const VkDebugUtilsMessengerEXT)object);
	case VK_OBJECT_TYPE_TENSOR_VIEW_ARM: return r.VkTensorViewARM_index.at((const VkTensorViewARM)object);
	case VK_OBJECT_TYPE_TENSOR_ARM: return r.VkTensorARM_index.at((const VkTensorARM)object);
	case VK_OBJECT_TYPE_DATA_GRAPH_PIPELINE_SESSION_ARM: return r.VkDataGraphPipelineSessionARM_index.at((const VkDataGraphPipelineSessionARM)object);
	case VK_OBJECT_TYPE_SHADER_INSTRUMENTATION_ARM: return r.VkShaderInstrumentationARM_index.at((const VkShaderInstrumentationARM)object);
	// not supported:
	case VK_OBJECT_TYPE_GPA_SESSION_AMD:
	case VK_OBJECT_TYPE_EXTERNAL_COMPUTE_QUEUE_NV:
	case VK_OBJECT_TYPE_CU_MODULE_NVX:
	case VK_OBJECT_TYPE_CU_FUNCTION_NVX:
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
	case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV:
	case VK_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA:
	case VK_OBJECT_TYPE_OPTICAL_FLOW_SESSION_NV:
	case VK_OBJECT_TYPE_UNKNOWN:
	case VK_OBJECT_TYPE_PIPELINE_BINARY_KHR:
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT:
	case VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT:
	case VK_OBJECT_TYPE_MAX_ENUM: assert(false); return nullptr;
	}
	return nullptr;
}
