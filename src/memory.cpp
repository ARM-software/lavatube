#include "memory.h"
#include "util.h"

// Android may export unsupported device commands as loader trampolines which
// jump through a null device dispatch slot. Query the device dispatch directly.
// TODO Remove all of this once Android gets fixed / assumes a recent enough Vulkan version.
static void get_buffer_memory_requirements_compat(VkDevice device, const VkBufferCreateInfo& cinfo, VkMemoryRequirements2& req)
{
	PFN_vkGetDeviceBufferMemoryRequirements get_device_requirements = nullptr;
	if (wrap_vkGetDeviceProcAddr)
	{
		get_device_requirements = reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(wrap_vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirements"));
		if (!get_device_requirements)
		{
			get_device_requirements = reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(wrap_vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirementsKHR"));
		}
	}
	if (get_device_requirements)
	{
		VkDeviceBufferMemoryRequirements info = { VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, nullptr };
		info.pCreateInfo = &cinfo;
		get_device_requirements(device, &info, &req);
		return;
	}

	VkBuffer buffer = VK_NULL_HANDLE;
	VkResult result = wrap_vkCreateBuffer(device, &cinfo, nullptr, &buffer);
	if (result != VK_SUCCESS)
	{
		ABORT("Failed to create temporary buffer for memory requirements: %s", errorString(result));
	}
	if (wrap_vkGetBufferMemoryRequirements2)
	{
		VkBufferMemoryRequirementsInfo2 info = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, nullptr };
		info.buffer = buffer;
		wrap_vkGetBufferMemoryRequirements2(device, &info, &req);
	}
	else if (wrap_vkGetBufferMemoryRequirements)
	{
		wrap_vkGetBufferMemoryRequirements(device, buffer, &req.memoryRequirements);
	}
	else
	{
		wrap_vkDestroyBuffer(device, buffer, nullptr);
		ABORT("Cannot query temporary buffer memory requirements");
	}
	wrap_vkDestroyBuffer(device, buffer, nullptr);
}
static void get_image_memory_requirements_compat(VkDevice device, const VkImageCreateInfo& cinfo, VkMemoryRequirements2& req)
{
	PFN_vkGetDeviceImageMemoryRequirements get_device_requirements = nullptr;
	if (wrap_vkGetDeviceProcAddr)
	{
		get_device_requirements = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(wrap_vkGetDeviceProcAddr(device, "vkGetDeviceImageMemoryRequirements"));
		if (!get_device_requirements)
		{
			get_device_requirements = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(wrap_vkGetDeviceProcAddr(device, "vkGetDeviceImageMemoryRequirementsKHR"));
		}
	}
	if (get_device_requirements)
	{
		VkDeviceImageMemoryRequirements info = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, nullptr };
		info.pCreateInfo = &cinfo;
		info.planeAspect = VK_IMAGE_ASPECT_NONE; // ignored unless tiling is DRM or DISJOINT, and we support neither for now
		get_device_requirements(device, &info, &req);
		return;
	}

	VkImage image = VK_NULL_HANDLE;
	VkResult result = wrap_vkCreateImage(device, &cinfo, nullptr, &image);
	if (result != VK_SUCCESS)
	{
		ABORT("Failed to create temporary image for memory requirements: %s", errorString(result));
	}
	if (wrap_vkGetImageMemoryRequirements2)
	{
		VkImageMemoryRequirementsInfo2 info = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr };
		info.image = image;
		wrap_vkGetImageMemoryRequirements2(device, &info, &req);
	}
	else if (wrap_vkGetImageMemoryRequirements)
	{
		wrap_vkGetImageMemoryRequirements(device, image, &req.memoryRequirements);
	}
	else
	{
		wrap_vkDestroyImage(device, image, nullptr);
		ABORT("Cannot query temporary image memory requirements");
	}
	wrap_vkDestroyImage(device, image, nullptr);
}

