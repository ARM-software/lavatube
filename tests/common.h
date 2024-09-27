#pragma once

#include <vector>
#include <string>

#include "vulkan/vulkan.h"

#include "util.h"
#include "read_auto.h"
#include "write_auto.h"
#include "suballocator.h"
#include "tests/tests.h"

#define check(result) \
	if (result != VK_SUCCESS) \
	{ \
		fprintf(stderr, "Error 0x%04x: %s\n", result, errorString(result)); \
	} \
	assert(result == VK_SUCCESS);

struct vulkan_setup_t
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
};

vulkan_setup_t test_init(const std::string& testname, size_t size = 0);
void test_done(vulkan_setup_t s);
uint32_t get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
void test_set_name(VkDevice device, VkObjectType type, uint64_t handle, const char* name);

void print_cmdbuf(vulkan_setup_t& vulkan, VkCommandBuffer cmdbuf);
void print_memory(vulkan_setup_t& vulkan, VkDeviceMemory memory, const char* name);
void print_buffer(vulkan_setup_t& vulkan, VkBuffer buffer);

// Prior assumption: Memory is not already mapped.
static inline void test_destroy_buffer(vulkan_setup_t& vulkan, unsigned value, VkDeviceMemory memory, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
	uint8_t* ptr = nullptr;
	VkResult result = trace_vkMapMemory(vulkan.device, memory, offset, size, 0, (void**)&ptr);
	if (result != VK_SUCCESS) ABORT("Failed to map memory in test_assert_buffer");
	assert(ptr[0] == value);
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkAssertBufferTRACETOOLTEST(vulkan.device, buffer);
	trace_vkDestroyBuffer(vulkan.device, buffer, nullptr);
}
