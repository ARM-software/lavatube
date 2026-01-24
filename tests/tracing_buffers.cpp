// Test of VK_ARM_trace_helpers support

#include "tests/common.h"
#include <inttypes.h>
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"

#define TEST_NAME "tracing_buffers"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

static void trace()
{
	// Set before device creation to make sure it propagates everywhere.
	p__trust_host_flushes = 1;

	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result;

	PFN_vkAssertBufferARM vkAssertBuffer = (PFN_vkAssertBufferARM)trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertBufferARM");
	assert(vkAssertBuffer != nullptr);

	test_marker(vulkan, "Creating buffers");
	VkBuffer buffer[3];
	VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	bufferCreateInfo.size = 99;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[0]);
	check(result);
	bufferCreateInfo.size = UINT16_MAX + 99;
	result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[1]);
	check(result);
	bufferCreateInfo.size = 5000;
	result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[2]);
	check(result);

	VkMemoryRequirements req[3];
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[0], &req[0]);
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[1], &req[1]);
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[2], &req[2]);
	uint32_t memoryTypeIndex[3];
	memoryTypeIndex[0] = get_device_memory_type(req[0].memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	memoryTypeIndex[1] = get_device_memory_type(req[1].memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	memoryTypeIndex[2] = get_device_memory_type(req[2].memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	assert(memoryTypeIndex[0] == memoryTypeIndex[1] && memoryTypeIndex[1] == memoryTypeIndex[2]);
	uint32_t aligned_size[3];
	for (unsigned i = 0; i < 3; i++)
	{
		const uint32_t align_mod = req[i].size % req[i].alignment;
		aligned_size[i] = (align_mod == 0) ? req[i].size : (req[i].size + req[i].alignment - align_mod);
	}

	VkMemoryAllocateInfo pAllocateMemInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex[0];
	pAllocateMemInfo.allocationSize = aligned_size[0] + aligned_size[1] + aligned_size[2];
	VkDeviceMemory memory = 0;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
	check(result);
	assert(memory != 0);

	test_marker(vulkan, "Binding buffers");
	trace_vkBindBufferMemory(vulkan.device, buffer[0], memory, 0);
	ILOG("Binding to %d - alignment %d", (int)0, (int)req[0].alignment);
	trace_vkBindBufferMemory(vulkan.device, buffer[1], memory, aligned_size[0]);
	ILOG("Binding to %d - alignment %d", (int)aligned_size[0], (int)req[1].alignment);
	trace_vkBindBufferMemory(vulkan.device, buffer[2], memory, aligned_size[0] + aligned_size[1]);
	ILOG("Binding to %d - alignment %d", (int)(aligned_size[0] + aligned_size[1]), (int)req[2].alignment);

	test_marker(vulkan, "Filling buffers");
	char* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize; ii++)
	{
		ptr[ii] = ii % 255;
	}
	VkMappedMemoryRange flush = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr };
	flush.memory = memory;
	flush.offset = 0;
	flush.size = pAllocateMemInfo.allocationSize;
	trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Asserting buffers");
	uint32_t checksum = 0;
	result = trace_vkAssertBufferARM(vulkan.device, buffer[0], 0, VK_WHOLE_SIZE, &checksum, nullptr);
	check(result);
	result = trace_vkAssertBufferARM(vulkan.device, buffer[1], 0, VK_WHOLE_SIZE, &checksum, nullptr);
	check(result);
	result = trace_vkAssertBufferARM(vulkan.device, buffer[2], 0, VK_WHOLE_SIZE, &checksum, nullptr);
	check(result);

	trace_vkDestroyBuffer(vulkan.device, buffer[0], nullptr);
	trace_vkDestroyBuffer(vulkan.device, buffer[1], nullptr);
	trace_vkDestroyBuffer(vulkan.device, buffer[2], nullptr);
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
}

static int count_flushes = 0;

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == 1) done = true; // is vkDestroyInstance
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else if (instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_IMAGE_UPDATE2)
	{
		update_image_packet(instrtype, t);
	}
	else if (instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_BUFFER_UPDATE2)
	{
		update_buffer_packet(instrtype, t);
		count_flushes++;
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		update_tensor_packet(instrtype, t);
	}
	else assert(false);
	return !done;
}

static void retrace()
{
	lava_reader r(TEST_NAME ".vk");
	lava_file_reader& t = r.file_reader(0);
	while (getnext(t)) {}
	assert(count_flushes == 3);
}

int main()
{
	trace();
	retrace();
	return 0;
}
