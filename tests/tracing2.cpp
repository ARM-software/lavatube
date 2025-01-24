#include "tests/common.h"
#include "util_auto.h"

#define TEST_NAME_1 "tracing_2"
#define NUM_BUFFERS 14
#define BUFFER_SIZE (1024 * 1024)

#pragma GCC diagnostic ignored "-Wunused-variable"

static void trace_3()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init(TEST_NAME_1, reqs);

	PFN_vkVoidFunction badptr = trace_vkGetInstanceProcAddr(nullptr, "vkNonsense");
	assert(!badptr);
	badptr = trace_vkGetInstanceProcAddr(vulkan.instance, "vkNonsense");
	assert(!badptr);
	badptr = trace_vkGetDeviceProcAddr(vulkan.device, "vkNonsense");
	assert(!badptr);
	PFN_vkVoidFunction goodptr = trace_vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
	assert(goodptr);

	std::vector<VkPhysicalDeviceToolPropertiesEXT> tools;
	uint32_t toolCount = 0;
	trace_vkGetPhysicalDeviceToolProperties(vulkan.physical, &toolCount, NULL);
	tools.resize(toolCount);
	printf("%u tools in use:\n", toolCount); // should be 1 for most runs
	trace_vkGetPhysicalDeviceToolProperties(vulkan.physical, &toolCount, tools.data());
	for (VkPhysicalDeviceToolPropertiesEXT &tool : tools)
	{
		printf("\t%s %s\n", tool.name, tool.version);
	}

	VkCommandPool cmdpool;
	VkCommandPoolCreateInfo cmdcreateinfo = {};
	cmdcreateinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdcreateinfo.flags = 0;
	cmdcreateinfo.queueFamilyIndex = 0;
	VkResult result = trace_vkCreateCommandPool(vulkan.device, &cmdcreateinfo, nullptr, &cmdpool);
	check(result);
	test_set_name(vulkan.device, VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)cmdpool, "Our command pool");

	std::vector<VkCommandBuffer> cmdbuffers(10);
	VkCommandBufferAllocateInfo pAllocateInfo = {};
	pAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	pAllocateInfo.commandBufferCount = 10;
	pAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	pAllocateInfo.commandPool = cmdpool;
	pAllocateInfo.pNext = nullptr;
	result = trace_vkAllocateCommandBuffers(vulkan.device, &pAllocateInfo, cmdbuffers.data());
	check(result);
	uint64_t v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuffers[0], VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
	assert(v == 0);
	v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuffers[0], VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
	assert(v == 0);

	VkBuffer buffer[NUM_BUFFERS];
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = BUFFER_SIZE;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[i]);
	}

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[0], &req);
	const uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	const uint32_t align_mod = req.size % req.alignment;
	const uint32_t aligned_size = (align_mod == 0) ? req.size : (req.size + req.alignment - align_mod);
	assert(req.size == aligned_size);

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
	v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
	assert(v == 1);
	v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
	assert(v == pAllocateMemInfo.allocationSize);
	VkMappedMemoryRange flush = {};
	flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	flush.memory = memory;
	flush.offset = 0;
	flush.size = pAllocateMemInfo.allocationSize;
	trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
	trace_vkUnmapMemory(vulkan.device, memory);
	test_set_name(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, "Our memory object");

	VkDeviceSize offset = 0;
	trace_vkBindBufferMemory(vulkan.device, buffer[0], memory, offset); offset += req.size;
	test_set_name(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer[0], "Very temporary buffer");
	trace_vkBindBufferMemory(vulkan.device, buffer[1], memory, offset); offset += req.size;
	trace_vkDestroyBuffer(vulkan.device, buffer[0], nullptr); buffer[0] = VK_NULL_HANDLE;
	trace_vkBindBufferMemory(vulkan.device, buffer[2], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[3], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[4], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[5], memory, offset); offset += req.size;
	trace_vkDestroyBuffer(vulkan.device, buffer[4], nullptr); buffer[4] = VK_NULL_HANDLE;
	trace_vkBindBufferMemory(vulkan.device, buffer[6], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[7], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[8], memory, offset); offset += req.size;
	trace_vkBindBufferMemory(vulkan.device, buffer[9], memory, offset); offset += req.size;
	trace_vkDestroyBuffer(vulkan.device, buffer[9], nullptr); buffer[9] = VK_NULL_HANDLE;
	trace_vkBindBufferMemory(vulkan.device, buffer[10], memory, offset); offset += req.size;
	test_set_name(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer[10], "Buffer 10");
	trace_vkDestroyBuffer(vulkan.device, buffer[1], nullptr); buffer[1] = VK_NULL_HANDLE;

	trace_vkDestroyBuffer(vulkan.device, VK_NULL_HANDLE, nullptr);
	trace_vkDestroyBuffer(vulkan.device, VK_NULL_HANDLE, nullptr);
	trace_vkDestroyBuffer(vulkan.device, VK_NULL_HANDLE, nullptr);

	for (unsigned i = 11; i < NUM_BUFFERS; i++)
	{
		trace_vkBindBufferMemory(vulkan.device, buffer[i], memory, offset);
		offset += req.size;
	}

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		if (buffer[i] == VK_NULL_HANDLE) continue;
		trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buffer[i]);
	}

	VkDescriptorSetLayoutCreateInfo cdslayout = {};
	cdslayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	cdslayout.bindingCount = 1;
	VkDescriptorSetLayoutBinding dslb = {};
	dslb.binding = 0;
	dslb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	dslb.descriptorCount = 10;
	dslb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	dslb.pImmutableSamplers = VK_NULL_HANDLE;
	cdslayout.pBindings = &dslb;
	VkDescriptorSetLayout dslayout;
	result = trace_vkCreateDescriptorSetLayout(vulkan.device, &cdslayout, nullptr, &dslayout);
	assert(result == VK_SUCCESS);

	VkDescriptorPoolCreateInfo cdspool = {};
	cdspool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	cdspool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	cdspool.maxSets = 500;
	cdspool.poolSizeCount = 1;
	VkDescriptorPoolSize dps = {};
	dps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	dps.descriptorCount = 1;
	cdspool.pPoolSizes = &dps;;
	VkDescriptorPool pool;
	result = trace_vkCreateDescriptorPool(vulkan.device, &cdspool, nullptr, &pool);
	assert(result == VK_SUCCESS);
	trace_vkResetDescriptorPool(vulkan.device, pool, 0);

	// Cleanup...
	trace_vkDestroyDescriptorPool(vulkan.device, pool, nullptr);
	trace_vkDestroyDescriptorSetLayout(vulkan.device, dslayout, nullptr);
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		if (buffer[i] == VK_NULL_HANDLE) continue;
		test_destroy_buffer(vulkan, (req.size * i) % 255, memory, buffer[i], req.size * i, BUFFER_SIZE);
	}

	trace_vkFreeMemory(vulkan.device, memory, nullptr);
	trace_vkFreeCommandBuffers(vulkan.device, cmdpool, cmdbuffers.size(), cmdbuffers.data());
	trace_vkDestroyCommandPool(vulkan.device, cmdpool, nullptr);

	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == 1) done = true; // is vkDestroyInstance
	}
	else if (instrtype == PACKET_BUFFER_UPDATE)
	{
		const uint32_t device_index = t.read_handle();
		const uint32_t buffer_index = t.read_handle();
		buffer_update(t, device_index, buffer_index);
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else assert(false);
	suballoc_internal_test();
	return !done;
}

static std::atomic_bool triggered_VkCreateInstance_callback { false };
static void my_VkCreateInstance_callback(lava_file_reader& reader, VkResult result, const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
	assert(result == VK_SUCCESS);
	assert(triggered_VkCreateInstance_callback.load() == false);
	triggered_VkCreateInstance_callback.store(true);
}

static void retrace_3()
{
	lava_reader r(TEST_NAME_1 ".vk");
	lava_file_reader& t = r.file_reader(0);
	int remaining = suballoc_internal_test();
	assert(remaining == 0); // there should be nothing now

	// set up callbacks
	vkCreateInstance_callbacks.push_back(my_VkCreateInstance_callback);

	while (getnext(t)) {}
	remaining = suballoc_internal_test();
	assert(remaining == 0); // everything should be destroyed now
	assert(triggered_VkCreateInstance_callback.load());
}

int main()
{
	trace_3();
	retrace_3();
	return 0;
}
