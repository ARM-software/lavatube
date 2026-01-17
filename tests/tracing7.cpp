// Test of VK_ARM_trace_helpers support

#include "tests/common.h"
#include <inttypes.h>
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"

typedef void (VKAPI_PTR *PFN_vkSyncBufferTRACETOOLTEST)(VkDevice device, VkBuffer buffer);

#define TEST_NAME "tracing_7"
#define NUM_BUFFERS 3

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

static void trace_3()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result;

	PFN_vkAssertBufferARM vkAssertBuffer = (PFN_vkAssertBufferARM)trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertBufferARM");
	assert(vkAssertBuffer != nullptr);
	PFN_vkSyncBufferTRACETOOLTEST vkSyncBuffer = (PFN_vkSyncBufferTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkSyncBufferTRACETOOLTEST");
	assert(vkSyncBuffer != nullptr);

	VkBuffer buffer[NUM_BUFFERS];
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = 99;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[i]);
		check(result);
	}

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[0], &req);
	uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VkMemoryAllocateInfo pAllocateMemInfo = {};
	pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = req.size * NUM_BUFFERS;
	VkDeviceMemory memory = 0;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
	check(result);
	assert(memory != 0);

	uint32_t checksum = 0;
	char* ptr = (char*)malloc(pAllocateMemInfo.allocationSize);
	for (unsigned b = 0; b < NUM_BUFFERS; b++)
	{
		for (unsigned ii = 0; ii < bufferCreateInfo.size; ii++) ptr[ii + b * req.size] = ii % 255; // just fill it with a pattern
		checksum = adler32((unsigned char*)&ptr[b * req.size], bufferCreateInfo.size);
		printf("Buffer %d checksum=%u\n", (int)b, (unsigned)checksum);
	}

	VkDeviceSize offset = 0;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkBindBufferMemory(vulkan.device, buffer[i], memory, offset);

		char* mapptr = nullptr;
		result = trace_vkMapMemory(vulkan.device, memory, offset, bufferCreateInfo.size, 0, (void**)&mapptr);
		check(result);

		// TBD memory map and send in some descriptor buffer stuff here that should not change on replay
		memcpy(mapptr, ptr, bufferCreateInfo.size); // TBD replace

		trace_vkUnmapMemory(vulkan.device, memory);

		offset += req.size;
	}
	free(ptr);
	ptr = nullptr;

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		uint32_t checksum2 = 0;
		result = trace_vkAssertBufferARM(vulkan.device, buffer[i], 0, VK_WHOLE_SIZE, &checksum2, "test");
		check(result);
		assert(checksum == checksum2);
	}

	// Cleanup...
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkDestroyBuffer(vulkan.device, buffer[i], nullptr);
	}
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		t.parent->allocator.self_test();
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
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		update_tensor_packet(instrtype, t);
	}
	else assert(false);
	t.parent->allocator.self_test();
	return !done;
}

static void retrace_3()
{
	lava_reader r(TEST_NAME ".vk");
	lava_file_reader& t = r.file_reader(0);
	int remaining = r.allocator.self_test();
	assert(remaining == 0); // there should be nothing now
	while (getnext(t)) {}
	remaining = r.allocator.self_test();
	assert(remaining == 0); // everything should be destroyed now
}

int main()
{
	trace_3();
	retrace_3();
	return 0;
}
