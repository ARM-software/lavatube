# This file contains Vulkan-specific code-generation information. It should not contain executed python code
# unless it is for validating the data contained within. The `spec` library must be initialized before including.

import spec

def validate_funcs(lst):
	assert len(lst) == len(set(lst))
	for x in lst: assert x in spec.valid_functions, '%s is not a valid function' % x

# Extra 'optional' variables to work around silly Vulkan API decisions
extra_optionals = {
	'VkWriteDescriptorSet': {
		'pImageInfo': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'pBufferInfo': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)',
		'pTexelBufferView': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)',
		'sampler': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)',
		'imageView': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'imageLayout': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'dstSet' : '!ignoreDstSet', # the actual condition is that the calling function is not vkCmdPushDescriptorSetKHR, which is specification insanity but there we go...
	},
	# we need to introduce a virtual parameter here that combine information here to express these dependencies, which is horrible but unavoidable
	# stageFlags, isDynamicViewports and isDynamicScissors are all our inventions
	'VkGraphicsPipelineCreateInfo': {
		'pTessellationState': '((stageFlags & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) && (stageFlags & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))',
		'pViewportState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)',
		'pMultisampleState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)',
		'pDepthStencilState' : '(!sptr->pRasterizationState->rasterizerDiscardEnable)', # "or if the subpass of the render pass the pipeline is created against does not use a depth/stencil attachment"
		'pColorBlendState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)', # "or if the subpass of the render pass the pipeline is created against does not use any color attachments"
	},
	'VkPipelineViewportStateCreateInfo': {
		'pViewports': '(!isDynamicViewports)',
		'pScissors': '(!isDynamicScissors)',
	},
	'VkDeviceCreateInfo': {
		'ppEnabledLayerNames': 'false', # deprecated and ignored
	},
	'VkBufferCreateInfo': {
		'pQueueFamilyIndices': '(sptr->sharingMode == VK_SHARING_MODE_CONCURRENT)',
	},
	'VkImageCreateInfo': {
		'pQueueFamilyIndices': '(sptr->sharingMode == VK_SHARING_MODE_CONCURRENT)',
	},
	'VkSwapchainCreateInfoKHR': {
		'pQueueFamilyIndices': '(sptr->imageSharingMode == VK_SHARING_MODE_CONCURRENT)', # implicitly regarded as ignorable
	},
	'VkPhysicalDeviceImageDrmFormatModifierInfoEXT': {
		'pQueueFamilyIndices': '(sptr->sharingMode == VK_SHARING_MODE_CONCURRENT)',
	},
	'VkFramebufferCreateInfo': {
		'pAttachments': '!(sptr->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)',
	},
	# this depends on state outside of the function parameters...
	'VkCommandBufferBeginInfo': {
		'pInheritanceInfo': '(tcmd->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)', # "If this is a primary command buffer, then this value is ignored."
	},
}

# Need to make extra sure these are externally synchronized
extra_sync = [ 'vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR', 'vkQueueWaitIdle', 'vkQueueBindSparse', 'vkDestroyDevice' ]
validate_funcs(extra_sync)

skip_opt_check = ['pAllocator', 'pUserData', 'pfnCallback', 'pfnUserCallback', 'pNext' ]
# for these, thread barrier goes before the function call to sync us up to other threads:
thread_barrier_funcs = [ 'vkQueueSubmit', 'vkResetDescriptorPool', 'vkResetCommandPool', 'vkUnmapMemory', 'vkFlushMappedMemoryRanges', 'vkResetQueryPool',
	'vkResetQueryPoolEXT', 'vkQueueSubmit2', 'vkQueueSubmit2KHR', 'vkQueuePresentKHR', 'vkUnmapMemory2KHR', 'vkUnmapMemory2' ]
