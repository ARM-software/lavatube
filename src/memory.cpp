#include "memory.h"

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
	VkDeviceBufferMemoryRequirements info = { VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, nullptr };
	info.pCreateInfo = &cinfo;
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	if (use_dedicated_allocation()) req.pNext = &reqs.dedicated;
	wrap_vkGetDeviceBufferMemoryRequirements(device, &info, &req);
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
	VkDeviceImageMemoryRequirements info = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, nullptr };
	info.pCreateInfo = &cinfo;
	info.planeAspect = VK_IMAGE_ASPECT_NONE; // ignored unless tiling is DRM or DISJOINT, and we support neither for now
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	if (use_dedicated_allocation()) req.pNext = &reqs.dedicated;
	wrap_vkGetDeviceImageMemoryRequirements(device, &info, &req);
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
