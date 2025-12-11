#include "tests/common.h"
#include "util_auto.h"

#define TEST_NAME_1 "tracing_remap"
#define BUFFER_SIZE (1024 * 1024)

#pragma GCC diagnostic ignored "-Wunused-variable"

static void trace()
{
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

	char* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	uint32_t* u32ptr = (uint32_t*)ptr;
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize / 4; ii++)
	{
		u32ptr[ii] = 0xdeadbeef; // all set to a magic value
	}
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buf);

	// 8-byte-aligned address
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	uint64_t* u64ptr = (uint64_t*)ptr;
	u64ptr[16] = address; // first address, on 8 byte aligned boundary
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buf);

	// 4-byte-aligned address that is _not_ 8 byte aligned
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)(ptr + 4);
	u64ptr[64] = address; // add one more address at non-8-byte-aligned boundary
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buf);

	// no address changes
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	u64ptr[0] = 0;
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buf);

	// remove all addresses
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u32ptr = (uint32_t*)ptr;
	u32ptr[0] = 0xdeadbeef;
	u32ptr[1] = 0xdeadbeef;
	u32ptr[32] = 0xdeadbeef;
	u32ptr[33] = 0xdeadbeef;
	u32ptr[129] = 0xdeadbeef;
	u32ptr[130] = 0xdeadbeef;
	for (unsigned ii = 0; ii < pAllocateMemInfo.allocationSize / 4; ii++)
	{
		assert(u32ptr[ii] == 0xdeadbeef); // all set to the magic value again
	}
	trace_vkUnmapMemory(vulkan.device, memory);
	trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buf);

	// now do it again but with vkUpdateBuffer
	VkUpdateMemoryInfoARM umi = { VK_STRUCTURE_TYPE_UPDATE_MEMORY_INFO_ARM, nullptr };
	//umi.flags = 0;
	umi.dstOffset = 0;
	umi.dataSize = BUFFER_SIZE;
	umi.pData = malloc(BUFFER_SIZE);
	for (uint32_t* p = (uint32_t*)umi.pData; (char*)p < (char*)umi.pData + BUFFER_SIZE; p++) *p = 0xdeadbeef; // all set to a magic value
	trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buf, &umi);

	u64ptr = (uint64_t*)umi.pData;
	u64ptr[16] = address; // first address, on 8 byte aligned boundary
	VkDeviceAddressOffsetsARM ar = { VK_STRUCTURE_TYPE_DEVICE_ADDRESS_OFFSETS_ARM, nullptr };
	ar.count = 1;
	std::vector<uint64_t> offsets(2);
	ar.pOffsets = offsets.data();
	offsets[0] = 16 * sizeof(uint64_t);
	umi.pNext = &ar;
	trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buf, &umi);

	u64ptr = (uint64_t*)((char*)umi.pData + 4);
	u64ptr[64] = address;
	offsets[1] = 64 * sizeof(uint64_t) + 4;
	ar.count = 2;
	trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buf, &umi);

	u64ptr = (uint64_t*)umi.pData;
	u64ptr[0] = 0;
	trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buf, &umi);

	u32ptr = (uint32_t*)umi.pData;
	u32ptr[0] = 0xdeadbeef;
	u32ptr[1] = 0xdeadbeef;
	u32ptr[32] = 0xdeadbeef;
	u32ptr[33] = 0xdeadbeef;
	u32ptr[129] = 0xdeadbeef;
	u32ptr[130] = 0xdeadbeef;
	ar.count = 0;
	trace_vkUpdateBufferTRACETOOLTEST(vulkan.device, buf, &umi);

	free((void*)umi.pData);
	trace_vkDestroyBuffer(vulkan.device, buf, nullptr);
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
}

static int buffer_count = 0;

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == VKDESTROYINSTANCE) done = true; // is vkDestroyInstance
		else if (apicall == VKUPDATEBUFFERTRACETOOLTEST)
		{
			trackedbuffer& buffer_data = VkBuffer_index.at(0); // there is only one
			assert(buffer_count >= 5);
			if (buffer_count == 5)
			{
				assert(buffer_data.candidates.size() == 0);
			}
			else if (buffer_count == 6) // second test: should be one address
			{
				assert(buffer_data.candidates.size() == 1);
				assert(buffer_data.candidate_lookup.size() == 1);
			}
			else if (buffer_count == 7) // third test: should be two addresses
			{
				assert(buffer_data.candidates.size() == 2);
			}
			else if (buffer_count == 8) // fourth test: should still only be two addresses
			{
				assert(buffer_data.candidates.size() == 2);
			}
			else if (buffer_count == 9) // fifth test: should be no addresses again
			{
				assert(buffer_data.candidates.size() == 0);
				assert(buffer_data.candidate_lookup.size() == 0);
			}
			buffer_count++;
		}
	}
	else if (instrtype == PACKET_BUFFER_UPDATE)
	{
		const uint32_t device_index = t.read_handle(DEBUGPARAM("VkDevice"));
		const uint32_t buffer_index = t.read_handle(DEBUGPARAM("VkBuffer"));
		buffer_update(t, device_index, buffer_index);
		trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
		if (buffer_count == 0) // first test: should be no addresses
		{
			assert(buffer_data.candidates.size() == 0);
		}
		else if (buffer_count == 1) // second test: should be one address
		{
			assert(buffer_data.candidates.size() == 1);
			assert(buffer_data.candidate_lookup.size() == 1);
		}
		else if (buffer_count == 2) // third test: should be two addresses
		{
			assert(buffer_data.candidates.size() == 2);
		}
		else if (buffer_count == 3) // fourth test: should still only be two addresses
		{
			assert(buffer_data.candidates.size() == 2);
		}
		else if (buffer_count == 4) // fifth test: should be no addresses again
		{
			assert(buffer_data.candidates.size() == 0);
			assert(buffer_data.candidate_lookup.size() == 0);
		}
		buffer_count++;
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
	r.remap = true;
	lava_file_reader& t = r.file_reader(0);
	int remaining = r.allocator.self_test();
	assert(remaining == 0); // there should be nothing now
	while (getnext(t)) {}
	assert(buffer_count == 10);
	remaining = r.allocator.self_test();
	assert(remaining == 0); // everything should be destroyed now
}

int main()
{
	trace();
	retrace();
	return 0;
}