validate_funcs(thread_barrier_funcs)
# for these, thread barrier goes after the function call to sync other threads up to us:
push_thread_barrier_funcs = [ 'vkQueueWaitIdle', 'vkDeviceWaitIdle', 'vkResetDescriptorPool', 'vkResetQueryPool', 'vkResetQueryPoolEXT', 'vkResetCommandPool',
	'vkQueuePresentKHR', 'vkWaitForFences', 'vkGetFenceStatus', 'vkWaitForPresent2KHR' ]
validate_funcs(push_thread_barrier_funcs)

# TODO : Add support for these functions and structures
functions_noop = [
	'vkGetImageViewOpaqueCaptureDescriptorDataEXT',
	'vkCmdPushDescriptorSetWithTemplate2KHR', 'vkCmdPushDescriptorSetWithTemplate2',
	'vkGetPipelinePropertiesEXT', 'vkGetBufferOpaqueCaptureDescriptorDataEXT', 'vkGetTensorOpaqueCaptureDescriptorDataARM', 'vkGetTensorViewOpaqueCaptureDescriptorDataARM',
	'vkCmdBuildMicromapsEXT', 'vkBuildMicromapsEXT', 'vkGetMicromapBuildSizesEXT', 'vkGetImageOpaqueCaptureDescriptorDataEXT', 'vkGetSamplerOpaqueCaptureDescriptorDataEXT',
	'vkGetDeviceFaultInfoEXT', # we never want to trace this, but rather inject it during tracing if device loss happens, print the info, then abort
	'vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT', 'vkCmdSetRenderingInputAttachmentIndicesKHR', 'vkCmdSetRenderingInputAttachmentIndices',
	'vkGetEncodedVideoSessionParametersKHR', 'vkCreatePipelineBinariesKHR', 'vkGetPipelineBinaryDataKHR',
]
validate_funcs(functions_noop)
struct_noop = []

# these should skip their native/upstream call and leave everything up to the post-execute callback
noscreen_calls = [ 'vkDestroySurfaceKHR', 'vkAcquireNextImageKHR', 'vkCreateSwapchainKHR', 'vkGetPhysicalDeviceSurfaceCapabilitiesKHR', 'vkAcquireNextImage2KHR',
	'vkGetPhysicalDeviceSurfacePresentModesKHR', 'vkGetPhysicalDeviceXlibPresentationSupportKHR', 'vkQueuePresentKHR', 'vkGetDeviceGroupPresentCapabilitiesKHR',
	'vkGetPhysicalDevicePresentRectanglesKHR', 'vkDestroySwapchainKHR', 'vkGetDeviceGroupSurfacePresentModesKHR', 'vkCreateSharedSwapchainsKHR',
	'vkGetSwapchainStatusKHR', 'vkGetSwapchainCounterEXT', 'vkGetRefreshCycleDurationGOOGLE', 'vkGetPastPresentationTimingGOOGLE', 'vkSetHdrMetadataEXT',
	'vkSetLocalDimmingAMD', 'vkGetPhysicalDeviceSurfaceCapabilities2EXT', 'vkGetPhysicalDeviceSurfaceFormats2KHR',
	'vkGetPhysicalDeviceSurfaceCapabilities2KHR', 'vkCreateDisplayPlaneSurfaceKHR', 'vkGetPhysicalDeviceSurfaceSupportKHR', 'vkGetPhysicalDeviceSurfaceFormatsKHR',
	'VkSwapchainCreateInfoKHR', 'vkAcquireFullScreenExclusiveModeEXT', 'vkReleaseFullScreenExclusiveModeEXT', 'vkWaitForPresentKHR', 'vkWaitForPresent2KHR',
	'vkSetSwapchainPresentTimingQueueSizeEXT', 'vkGetSwapchainTimingPropertiesEXT', 'vkGetSwapchainTimeDomainPropertiesEXT' ]

# make sure we've thought about all the ways virtual swapchains interact with everything else
virtualswap_calls = [ 'vkCreateSwapchainKHR', 'vkDestroySwapchainKHR', 'vkCreateSharedSwapchainsKHR', 'vkGetSwapchainStatusKHR', 'vkGetSwapchainCounterEXT' ]
validate_funcs(virtualswap_calls)

