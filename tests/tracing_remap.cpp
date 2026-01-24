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
	VkBuffer buf2;
	VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	bufferCreateInfo.size = BUFFER_SIZE;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buf);
	check(result);
	result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buf2);
	check(result);

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(vulkan.device, buf, &req);
	const uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	const uint32_t align_mod = req.size % req.alignment;
	const uint32_t aligned_size = (align_mod == 0) ? req.size : (req.size + req.alignment - align_mod);
	assert(req.size == aligned_size);

	VkMemoryAllocateInfo pAllocateMemInfo = {};
	pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = aligned_size * 2;
	VkDeviceMemory memory = 0;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
	check(result);
	assert(memory != 0);

	test_marker(vulkan, "Bind buffers");
	trace_vkBindBufferMemory(vulkan.device, buf, memory, 0);
	trace_vkBindBufferMemory(vulkan.device, buf2, memory, aligned_size);

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

	test_marker(vulkan, "Device address at first byte - " + std::to_string(address));
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

	test_marker(vulkan, "Device address at bytes 32 and 128"); // 8-byte-aligned address
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	uint64_t* u64ptr = (uint64_t*)ptr;
	u64ptr[4] = address; // second address, on 8 byte aligned boundary
	u64ptr[16] = address; // second address, on 8 byte aligned boundary
	ar.count = 2; // we only changed one value
	offsets[0] = 4 * sizeof(uint64_t); // and it is here
	offsets[1] = 16 * sizeof(uint64_t); // and it is here too
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Partial flush");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	const VkDeviceSize flush_offset = 64 * sizeof(uint64_t);
	const VkDeviceSize flush_size = 32 * sizeof(uint64_t);
	const VkDeviceSize marked_offset = flush_offset + (4 * sizeof(uint64_t));
	u64ptr[marked_offset / sizeof(uint64_t)] = address;
	ar.count = 1;
	offsets[0] = marked_offset - flush_offset;
	testFlushMemory(vulkan, memory, flush_offset, flush_size, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Flush with mapped offset");
	result = trace_vkMapMemory(vulkan.device, memory, 192, VK_WHOLE_SIZE, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	*u64ptr = address;
	ar.count = 1;
	offsets[0] = 0;
	testFlushMemory(vulkan, memory, 192, 64, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Flush with mapped offset and limited window");
	result = trace_vkMapMemory(vulkan.device, memory, 200, 64, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[1] = address;
	ar.count = 1;
	offsets[0] = 8;
	testFlushMemory(vulkan, memory, 200, VK_WHOLE_SIZE, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Flush with out of bounds offset");
	result = trace_vkMapMemory(vulkan.device, memory, 8, 64, 0, (void**)&ptr);
	check(result);
	ar.count = 1;
	offsets[0] = UINT32_MAX;
	testFlushMemory(vulkan, memory, 8, VK_WHOLE_SIZE, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Device address on a 4 byte alignment");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)(ptr + 4);
	u64ptr[64] = address; // add one more address at non-8-byte-aligned boundary
	ar.count = 1; // we still only changed one value
	offsets[0] = 64 * sizeof(uint64_t) + 4; // and it is here
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Device address in a second buffer and flush covering both");
	result = trace_vkMapMemory(vulkan.device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)(ptr + aligned_size);
	u64ptr[0] = address;
	u64ptr[1] = address;
	ar.count = 2;
	offsets[0] = aligned_size;
	offsets[1] = aligned_size + 8;
	testFlushMemory(vulkan, memory, 0, VK_WHOLE_SIZE, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "No address changes, empty address list sent");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[10] = 0;
	ar.count = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "No address changes, no address list sent");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[20] = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, true, nullptr);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "No address changes, just flush");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[21] = 0;
	testFlushMemory(vulkan, memory, 0, pAllocateMemInfo.allocationSize, false, nullptr);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "No address changes, no flush");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[22] = 0;
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Reset buffer back to dead pattern and flush");
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
	return !done;
}

static void retrace()
{
	lava_reader r(TEST_NAME_1 ".vk");
	r.remap_scan = true;
	lava_file_reader& t = r.file_reader(0);
	while (getnext(t)) {}
}

int main()
{
	trace();
	retrace();
	return 0;
}
