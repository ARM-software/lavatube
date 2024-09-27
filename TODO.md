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
* vkGetDeviceBufferMemoryRequirements(KHR)
* vkGetDeviceImageMemoryRequirements(KHR)
* vkUpdateDescriptorSets with descriptorCopyCount > 0
* vkUpdateDescriptorSetWithTemplate(KHR)
* vkCmdPushDescriptorSetWithTemplate(KHR)
* vkGetPipelinePropertiesEXT
* vkCmdBuildMicromapsEXT
* vkBuildMicromapsEXT
* vkGetMicromapBuildSizesEXT
* vkGetDeviceFaultInfoEXT
* vkCmdUpdateBuffer

Missing and desirable extension support:
* VK_EXT_mutable_descriptor_type

Replayer:
* Preloading
* Add back Android build
* Checkpoint and fastforward traces
* VK_EXT_pipeline_creation_feedback
* Built-in screenshotting support, reading from virtual swapchain
* Blackhole and none WSI generate validation warnings

Tools
* Trace to text tool
* Upgrade the code generators to python 3 (and generally improve them)