# These functions are hard-coded in hardcode_{write|read}.cpp
hardcoded = [ 'vkGetSwapchainImagesKHR', 'vkCreateAndroidSurfaceKHR', 'vkGetDeviceProcAddr',
	'vkGetInstanceProcAddr', 'vkCreateWaylandSurfaceKHR', 'vkCreateHeadlessSurfaceEXT', 'vkCreateXcbSurfaceKHR', 'vkCreateXlibSurfaceKHR',
	'vkDestroySurfaceKHR', 'vkGetDeviceQueue', 'vkGetDeviceQueue2', "vkGetAndroidHardwareBufferPropertiesANDROID", "vkGetMemoryAndroidHardwareBufferANDROID",
	'vkEnumerateInstanceLayerProperties', 'vkEnumerateInstanceExtensionProperties', 'vkEnumerateDeviceLayerProperties', 'vkEnumerateDeviceExtensionProperties',
	'vkGetPhysicalDeviceXlibPresentationSupportKHR', 'vkCreateWin32SurfaceKHR', 'vkCreateDirectFBSurfaceEXT', 'vkCreateMetalSurfaceEXT' ]
validate_funcs(hardcoded)
hardcoded_write = [ 'vkGetPhysicalDeviceToolPropertiesEXT', 'vkGetPhysicalDeviceToolProperties', 'vkGetPhysicalDeviceQueueFamilyProperties' ]
validate_funcs(hardcoded_write)
hardcoded_read = []
validate_funcs(hardcoded_read)
# For these functions it is ok if the function pointer is missing, since we implement them ourselves
layer_implemented = [ 'vkCreateDebugReportCallbackEXT', 'vkDestroyDebugReportCallbackEXT', 'vkDebugReportMessageEXT', 'vkDebugMarkerSetObjectTagEXT',
	'vkDebugMarkerSetObjectNameEXT', 'vkCmdDebugMarkerBeginEXT', 'vkCmdDebugMarkerEndEXT', 'vkCmdDebugMarkerInsertEXT', 'vkSetDebugUtilsObjectNameEXT',
	'vkSetDebugUtilsObjectTagEXT',	'vkQueueBeginDebugUtilsLabelEXT', 'vkQueueEndDebugUtilsLabelEXT', 'vkQueueInsertDebugUtilsLabelEXT',
	'vkCmdBeginDebugUtilsLabelEXT', 'vkCmdEndDebugUtilsLabelEXT', 'vkCmdInsertDebugUtilsLabelEXT', 'vkCreateDebugUtilsMessengerEXT',
	'vkDestroyDebugUtilsMessengerEXT', 'vkSubmitDebugUtilsMessageEXT', 'vkGetPhysicalDeviceToolPropertiesEXT', 'vkGetPhysicalDeviceToolProperties' ]
validate_funcs(layer_implemented)
# functions we should ignore on replay
ignore_on_read = [ 'vkGetMemoryHostPointerPropertiesEXT', 'vkCreateDebugUtilsMessengerEXT', 'vkDestroyDebugUtilsMessengerEXT', 'vkAllocateMemory',
	'vkMapMemory', 'vkUnmapMemory', 'vkCreateDebugReportCallbackEXT', 'vkDestroyDebugReportCallbackEXT', 'vkFlushMappedMemoryRanges',
	'vkInvalidateMappedMemoryRanges', 'vkFreeMemory', 'vkGetPhysicalDeviceXcbPresentationSupportKHR', 'vkMapMemory2KHR', 'vkUnmapMemory2KHR',
	'vkGetImageMemoryRequirements2KHR', 'vkGetBufferMemoryRequirements2KHR', 'vkGetImageSparseMemoryRequirements2KHR', 'vkGetImageMemoryRequirements',
	'vkGetBufferMemoryRequirements', 'vkGetImageSparseMemoryRequirements', 'vkGetImageMemoryRequirements2', 'vkGetBufferMemoryRequirements2',
	'vkGetImageSparseMemoryRequirements2', 'vkMapMemory2', 'vkUnmapMemory2', 'vkGetPhysicalDeviceWaylandPresentationSupportKHR' ]
