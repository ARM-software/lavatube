#include <vector>

#include "tests/common.h"
#include "write.h"

static const VkDeviceSize buffer_size = 4 * 1024 * 1024;
static const uint32_t initialization_count = 8;
static const uint32_t submit_count = 4096;

int main()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init("tracing_injected_queue_sync", reqs);
	lava_writer& writer = lava_writer::instance();

	VkQueue queue = VK_NULL_HANDLE;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);

	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.size = buffer_size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &buffer);
	check(result);

	VkMemoryRequirements requirements = {};
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer, &requirements);
	VkMemoryAllocateInfo allocation_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	allocation_info.allocationSize = requirements.size;
	allocation_info.memoryTypeIndex = get_device_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &allocation_info, nullptr, &memory);
	check(result);
	result = trace_vkBindBufferMemory(vulkan.device, buffer, memory, 0);
	check(result);
	VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
	VkFence fence = VK_NULL_HANDLE;
	result = trace_vkCreateFence(vulkan.device, &fence_info, nullptr, &fence);
	check(result);
	result = trace_vkWaitForFences(vulkan.device, 1, &fence, VK_TRUE, 0);
	check(result);
	result = trace_vkResetFences(vulkan.device, 1, &fence);
	check(result);
	result = trace_vkWaitForFences(vulkan.device, 1, &fence, VK_TRUE, 0);
	assert(result == VK_TIMEOUT);
	trace_vkDestroyFence(vulkan.device, fence, nullptr);
	VkEventCreateInfo event_info = { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, nullptr };
	VkEvent event = VK_NULL_HANDLE;
	result = trace_vkCreateEvent(vulkan.device, &event_info, nullptr, &event);
	check(result);
	result = trace_vkGetEventStatus(vulkan.device, event);
	assert(result == VK_EVENT_RESET);
	trace_vkDestroyEvent(vulkan.device, event, nullptr);

	trackeddevice* device_data = writer.records.VkDevice_index.at(vulkan.device);
	trackedbuffer* buffer_data = writer.records.VkBuffer_index.at(buffer);
	const uint32_t setup_endpoint = writer.file_writer().current.packet;

	writer.prepare_threads(2);
	writer.bind_thread(1);
	// Imported API calls retain their source threads without capture-side queue barriers.
	// Model that here so these submits can overlap the replay-only initialization packets.
	writer.file_writer().write_output = true;
	writer.file_writer().write_thread_barrier({ setup_endpoint, 0 });
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	for (uint32_t i = 0; i < submit_count; i++)
	{
		result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
		check(result);
	}
	result = trace_vkQueueWaitIdle(queue);
	check(result);
	const uint32_t submit_endpoint = writer.file_writer().current.packet;

	writer.bind_thread(0);
	writer.file_writer().write_thread_barrier({ setup_endpoint, 1 });
	std::vector<uint8_t> data(buffer_size, 0x5a);
	for (uint32_t i = 0; i < initialization_count; i++)
	{
		lava_file_writer& file = writer.file_writer();
		file.begin_packet(PACKET_BUFFER_INITIALIZATION);
		file.write_handle(device_data);
		file.write_handle(buffer_data);
		file.write_uint64_t(data.size());
		file.write_array(data.data(), data.size());
		file.end_packet();
	}
	writer.file_writer().write_thread_barrier({ writer.file_writer().current.packet, submit_endpoint });

	trace_vkDestroyBuffer(vulkan.device, buffer, nullptr);
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
	return 0;
}
