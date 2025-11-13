Things to do
============

General:
* More work needed on trace portability
* More work on making buffer suballocations faster
* Vulkan-SC support, look into on-the-fly conversion to and from normal Vulkan
* Rayquery / raytracing support
* Push descriptors support
* Inline uniform blocks support
* Memory aliasing support
* Make VkLayer_lavatube.json truthful
* Drop our own packfile format for using zip files instead
* Improved multi-device support
	* Store internal Vulkan object metadata by Vulkan device

Missing Vulkan call implementations:
* vkUpdateDescriptorSets with descriptorCopyCount > 0
* vkUpdateDescriptorSetWithTemplate(KHR)
* vkCmdPushDescriptorSetWithTemplate(KHR)
* vkGetPipelinePropertiesEXT
* vkCmdBuildMicromapsEXT
* vkBuildMicromapsEXT
* vkGetMicromapBuildSizesEXT
* vkGetDeviceFaultInfoEXT

Missing and desirable extension support:
* VK_EXT_descriptor_buffer
* VK_EXT_mutable_descriptor_type
* VK_EXT_device_generated_commands
* VK_KHR_pipeline_binary

Replayer:
* Add back Android build
* Trace fastforwarding
* VK_EXT_pipeline_creation_feedback
* Built-in screenshotting support, reading from virtual swapchain

Tools
* Trace to text tool
* Improve the python code generators (very ugly code)
