// Test of VK_TRACETOOLS_trace_helpers support

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

	PFN_vkAssertBufferTRACETOOLTEST vkAssertBuffer = (PFN_vkAssertBufferTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertBufferTRACETOOLTEST");
	assert(vkAssertBuffer != nullptr);
	PFN_vkSyncBufferTRACETOOLTEST vkSyncBuffer = (PFN_vkSyncBufferTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkSyncBufferTRACETOOLTEST");
	assert(vkSyncBuffer != nullptr);
	PFN_vkUpdateBufferTRACETOOLTEST vkUpdateBuffer = (PFN_vkUpdateBufferTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkUpdateBufferTRACETOOLTEST");
	assert(vkUpdateBuffer);
	PFN_vkUpdateImageTRACETOOLTEST vkUpdateImage = (PFN_vkUpdateImageTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkUpdateImageTRACETOOLTEST");
	assert(vkUpdateImage);
	PFN_vkThreadBarrierTRACETOOLTEST vkThreadBarrier = (PFN_vkThreadBarrierTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkThreadBarrierTRACETOOLTEST");
	assert(vkThreadBarrier);

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

		VkUpdateMemoryInfoTRACETOOLTEST updateInfo = { VK_STRUCTURE_TYPE_UPDATE_MEMORY_INFO_TRACETOOLTEST, nullptr };
		updateInfo.dstOffset = 0; // relative to start of buffer
		updateInfo.dataSize = bufferCreateInfo.size;
		updateInfo.pData = ptr;
		trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buffer[i], &updateInfo);

		offset += req.size;
	}
	free(ptr);
	ptr = nullptr;

	vkThreadBarrier(0, nullptr);

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		uint32_t checksum2 = trace_vkAssertBufferTRACETOOLTEST(vulkan.device, buffer[i]);
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
		suballoc_internal_test();
		if (apicall == 1) done = true; // is vkDestroyInstance
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else if (instrtype == PACKET_BUFFER_UPDATE)
	{
		const uint32_t device_index = t.read_handle();
		const uint32_t buffer_index = t.read_handle();
		buffer_update(t, device_index, buffer_index);
	}
	else assert(false);
	suballoc_internal_test();
	return !done;
}

static void retrace_3()
{
	lava_reader r(TEST_NAME ".vk");
	lava_file_reader& t = r.file_reader(0);
	int remaining = suballoc_internal_test();
	assert(remaining == 0); // there should be nothing now
	while (getnext(t)) {}
	remaining = suballoc_internal_test();
	assert(remaining == 0); // everything should be destroyed now
}

int main()
{
	trace_3();
	retrace_3();
	return 0;
}