validate_funcs(ignore_on_read)
# functions we should not call natively when tracing - let pre or post calls handle it
ignore_on_trace = []
validate_funcs(ignore_on_trace)
# these functions have hard-coded post-execute callbacks
replay_pre_calls = [ 'vkDestroyInstance', 'vkDestroyDevice', 'vkCreateDevice', 'vkCreateSampler', 'vkQueuePresentKHR', 'vkCreateSwapchainKHR',
	'vkCreateSharedSwapchainsKHR', 'vkCreateGraphicsPipelines', 'vkCreateComputePipelines', 'vkCreateRayTracingPipelinesKHR', 'vkCmdPushConstants2KHR',
	'vkCmdPushConstants2', 'vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR', 'vkDestroyPipelineCache', 'vkDestroySwapchainKHR', 'vkCreateInstance' ]
validate_funcs(replay_pre_calls)
replay_post_calls = [ 'vkCreateInstance', 'vkDestroyInstance', 'vkQueuePresentKHR', 'vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR',
	'vkGetBufferDeviceAddress', 'vkGetBufferDeviceAddressKHR', 'vkGetAccelerationStructureDeviceAddressKHR', 'vkSubmitDebugUtilsMessageEXT' ]
validate_funcs(replay_post_calls)
trace_pre_calls = [ 'vkQueueSubmit', 'vkCreateInstance', 'vkCreateDevice', 'vkFreeMemory', 'vkQueueSubmit2', 'vkQueueSubmit2KHR' ]
validate_funcs(trace_pre_calls)
trace_post_calls = [ 'vkCreateInstance', 'vkCreateDevice', 'vkDestroyInstance', 'vkGetPhysicalDeviceFeatures', 'vkGetPhysicalDeviceProperties',
		'vkGetPhysicalDeviceSurfaceCapabilitiesKHR', 'vkBindImageMemory', 'vkBindBufferMemory', 'vkBindImageMemory2', 'vkBindImageMemory2KHR',
		'vkBindBufferMemory2', 'vkUpdateDescriptorSets', 'vkUpdateDescriptorSetWithTemplate', 'vkUpdateDescriptorSetWithTemplateKHR',
		'vkFlushMappedMemoryRanges', 'vkQueuePresentKHR', 'vkMapMemory2KHR', 'vkMapMemory2', 'vkMapMemory', 'vkCmdBindDescriptorSets',
		'vkBindBufferMemory2KHR', 'vkCmdPushDescriptorSet2KHR', 'vkCmdPushDescriptorSet2', 'vkCmdPushDescriptorSetWithTemplate',
		'vkCmdPushDescriptorSetWithTemplateKHR', 'vkCreateDescriptorUpdateTemplate', 'vkCreateDescriptorUpdateTemplateKHR',
		'vkGetImageMemoryRequirements', 'vkGetPipelineCacheData', 'vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR',
		'vkGetBufferMemoryRequirements', 'vkGetBufferMemoryRequirements2', 'vkGetImageMemoryRequirements2', 'vkGetPhysicalDeviceMemoryProperties',
		'vkGetPhysicalDeviceFormatProperties', 'vkGetPhysicalDeviceFormatProperties2', 'vkCmdPushDescriptorSetKHR', 'vkCmdPushDescriptorSet', 'vkCreateSwapchainKHR',
		'vkGetBufferMemoryRequirements2KHR', 'vkGetDeviceBufferMemoryRequirements', 'vkGetDeviceBufferMemoryRequirementsKHR',
		'vkGetDeviceImageMemoryRequirements', 'vkGetDeviceImageMemoryRequirementsKHR', 'vkGetPhysicalDeviceFeatures2', 'vkGetPhysicalDeviceFeatures2KHR',
		'vkGetPhysicalDeviceMemoryProperties2', 'vkGetDeviceImageSparseMemoryRequirementsKHR', 'vkGetDeviceImageSparseMemoryRequirements',
		'vkCreateShaderModule', 'vkGetBufferDeviceAddress', 'vkGetBufferDeviceAddressKHR', 'vkGetAccelerationStructureDeviceAddressKHR',
		'vkCmdBindDescriptorSets2KHR', 'vkCmdBindDescriptorSets2', 'vkGetTensorMemoryRequirementsARM', 'vkBindTensorMemoryARM', 'vkSubmitDebugUtilsMessageEXT',
		'vkGetPhysicalDeviceProperties2', 'vkGetPhysicalDeviceProperties2KHR' ]
