#include <errno.h>
#include <inttypes.h>
#include <limits>
#include <stdlib.h>
#include <string.h>

#include "tests/common.h"

static bool parse_size(const char* text, VkDeviceSize& size)
{
	char* end = nullptr;
	errno = 0;
	const unsigned long long value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || value == 0) return false;
	uint64_t multiplier = 1;
	if (*end != '\0')
	{
		if (end[1] != '\0') return false;
		if (*end == 'K' || *end == 'k') multiplier = 1024;
		else if (*end == 'M' || *end == 'm') multiplier = 1024 * 1024;
		else if (*end == 'G' || *end == 'g') multiplier = 1024ull * 1024ull * 1024ull;
		else return false;
	}
	if (value > std::numeric_limits<uint64_t>::max() / multiplier) return false;
	size = (VkDeviceSize)(value * multiplier);
	return size >= 4;
}

static bool memory_type_matches(VkMemoryPropertyFlags flags, const char* memory_class, bool preferred)
{
	if (strcmp(memory_class, "cached") == 0)
	{
		return (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
			== (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	}
	if (strcmp(memory_class, "uncached") == 0)
	{
		if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 || (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) return false;
		return !preferred || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	if (strcmp(memory_class, "device") == 0)
	{
		if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) return false;
		return !preferred || (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0;
	}
	return false;
}

static bool select_memory_type(VkPhysicalDevice physical, uint32_t type_bits, const char* memory_class,
	uint32_t& memory_type, VkMemoryPropertyFlags& selected_flags)
{
	VkPhysicalDeviceMemoryProperties properties = {};
	trace_vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (uint32_t pass = 0; pass < 2; pass++)
	{
		const bool preferred = pass == 0;
		for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
		{
			const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
			if ((type_bits & (1u << i)) && memory_type_matches(flags, memory_class, preferred))
			{
				memory_type = i;
				selected_flags = flags;
				return true;
			}
		}
	}
	return false;
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "usage: tracing_cli_save_buffer_perf SIZE cached|uncached|device\n");
		return 2;
	}
	VkDeviceSize buffer_size = 0;
	if (!parse_size(argv[1], buffer_size)
	    || (strcmp(argv[2], "cached") != 0 && strcmp(argv[2], "uncached") != 0 && strcmp(argv[2], "device") != 0))
	{
		fprintf(stderr, "invalid buffer size or memory class\n");
		return 2;
	}

	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init("tracing_cli_save_buffer_perf", reqs);
	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.size = buffer_size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &buffer);
	check(result);
	test_set_name(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, "lava-cli save benchmark buffer");

	VkMemoryRequirements requirements = {};
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer, &requirements);
	uint32_t memory_type = UINT32_MAX;
	VkMemoryPropertyFlags memory_flags = 0;
	if (!select_memory_type(vulkan.physical, requirements.memoryTypeBits, argv[2], memory_type, memory_flags))
	{
		fprintf(stderr, "memory class %s is unavailable for this buffer\n", argv[2]);
		trace_vkDestroyBuffer(vulkan.device, buffer, nullptr);
		test_done(vulkan);
		return 77;
	}
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = memory_type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &allocation, nullptr, &memory);
	check(result);
	result = trace_vkBindBufferMemory(vulkan.device, buffer, memory, 0);
	check(result);

	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
	pool_info.queueFamilyIndex = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	result = trace_vkCreateCommandPool(vulkan.device, &pool_info, nullptr, &pool);
	check(result);
	VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
	command_info.commandPool = pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	result = trace_vkAllocateCommandBuffers(vulkan.device, &command_info, &command_buffer);
	check(result);
	VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = trace_vkBeginCommandBuffer(command_buffer, &begin_info);
	check(result);
	const VkDeviceSize fill_size = buffer_size - (buffer_size % 4);
	trace_vkCmdFillBuffer(command_buffer, buffer, 0, fill_size, 0x5aa55aa5);
	result = trace_vkEndCommandBuffer(command_buffer);
	check(result);
	VkQueue queue = VK_NULL_HANDLE;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer;
	result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	check(result);
	result = trace_vkQueueWaitIdle(queue);
	check(result);
	printf("buffer_index=0 size=%" PRIu64 " memory=%s memory_type=%u flags=%u\n",
		(uint64_t)buffer_size, argv[2], memory_type, (unsigned)memory_flags);
	result = trace_vkDeviceWaitIdle(vulkan.device);
	check(result);

	trace_vkDestroyCommandPool(vulkan.device, pool, nullptr);
	trace_vkDestroyBuffer(vulkan.device, buffer, nullptr);
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
	return 0;
}
