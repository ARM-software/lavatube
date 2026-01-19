#include "tests/common.h"
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"

#define TEST_NAME_1 "tracing_remap_noncoherent"
#define BUFFER_SIZE (1024 * 1024)
#define MAX_VALUES 8

#pragma GCC diagnostic ignored "-Wunused-variable"

struct aligned_range
{
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	VkDeviceSize delta = 0;
};

static VkDeviceSize align_down(VkDeviceSize value, VkDeviceSize align)
{
	return (value / align) * align;
}

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize align)
{
	return ((value + align - 1) / align) * align;
}

static aligned_range make_aligned_range(VkDeviceSize offset, VkDeviceSize size, VkDeviceSize limit, VkDeviceSize atom)
{
	aligned_range range = {};
	if (offset > limit) offset = limit;
	VkDeviceSize end = offset + size;
	if (end > limit) end = limit;
	if (atom <= 1)
	{
		range.offset = offset;
		range.size = end - offset;
		range.delta = 0;
		return range;
	}
	const VkDeviceSize aligned_offset = align_down(offset, atom);
	VkDeviceSize aligned_end = align_up(end, atom);
	if (aligned_end > limit) aligned_end = limit;
	range.offset = aligned_offset;
	range.size = (aligned_end > aligned_offset) ? (aligned_end - aligned_offset) : 0;
	range.delta = offset - aligned_offset;
	return range;
}

static aligned_range map_noncoherent(const vulkan_setup_t& vulkan, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkDeviceSize limit, VkDeviceSize atom, char** out_ptr)
{
	aligned_range range = make_aligned_range(offset, size, limit, atom);
	assert(range.size > 0);
	char* raw = nullptr;
	VkResult result = trace_vkMapMemory(vulkan.device, memory, range.offset, range.size, 0, (void**)&raw);
	check(result);
	*out_ptr = raw + range.delta;
	return range;
}

static void flush_noncoherent(const vulkan_setup_t& vulkan, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkDeviceSize limit, VkDeviceSize atom, VkMarkedOffsetsARM* ar)
{
	aligned_range range = make_aligned_range(offset, size, limit, atom);
	assert(range.size > 0);
	if (ar && ar->count > 0 && ar->pOffsets)
	{
		VkMarkedOffsetsARM adjusted = *ar;
		std::vector<VkDeviceSize> adjusted_offsets(ar->pOffsets, ar->pOffsets + ar->count);
		for (auto& off : adjusted_offsets) off += range.delta;
		adjusted.pOffsets = adjusted_offsets.data();
		testFlushMemory(vulkan, memory, range.offset, range.size, true, &adjusted);
	}
	else
	{
		testFlushMemory(vulkan, memory, range.offset, range.size, true, ar);
	}
}

static bool trace()
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
	check(result);

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(vulkan.device, buf, &req);
	VkPhysicalDeviceMemoryProperties mem_props = {};
	trace_vkGetPhysicalDeviceMemoryProperties(vulkan.physical, &mem_props);
	uint32_t memoryTypeIndex = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
	{
		const VkMemoryPropertyFlags flags = mem_props.memoryTypes[i].propertyFlags;
		if ((req.memoryTypeBits & (1 << i)) == 0) continue;
		if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
		if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) continue;
		memoryTypeIndex = i;
		break;
	}
	if (memoryTypeIndex == UINT32_MAX)
	{
		printf("No non-coherent host-visible memory type available, skipping %s\n", TEST_NAME_1);
		trace_vkDestroyBuffer(vulkan.device, buf, nullptr);
		test_done(vulkan);
		return false;
	}

	const uint32_t align_mod = req.size % req.alignment;
	const uint32_t aligned_size = (align_mod == 0) ? req.size : (req.size + req.alignment - align_mod);
	assert(req.size == aligned_size);

	VkMemoryAllocateInfo pAllocateMemInfo = {};
	pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = aligned_size;
	VkDeviceMemory memory = 0;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
	check(result);
	assert(memory != 0);

	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, nullptr };
	trace_vkGetPhysicalDeviceProperties2(vulkan.physical, &props);
	VkDeviceSize non_coherent_atom = props.properties.limits.nonCoherentAtomSize;
	if (non_coherent_atom == 0) non_coherent_atom = 1;

	test_marker(vulkan, "Bind buffer");
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

	test_marker(vulkan, "Aligned flush at start");
	char* ptr = nullptr;
	map_noncoherent(vulkan, memory, 0, 256, pAllocateMemInfo.allocationSize, non_coherent_atom, &ptr);
	*((uint64_t*)ptr) = address;
	ar.count = 1;
	offsets[0] = 0;
	flush_noncoherent(vulkan, memory, 0, 256, pAllocateMemInfo.allocationSize, non_coherent_atom, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "Unaligned offset flush");
	VkDeviceSize unaligned_offset = (non_coherent_atom > 1) ? (non_coherent_atom / 2) : 1;
	unaligned_offset += 4;
	if (unaligned_offset + 64 >= pAllocateMemInfo.allocationSize) unaligned_offset = 8;
	map_noncoherent(vulkan, memory, unaligned_offset, 64, pAllocateMemInfo.allocationSize, non_coherent_atom, &ptr);
	*((uint64_t*)ptr) = address;
	ar.count = 1;
	offsets[0] = 0;
	flush_noncoherent(vulkan, memory, unaligned_offset, 64, pAllocateMemInfo.allocationSize, non_coherent_atom, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	test_marker(vulkan, "4-byte aligned address");
	map_noncoherent(vulkan, memory, 0, 128, pAllocateMemInfo.allocationSize, non_coherent_atom, &ptr);
	uint64_t* u64ptr = (uint64_t*)(ptr + 4);
	u64ptr[0] = address;
	ar.count = 1;
	offsets[0] = 4;
	flush_noncoherent(vulkan, memory, 0, 128, pAllocateMemInfo.allocationSize, non_coherent_atom, &ar);
	trace_vkUnmapMemory(vulkan.device, memory);

	trace_vkDestroyBuffer(vulkan.device, buf, nullptr);
	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	test_done(vulkan);
	return true;
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
	if (trace()) retrace();
}
