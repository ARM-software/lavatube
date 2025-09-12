Things to do
============

General:
* More work needed on trace portability
* More work on making buffer suballocations faster
* Vulkan-SC support, look into on-the-fly conversion to and from normal Vulkan
* Rayquery / raytracing
* Push descriptors
* Inline uniform blocks
* Memory aliasing
* Make VkLayer_lavatube.json truthful
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
* VK_EXT_mutable_descriptor_type
* VK_EXT_device_generated_commands
* VK_KHR_pipeline_binary

Replayer:
* Add back Android build
* Checkpoint and fastforward traces
* VK_EXT_pipeline_creation_feedback
* Built-in screenshotting support, reading from virtual swapchain
* Blackhole and none WSI generate validation warnings

Tools
* Trace to text tool
* Improve the python code generators
