#include "tests/common.h"
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"
#include <cstring>

#define TEST_NAME_1 "tracing_remap_noncoherent"
#define BUFFER_SIZE (1024 * 1024)
#define MAX_VALUES 8

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

static VkDeviceAddress capture_address = 0;
static VkDeviceSize capture_address_offset = 40;
static VkDeviceSize unaligned_address_location = 0;
static VkDeviceSize capture_address_offset_location = 0;
static std::string replay_marker;

static trackedbuffer& replay_address_buffer()
{
	for (auto& buffer : VkBuffer_index)
	{
		if (buffer.capture_device_address == capture_address) return buffer;
	}
	ABORT("Failed to find replay buffer for capture address %llu", (unsigned long long)capture_address);
	__builtin_unreachable();
}

static void record_replay_marker(callback_context& cb, VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData)
{
	(void)cb;
	(void)instance;
	(void)messageSeverity;
	(void)messageTypes;
	if (!pCallbackData || !pCallbackData->pMessage) return;
	replay_marker = pCallbackData->pMessage;
}

static suballoc_location replay_buffer_location(const trackedbuffer& buffer)
{
	const trackeddevice& device_data = VkDevice_index.at(buffer.parent_device_index);
	return device_data.allocator->find_buffer_memory(buffer.index);
}

static char* map_replay_buffer(const trackedbuffer& buffer, VkDevice& device, suballoc_location& loc, bool& temporary_map)
{
	device = index_to_VkDevice.at(buffer.parent_device_index);
	loc = replay_buffer_location(buffer);
	char* ptr = loc.mapped;
	temporary_map = false;
	if (ptr == nullptr)
	{
		VkResult result = wrap_vkMapMemory(device, loc.memory, loc.offset, loc.size, 0, (void**)&ptr);
		check(result);
		temporary_map = true;
	}
	return ptr;
}

static void unmap_replay_buffer(VkDevice device, const suballoc_location& loc, bool temporary_map)
{
	if (temporary_map) wrap_vkUnmapMemory(device, loc.memory);
}

static uint64_t load_u64(const char* ptr, VkDeviceSize offset)
{
	uint64_t value = 0;
	memcpy(&value, ptr + offset, sizeof(value));
	return value;
}

static void verify_replay_buffer_update(lava_file_reader& t, uint32_t buffer_index)
{
	trackedbuffer& address_buffer = replay_address_buffer();
	trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
	assert(&buffer_data == &address_buffer);
	const VkDeviceAddress replay_address = t.parent->device_address_remapping.translate_address(capture_address);
	assert(replay_address != 0);
	assert(replay_address == address_buffer.device_address);
	assert(t.parent->device_address_remapping.get_by_address(capture_address) == &address_buffer);
	assert(t.parent->device_address_remapping.translate_address(capture_address + capture_address_offset) == replay_address + capture_address_offset);
	assert(t.parent->device_address_remapping.get_by_address(capture_address + capture_address_offset) == &address_buffer);

	VkDevice device = VK_NULL_HANDLE;
	suballoc_location loc = {};
	bool temporary_map = false;
	char* ptr = map_replay_buffer(buffer_data, device, loc, temporary_map);

	if (replay_marker == "Aligned flush at start")
	{
		assert(load_u64(ptr, 0) == replay_address);
	}
	else if (replay_marker == "Unaligned offset flush")
	{
		assert(load_u64(ptr, unaligned_address_location) == replay_address);
	}
	else if (replay_marker == "4-byte aligned address")
	{
		assert(load_u64(ptr, 4) == replay_address);
	}
	else if (replay_marker == "Device address with non-zero in-buffer offset")
	{
		assert(load_u64(ptr, capture_address_offset_location) == replay_address + capture_address_offset);
	}
	else
	{
		ABORT("Unexpected replay marker while validating noncoherent remap test: %s", replay_marker.c_str());
	}

	unmap_replay_buffer(device, loc, temporary_map);
}

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
	capture_address = address;

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
	unaligned_address_location = unaligned_offset;
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

	test_marker(vulkan, "Device address with non-zero in-buffer offset");
	VkDeviceSize offset_location = (non_coherent_atom > 1) ? (non_coherent_atom / 2) : 3;
	offset_location += 16;
	if (offset_location + 64 >= pAllocateMemInfo.allocationSize) offset_location = 32;
	map_noncoherent(vulkan, memory, offset_location, 64, pAllocateMemInfo.allocationSize, non_coherent_atom, &ptr);
	uint64_t* offset_ptr = (uint64_t*)(ptr + 4);
	offset_ptr[0] = address + capture_address_offset;
	capture_address_offset_location = offset_location + 4;
	ar.count = 1;
	offsets[0] = 4;
	flush_noncoherent(vulkan, memory, offset_location, 64, pAllocateMemInfo.allocationSize, non_coherent_atom, &ar);
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
		verify_replay_buffer_update(t, buffer_index);
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
	test_register_replay_callbacks();
	vkSubmitDebugUtilsMessageEXT_callbacks.push_back(record_replay_marker);
	replay_marker.clear();
	lava_file_reader& t = r.file_reader(0);
	while (getnext(t)) {}
	assert(r.device_address_remapping.translate_address(capture_address) == 0);
	assert(r.device_address_remapping.get_by_address(capture_address) == nullptr);
}

int main()
{
	if (trace()) retrace();
}