memory_requirements get_trackedtensor_memory_requirements(VkDevice device, const trackedtensor& data)
{
	memory_requirements reqs;
	VkTensorDescriptionARM td = { VK_STRUCTURE_TYPE_TENSOR_DESCRIPTION_ARM, nullptr };
	td.tiling = (VkTensorTilingARM)data.tiling;
	td.format = data.format;
	td.dimensionCount = data.dimensions.size();
	td.pDimensions = data.dimensions.data();
	if (data.strides.size() > 0) td.pStrides = data.strides.data();
	td.usage = data.usage;
	VkTensorCreateInfoARM cinfo = { VK_STRUCTURE_TYPE_TENSOR_CREATE_INFO_ARM, nullptr };
	cinfo.flags = data.flags;
	cinfo.pDescription = &td;
	cinfo.sharingMode = data.sharingMode;
	cinfo.queueFamilyIndexCount = 0;
	cinfo.pQueueFamilyIndices = nullptr;
	VkDeviceTensorMemoryRequirementsARM info = { VK_STRUCTURE_TYPE_DEVICE_TENSOR_MEMORY_REQUIREMENTS_ARM, nullptr };
	info.pCreateInfo = &cinfo;
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	if (use_dedicated_allocation()) req.pNext = &reqs.dedicated;
	wrap_vkGetDeviceTensorMemoryRequirementsARM(device, &info, &req);
	reqs.requirements = req.memoryRequirements;
	reqs.memory_flags = data.memory_flags;
	return reqs;
}

memory_requirements get_trackedbuffer_memory_requirements(VkDevice device, const trackedbuffer& data)
{
	memory_requirements reqs;
	VkBufferCreateInfo cinfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	cinfo.flags = data.flags;
	cinfo.sharingMode = data.sharingMode;
	cinfo.size = data.size;
	cinfo.usage = data.usage;
	cinfo.queueFamilyIndexCount = 0; // hopefully won't make any difference here
	VkBufferUsageFlags2CreateInfo buf2ci = { VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO, nullptr };
	if (data.usage2 != 0)
	{
		buf2ci.usage = data.usage2;
		cinfo.pNext = &buf2ci;
	}
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	if (use_dedicated_allocation()) req.pNext = &reqs.dedicated;
	get_buffer_memory_requirements_compat(device, cinfo, req);
	reqs.requirements = req.memoryRequirements;
	reqs.memory_flags = data.memory_flags;
	if (data.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		reqs.dedicated.prefersDedicatedAllocation = VK_TRUE;
		reqs.allocate_flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
	}
	return reqs;
}

memory_requirements get_trackedimage_memory_requirements(VkDevice device, const trackedimage& data)
{
	memory_requirements reqs;
	// TBD should handle VK_EXT_image_compression_control here
	VkImageCreateInfo cinfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr };
	cinfo.flags = data.flags;
	cinfo.imageType = data.imageType;
	cinfo.format = data.format;
	cinfo.extent= data.extent;
	cinfo.mipLevels = data.mipLevels;
	cinfo.arrayLayers = data.arrayLayers;
	cinfo.samples = data.samples;
	cinfo.tiling = (VkImageTiling)data.tiling;
	cinfo.usage = data.usage;
	cinfo.sharingMode = data.sharingMode;
	cinfo.queueFamilyIndexCount = 0; // hopefully won't make any difference here
	cinfo.initialLayout = data.initialLayout;
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	if (use_dedicated_allocation()) req.pNext = &reqs.dedicated;
	get_image_memory_requirements_compat(device, cinfo, req);
	reqs.requirements = req.memoryRequirements;
	reqs.memory_flags = data.memory_flags;
	assert(reqs.requirements.alignment != 0);
	return reqs;
}

memory_requirements merge_memory_requirements(const memory_requirements& req1, const memory_requirements& req2)
{
	memory_requirements reqs;
	reqs.requirements.memoryTypeBits = req1.requirements.memoryTypeBits & req2.requirements.memoryTypeBits;
	reqs.requirements.size = std::max(req1.requirements.size, req2.requirements.size);
	reqs.requirements.alignment = std::max(req1.requirements.alignment, req2.requirements.alignment);
	reqs.dedicated.prefersDedicatedAllocation = req1.dedicated.prefersDedicatedAllocation | req2.dedicated.prefersDedicatedAllocation;
	reqs.dedicated.requiresDedicatedAllocation = req1.dedicated.requiresDedicatedAllocation | req2.dedicated.requiresDedicatedAllocation;
	reqs.memory_flags = req1.memory_flags | req2.memory_flags;
	reqs.allocate_flags = req1.allocate_flags | req2.allocate_flags;
	return reqs;
}

memory_requirements get_fake_memory_requirements(VkDevice device, const trackedobject& data)
{
	memory_requirements reqs;
	reqs.requirements.size = data.size;
	reqs.requirements.alignment = 1;
	reqs.requirements.memoryTypeBits = 1;
	reqs.memory_flags = data.memory_flags;
	if (data.object_type == VK_OBJECT_TYPE_BUFFER)
	{
		if (((trackedbuffer&)data).usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		{
			reqs.dedicated.prefersDedicatedAllocation = VK_TRUE;
			reqs.allocate_flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
		}
	}
	return reqs;
}
