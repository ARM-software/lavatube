// Test of VK_ARM_trace_helpers support for vkAssertMemoryARM

#include "tests/common.h"
#include <inttypes.h>
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"

#define TEST_NAME "tracing_memory"
#define NUM_BUFFERS 3

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

static int count_flushes = 0;
static int count_asserts = 0;

static void trace()
{
	p__trust_host_flushes = 1;

	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_3;
	reqs.reqfeat12.bufferDeviceAddress = VK_TRUE;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result;

	PFN_vkAssertMemoryARM vkAssertMemory = (PFN_vkAssertMemoryARM)trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertMemoryARM");
	assert(vkAssertMemory != nullptr);

	const VkDeviceSize buffer_sizes[NUM_BUFFERS] = { 99, 4097, 257 };
	VkBuffer buffers[NUM_BUFFERS] = {};
	VkMemoryRequirements reqs_mem[NUM_BUFFERS] = {};
	VkDeviceSize aligned_sizes[NUM_BUFFERS] = {};
	VkDeviceSize offsets[NUM_BUFFERS] = {};

	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		buffer_info.size = buffer_sizes[i];
		result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &buffers[i]);
		check(result);
		trace_vkGetBufferMemoryRequirements(vulkan.device, buffers[i], &reqs_mem[i]);
		const VkDeviceSize align_mod = reqs_mem[i].size % reqs_mem[i].alignment;
		aligned_sizes[i] = (align_mod == 0) ? reqs_mem[i].size : (reqs_mem[i].size + reqs_mem[i].alignment - align_mod);
	}

	const uint32_t memory_type_index = get_device_memory_type(reqs_mem[0].memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	for (unsigned i = 1; i < NUM_BUFFERS; i++)
	{
		assert(memory_type_index == get_device_memory_type(reqs_mem[i].memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	}

	VkMemoryAllocateFlagsInfo alloc_flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr };
	alloc_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;

	VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &alloc_flags };
	alloc_info.memoryTypeIndex = memory_type_index;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		offsets[i] = alloc_info.allocationSize;
		alloc_info.allocationSize += aligned_sizes[i];
	}

	VkDeviceMemory memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &alloc_info, nullptr, &memory);
	check(result);
	assert(memory != VK_NULL_HANDLE);

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		result = trace_vkBindBufferMemory(vulkan.device, buffers[i], memory, offsets[i]);
		check(result);
	}

	VkDeviceAddress addresses[NUM_BUFFERS] = {};
	VkBufferDeviceAddressInfo address_info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr };
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		address_info.buffer = buffers[i];
		addresses[i] = trace_vkGetBufferDeviceAddress(vulkan.device, &address_info);
		assert(addresses[i] != 0);
	}

	char* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, memory, 0, alloc_info.allocationSize, 0, (void**)&ptr);
	check(result);
	for (VkDeviceSize i = 0; i < alloc_info.allocationSize; i++)
	{
		ptr[i] = (char)((i * 17 + 5) % 251);
	}

	const VkDeviceSize partial_offset = 33;
	const VkDeviceSize partial_size = 1234;
	const uint32_t full_checksum = adler32((unsigned char*)(ptr + offsets[0]), buffer_sizes[0]);
	const uint32_t partial_checksum = adler32((unsigned char*)(ptr + offsets[1] + partial_offset), partial_size);
	const uint32_t empty_checksum = adler32(nullptr, 0);

	VkMappedMemoryRange flush = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr };
	flush.memory = memory;
	flush.offset = 0;
	flush.size = alloc_info.allocationSize;
	trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
	trace_vkUnmapMemory(vulkan.device, memory);

	uint32_t checksum = 0;
	VkDeviceAddressRangeKHR full_range = { addresses[0], buffer_sizes[0] };
	VkUpdateMemoryInfoARM full_info = make_memory_update_info(&full_range, VK_WHOLE_SIZE, nullptr);
	result = trace_vkAssertMemoryARM(vulkan.device, &full_info, &checksum, "full buffer");
	check(result);
	assert(checksum == full_checksum);

	VkDeviceAddressRangeKHR partial_range = { addresses[1] + partial_offset, partial_size };
	VkUpdateMemoryInfoARM partial_info = make_memory_update_info(&partial_range, partial_size, nullptr);
	result = trace_vkAssertMemoryARM(vulkan.device, &partial_info, &checksum, "partial buffer");
	check(result);
	assert(checksum == partial_checksum);

	VkDeviceAddressRangeKHR empty_range = { addresses[2], 0 };
	VkUpdateMemoryInfoARM empty_info = make_memory_update_info(&empty_range, 0, nullptr);
	result = trace_vkAssertMemoryARM(vulkan.device, &empty_info, &checksum, "empty range");
	check(result);
	assert(checksum == empty_checksum);

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkDestroyBuffer(vulkan.device, buffers[i], nullptr);
	}
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.step();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == VKASSERTMEMORYARM) count_asserts++;
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
	lava_reader r(TEST_NAME ".api");
	test_register_replay_callbacks();
	lava_file_reader& t = r.file_reader(0);
	while (getnext(t)) {}
	assert(count_flushes == NUM_BUFFERS);
	assert(count_asserts == 3);
}

int main()
{
	trace();
	retrace();
	return 0;
}
