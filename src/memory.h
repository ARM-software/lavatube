#pragma once

#include "lavatube.h"

struct memory_requirements
{
	VkMemoryRequirements requirements { 0 };
	VkMemoryPropertyFlags memory_flags { 0 };
	VkMemoryDedicatedRequirements dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, nullptr };
	VkMemoryAllocateFlags allocate_flags { 0 };
};

// Merge two memory requirements
memory_requirements merge_memory_requirements(const memory_requirements& req1, const memory_requirements& req2);

// Fake memory requirements for post-processing runs
memory_requirements get_fake_memory_requirements(VkDevice device, const trackedobject& data);

// Generate memory requirements from stored meta information
memory_requirements get_trackedbuffer_memory_requirements(VkDevice device, const trackedbuffer& data);
memory_requirements get_trackedimage_memory_requirements(VkDevice device, const trackedimage& data);
memory_requirements get_trackedtensor_memory_requirements(VkDevice device, const trackedtensor& data);
