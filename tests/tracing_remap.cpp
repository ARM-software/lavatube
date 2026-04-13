#include "tests/common.h"
#include "util_auto.h"
#include "external/tracetooltests/include/vulkan_ext.h"
#include <cstring>

#define TEST_NAME_1 "tracing_remap"
#define BUFFER_SIZE (1024 * 1024)
#define MAX_VALUES 10

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

static VkDeviceAddress capture_address = 0;
static VkDeviceSize capture_address_offset = 96;
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

static uint32_t load_u32(const char* ptr, VkDeviceSize offset)
{
	uint32_t value = 0;
	memcpy(&value, ptr + offset, sizeof(value));
	return value;
}

static bool marker_starts_with(const char* prefix)
{
	return replay_marker.rfind(prefix, 0) == 0;
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

static void verify_replay_buffer_update(lava_file_reader& t, uint32_t buffer_index)
{
	trackedbuffer& address_buffer = replay_address_buffer();
	trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
	const bool is_address_buffer = (&buffer_data == &address_buffer);
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

	if (marker_starts_with("Device address at first byte"))
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 0) == replay_address);
			assert(load_u32(ptr, sizeof(uint64_t)) == 0xdeadbeef);
		}
		else
		{
			assert(load_u32(ptr, 0) == 0xdeadbeef);
			assert(load_u32(ptr, 4) == 0xdeadbeef);
		}
	}
	else if (replay_marker == "Device address at bytes 32 and 128")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 4 * sizeof(uint64_t)) == replay_address);
			assert(load_u64(ptr, 16 * sizeof(uint64_t)) == replay_address);
		}
		else
		{
			assert(load_u32(ptr, 0) == 0xdeadbeef);
		}
	}
	else if (replay_marker == "Partial flush")
	{
		assert(is_address_buffer);
		assert(load_u64(ptr, (64 * sizeof(uint64_t)) + (4 * sizeof(uint64_t))) == replay_address);
	}
	else if (replay_marker == "Flush with mapped offset")
	{
		assert(is_address_buffer);
		assert(load_u64(ptr, 192) == replay_address);
	}
	else if (replay_marker == "Flush with mapped offset and limited window")
	{
		assert(is_address_buffer);
		assert(load_u64(ptr, 208) == replay_address);
	}
	else if (replay_marker == "Flush with out of bounds offset")
	{
		assert(is_address_buffer);
		assert(load_u32(ptr, 8) == 0xdeadbeef);
	}
	else if (replay_marker == "Device address on a 4 byte alignment")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, (64 * sizeof(uint64_t)) + 4) == replay_address);
		}
	}
	else if (replay_marker == "Device address with non-zero in-buffer offset")
	{
		assert(is_address_buffer);
		assert(load_u64(ptr, capture_address_offset_location) == replay_address + capture_address_offset);
	}
	else if (replay_marker == "Device address in a second buffer and flush covering both")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 0) == replay_address);
		}
		else
		{
			assert(load_u64(ptr, 0) == replay_address);
			assert(load_u64(ptr, sizeof(uint64_t)) == replay_address);
		}
	}
	else if (replay_marker == "No address changes, empty address list sent")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 10 * sizeof(uint64_t)) == 0);
		}
	}
	else if (replay_marker == "No address changes, no address list sent")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 20 * sizeof(uint64_t)) == 0);
		}
	}
	else if (replay_marker == "No address changes, just flush")
	{
		if (is_address_buffer)
		{
			assert(load_u64(ptr, 21 * sizeof(uint64_t)) == 0);
		}
	}
	else if (replay_marker == "Reset buffer back to dead pattern and flush")
	{
		assert(load_u32(ptr, 0) == 0xdeadbeef);
		assert(load_u32(ptr, 4 * sizeof(uint64_t)) == 0xdeadbeef);
		if (is_address_buffer)
		{
			assert(load_u32(ptr, capture_address_offset_location) == 0xdeadbeef);
			assert(load_u32(ptr, (64 * sizeof(uint64_t)) + 4) == 0xdeadbeef);
		}
		else
		{
			assert(load_u32(ptr, sizeof(uint64_t)) == 0xdeadbeef);
		}
	}
	else
	{
		ABORT("Unexpected replay marker while validating remap test: %s", replay_marker.c_str());
	}

	unmap_replay_buffer(device, loc, temporary_map);
}

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
	capture_address = address;

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

	test_marker(vulkan, "Device address with non-zero in-buffer offset");
	result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
	check(result);
	u64ptr = (uint64_t*)ptr;
	capture_address_offset_location = 40 * sizeof(uint64_t);
	u64ptr[capture_address_offset_location / sizeof(uint64_t)] = address + capture_address_offset;
	ar.count = 1;
	offsets[0] = 0;
	testFlushMemory(vulkan, memory, capture_address_offset_location, sizeof(uint64_t), true, &ar);
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
	trace();
	retrace();
	return 0;
}
