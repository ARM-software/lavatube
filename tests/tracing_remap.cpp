#include "tests/common.h"
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"

#define TEST_NAME_1 "tracing_remap"
#define BUFFER_SIZE (1024 * 1024)
#define MAX_VALUES 10

#pragma GCC diagnostic ignored "-Wunused-variable"

static void trace()
{
	// Set before device creation to make sure it propagates everywhere.
	p__trust_host_flushes = 1;

	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_3;
	reqs.reqfeat12.bufferDeviceAddress = VK_TRUE;
	vulkan_setup_t vulkan = test_init(TEST_NAME_1, reqs);

	VkBuffer buf;
	VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	bufferCreateInfo.size = BUFFER_SIZE;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buf);

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(vulkan.device, buf, &req);
	const uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	const uint32_t align_mod = req.size % req.alignment;
	const uint32_t aligned_size = (align_mod == 0) ? req.size : (req.size + req.alignment - align_mod);
	assert(req.size == aligned_size);

	VkMemoryAllocateInfo pAllocateMemInfo = {};
	pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = req.size;
	VkDeviceMemory memory = 0;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
	check(result);
	assert(memory != 0);

	trace_vkBindBufferMemory(vulkan.device, buf, memory, 0);

	VkBufferDeviceAddressInfo bdainfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr };
	bdainfo.buffer = buf;
	VkDeviceAddress address = trace_vkGetBufferDeviceAddress(vulkan.device, &bdainfo);
	assert(address != 0);

	VkMarkedOffsetsARM ar = { VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM, nullptr };
	std::vector<VkDeviceSize> offsets(MAX_VALUES, 0);
	std::vector<VkMarkingTypeARM> markingTypes(MAX_VALUES, VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	std::vector<VkMarkingSubTypeARM> subTypes(MAX_VALUES, { .deviceAddressType = VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM });
	ar.count = 0;
	ar.pOffsets = offsets.data();
	ar.pMarkingTypes = markingTypes.data();
	ar.pSubTypes = subTypes.data();

	char* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	uint32_t* u32ptr = (uint32_t*)ptr;
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize / 4; ii++)
	{
		u32ptr[ii] = 0xdeadbeef; // all set to a magic value
	}
	*((uint64_t*)ptr) = address; // first address at start of buffer
	ar.count = 1;
	offsets[0] = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	// 8-byte-aligned address
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	uint64_t* u64ptr = (uint64_t*)ptr;
	u64ptr[16] = address; // second address, on 8 byte aligned boundary
	ar.count = 1; // we only changed one value
	offsets[0] = 16 * sizeof(uint64_t); // and it is here
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	// 4-byte-aligned address that is _not_ 8 byte aligned
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)(ptr + 4);
	u64ptr[64] = address; // add one more address at non-8-byte-aligned boundary
	ar.count = 1; // we still only changed one value
	offsets[0] = 64 * sizeof(uint32_t); // and it is here
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	// no address changes -- send empty address list
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[10] = 0;
	ar.count = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	// no address changes -- no address list
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[20] = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, nullptr);
	trace_vkUnmapMemory(vulkan.device, memory);

	// no address changes -- no info bit, just flush
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[21] = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, false, nullptr);
	trace_vkUnmapMemory(vulkan.device, memory);

	// no address changes -- no flush even
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[22] = 0;
	trace_vkUnmapMemory(vulkan.device, memory);

	// remove all addresses -- notice their removal if we scan for them
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u32ptr = (uint32_t*)ptr;
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize / 4; ii++)
	{
		u32ptr[ii] = 0xdeadbeef; // all set to the magic value again
	}
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, nullptr);
	trace_vkUnmapMemory(vulkan.device, memory);

	trace_vkDestroyBuffer(vulkan.device, buf, nullptr);
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
		if (apicall == VKDESTROYINSTANCE) done = true; // is vkDestroyInstance
	}
	else if (instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_IMAGE_UPDATE2)
	{
		assert(false);
		update_image_packet(instrtype, t);
	}
	else if (instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_BUFFER_UPDATE2)
	{
		uint32_t buffer_index = update_buffer_packet(instrtype, t);
		trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
		(void)buffer_data; // TBD test data sanity here
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		assert(false);
		update_tensor_packet(instrtype, t);
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else assert(false);
	t.parent->allocator.self_test();
	return !done;
}

static void retrace()
{
	lava_reader r(TEST_NAME_1 ".vk");
	r.remap_scan = true;
	lava_file_reader& t = r.file_reader(0);
	int remaining = r.allocator.self_test();
	assert(remaining == 0); // there should be nothing now
	while (getnext(t)) {}
	remaining = r.allocator.self_test();
	assert(remaining == 0); // everything should be destroyed now
}

int main()
{
	trace();
	retrace();
	return 0;
}
