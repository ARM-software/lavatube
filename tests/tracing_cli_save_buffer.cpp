#include <stdint.h>

#include "tests/common.h"

int main()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init("tracing_cli_save_buffer", reqs);
	const VkDeviceSize device_size = 4 * 1024 * 1024 + 257;

	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.size = device_size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer upload_buffer = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &upload_buffer);
	check(result);
	VkMemoryRequirements upload_requirements = {};
	trace_vkGetBufferMemoryRequirements(vulkan.device, upload_buffer, &upload_requirements);
	VkMemoryAllocateInfo upload_allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	upload_allocation.allocationSize = upload_requirements.size;
	upload_allocation.memoryTypeIndex = get_device_memory_type(upload_requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory upload_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &upload_allocation, nullptr, &upload_memory);
	check(result);
	result = trace_vkBindBufferMemory(vulkan.device, upload_buffer, upload_memory, 0);
	check(result);
	uint8_t* upload_data = nullptr;
	result = trace_vkMapMemory(vulkan.device, upload_memory, 0, VK_WHOLE_SIZE, 0, (void**)&upload_data);
	check(result);
	for (VkDeviceSize i = 0; i < device_size; i++) upload_data[i] = (uint8_t)((i * 37 + 11) & 0xff);
	testFlushMemory(vulkan, upload_memory, 0, VK_WHOLE_SIZE, false, nullptr);

	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer device_buffer = VK_NULL_HANDLE;
	result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &device_buffer);
	check(result);
	VkMemoryRequirements device_requirements = {};
	trace_vkGetBufferMemoryRequirements(vulkan.device, device_buffer, &device_requirements);
	VkMemoryAllocateInfo device_allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	device_allocation.allocationSize = device_requirements.size;
	device_allocation.memoryTypeIndex = get_device_memory_type(device_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory device_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &device_allocation, nullptr, &device_memory);
	check(result);
	result = trace_vkBindBufferMemory(vulkan.device, device_buffer, device_memory, 0);
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
	VkBufferCopy copy = { 0, 0, device_size };
	trace_vkCmdCopyBuffer(command_buffer, upload_buffer, device_buffer, 1, &copy);
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
	result = trace_vkDeviceWaitIdle(vulkan.device);
	check(result);

	trace_vkDestroyCommandPool(vulkan.device, pool, nullptr);
	trace_vkDestroyBuffer(vulkan.device, device_buffer, nullptr);
	trace_vkFreeMemory(vulkan.device, device_memory, nullptr);
	trace_vkUnmapMemory(vulkan.device, upload_memory);
	trace_vkDestroyBuffer(vulkan.device, upload_buffer, nullptr);
	trace_vkFreeMemory(vulkan.device, upload_memory, nullptr);
	test_done(vulkan);
	return 0;
}
