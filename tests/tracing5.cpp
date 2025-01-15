#include "tests/common.h"
#include <inttypes.h>
#include "util_auto.h"

#define TEST_NAME_1 "tracing_5"
#define NUM_BUFFERS 3

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

typedef uint32_t (VKAPI_PTR *PFN_vkAssertBufferTRACETOOLTEST)(VkDevice device, VkBuffer buffer);
typedef void (VKAPI_PTR *PFN_vkSyncBufferTRACETOOLTEST)(VkDevice device, VkBuffer buffer);

static void trace_3()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init(TEST_NAME_1, reqs);
	VkResult result;

	PFN_vkAssertBufferTRACETOOLTEST vkAssertBuffer = (PFN_vkAssertBufferTRACETOOLTEST)trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertBufferTRACETOOLTEST");
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
	char* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize; ii++) ptr[ii] = ii % 255; // just fill it with a pattern
	VkMappedMemoryRange flush = {};
	flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	flush.memory = memory;
	flush.offset = 0;
	flush.size = pAllocateMemInfo.allocationSize;
	trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
	std::vector<uint32_t> checksums(NUM_BUFFERS);
	VkDeviceSize offset = 0;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		checksums.at(i) = adler32((unsigned char*)(ptr + offset), bufferCreateInfo.size);
		offset += req.size;
	}
	trace_vkUnmapMemory(vulkan.device, memory);

	offset = 0;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkBindBufferMemory(vulkan.device, buffer[i], memory, offset);
		offset += req.size;
	}

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		uint32_t checksum = trace_vkAssertBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		assert(checksum == checksums[i]);

		const uint64_t updates = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer[i], VK_TRACING_OBJECT_PROPERTY_UPDATES_COUNT_TRACETOOLTEST);
		const uint64_t bytes = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer[i], VK_TRACING_OBJECT_PROPERTY_UPDATES_BYTES_TRACETOOLTEST);
		printf("buffer %u updated %" PRIu64 " times for %" PRIu64 " bytes\n", i, updates, bytes);
	}
	VkDeviceMemory backingmem = (VkDeviceMemory)trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer[0], VK_TRACING_OBJECT_PROPERTY_BACKING_STORE_TRACETOOLTEST);
	assert(backingmem == memory);

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
	if (instrtype == PACKET_API_CALL)
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
	lava_reader r(TEST_NAME_1 ".vk");
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