validate_funcs(trace_post_calls)
skip_post_calls = [ 'vkGetQueryPoolResults', 'vkGetPhysicalDeviceXcbPresentationSupportKHR' ]
validate_funcs(skip_post_calls)
# Workaround to be able to rewrite parameter inputs while tracing: These input variables are copied and replaced to not be const anymore.
deconstify = {
	'vkAllocateMemory' : 'pAllocateInfo',
	'vkCreateInstance' : 'pCreateInfo',
	'vkCreateDevice' : 'pCreateInfo',
	'vkCreateSampler' : 'pCreateInfo',
	'vkCreateSwapchainKHR' : 'pCreateInfo',
	'vkCreateCommandPool' : 'pCreateInfo',
	'vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR' : 'pPerformanceQueryCreateInfo',
	'vkCreateVideoSessionKHR' : 'pCreateInfo',
}
validate_funcs(deconstify.keys())
# Workaround to deconstify nested structures
deconst_struct = [
	'VkDeviceQueueCreateInfo', 'VkDeviceQueueInfo2', 'VkQueryPoolPerformanceCreateInfoKHR', 'VkVideoSessionCreateInfoKHR', 'VkDeviceCreateInfo', 'VkCommandPoolCreateInfo',
]
# Subclassing of trackable
trackable_type_map_general = { 'VkBuffer': 'trackedbuffer', 'VkImage': 'trackedimage', 'VkCommandBuffer': 'trackedcmdbuffer', 'VkDescriptorSet': 'trackeddescriptorset',
	'VkDeviceMemory': 'trackedmemory', 'VkFence': 'trackedfence', 'VkPipeline': 'trackedpipeline', 'VkImageView': 'trackedimageview', 'VkBufferView': 'trackedbufferview',
	'VkDevice': 'trackeddevice', 'VkFramebuffer': 'trackedframebuffer', 'VkRenderPass': 'trackedrenderpass', 'VkQueue': 'trackedqueue', 'VkPhysicalDevice': 'trackedphysicaldevice',
	'VkShaderModule': 'trackedshadermodule', 'VkAccelerationStructureKHR': 'trackedaccelerationstructure', 'VkPipelineLayout': 'trackedpipelinelayout',
	'VkDescriptorSetLayout': 'trackeddescriptorsetlayout', 'VkTensorARM': 'trackedtensor', 'VkDescriptorUpdateTemplate': 'trackeddescriptorupdatetemplate' }
trackable_type_map_trace = trackable_type_map_general.copy()
trackable_type_map_trace.update({ 'VkCommandBuffer': 'trackedcmdbuffer_trace', 'VkSwapchainKHR': 'trackedswapchain', 'VkDescriptorSet': 'trackeddescriptorset_trace',
	'VkEvent': 'trackedevent_trace', 'VkDescriptorPool': 'trackeddescriptorpool_trace', 'VkCommandPool': 'trackedcommandpool_trace' })
trackable_type_map_replay = trackable_type_map_general.copy()
trackable_type_map_replay.update({ 'VkCommandBuffer': 'trackedcmdbuffer', 'VkDescriptorSet': 'trackeddescriptorset', 'VkSwapchainKHR': 'trackedswapchain_replay' })
