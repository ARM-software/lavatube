// Unit test to try out various combinations of GPU memory copying and synchronization with CPU-side
// memory mapping.

#include "tests/common.h"
#include "util_auto.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

static int spurious_checks = 0;
static int queue_variant = 0;
static int map_variant = 0;
static int fence_variant = 0;
static unsigned buffer_size = (32 * 1024);
static unsigned num_buffers = 10;
static int flush_variant = 0;

static std::string filename()
{
	const char* outpath = getenv("LAVATUBE_DESTINATION");
	if (outpath) return outpath;
	return "tracing_4_q" + _to_string(queue_variant) + "_m" + _to_string(map_variant) + "_F" + _to_string(flush_variant);
}

static void usage()
{
	printf("Usage: tracing4\n");
	printf("-h/--help              This help\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-b/--buffer-size       Set buffer size (default 32mb)\n");
	printf("-c/--buffer-count      Set buffer count (default 10)\n");
	printf("-H/--heap-size         Set suballocator heap size\n");
	printf("-f/--fence-variant     Set fence variant (default 0)\n");
	printf("\t0 - use vkWaitForFences\n");
	printf("\t1 - use vkGetFenceStatus\n");
	printf("-q/--queue-variant     Set queue variant (default 0)\n");
	printf("\t0 - many commandbuffers, many queue submit calls, many flushes, wait for fence after each submit\n");
	printf("\t1 - many commandbuffers, one queue submit call with many submits, one flush\n");
	printf("\t2 - many commandbuffers, one queue submit, one flush\n");
	printf("\t3 - one commandbuffer, one queue submit, no flush\n");
	printf("\t4 - many commandbuffers, many queue submit calls, wait for all fences after all submits\n");
	printf("-m/--map-variant       Set map variant (default %d)\n", map_variant);
	printf("\t0 - memory map kept open\n");
	printf("\t1 - memory map unmapped before submit\n");
	printf("\t2 - memory map remapped to tiny area before submit\n");
	printf("-F/--flush-variant     Set flush variant (default %d)\n", flush_variant);
	printf("\t0 - explicit memory flushing\n");
	printf("\t1 - no explicit memory flushing\n");
	exit(-1);
}

static bool match(const char* in, const char* short_form, const char* long_form, int& remaining)
{
	if (strcmp(in, short_form) == 0 || strcmp(in, long_form) == 0)
	{
		remaining--;
		return true;
	}
	return false;
}

static int get_int(const char* in, int& remaining)
{
	if (remaining == 0)
	{
		usage();
	}
	remaining--;
	return atoi(in);
}

static void waitfence(vulkan_setup_t& vulkan, VkFence fence)
{
	if (fence_variant == 0)
	{
		VkResult result = trace_vkWaitForFences(vulkan.device, 1, &fence, VK_TRUE, UINT32_MAX);
		check(result);
	}
	else if (fence_variant == 1)
	{
		VkResult result = VK_NOT_READY;
		do
		{
			result = trace_vkGetFenceStatus(vulkan.device, fence);
			assert(result != VK_ERROR_DEVICE_LOST);
			if (result != VK_SUCCESS) usleep(10);
		} while (result != VK_SUCCESS);
	}
}

static void trace_4()
{
	vulkan_req_t reqs;
	std::string name = filename();
	printf("Creating %s.vk\n", name.c_str());
	vulkan_setup_t vulkan = test_init(name, reqs);
	VkResult result;

	VkQueue queue;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);

	std::vector<VkBuffer> origin_buffers(num_buffers);
	std::vector<VkBuffer> target_buffers(num_buffers);
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = buffer_size;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBufferCreateInfo bufferCreateInfo2 = bufferCreateInfo;
	bufferCreateInfo2.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	for (unsigned i = 0; i < num_buffers; i++)
	{
		result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &origin_buffers.at(i));
		assert(result == VK_SUCCESS);
		result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo2, nullptr, &target_buffers.at(i));
		assert(result == VK_SUCCESS);
	}

	VkMemoryRequirements memory_requirements;
	trace_vkGetBufferMemoryRequirements(vulkan.device, origin_buffers.at(0), &memory_requirements);
	const uint32_t memoryTypeIndex = get_device_memory_type(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	const uint32_t align_mod = memory_requirements.size % memory_requirements.alignment;
	const uint32_t aligned_size = (align_mod == 0) ? memory_requirements.size : (memory_requirements.size + memory_requirements.alignment - align_mod);

	VkMemoryAllocateInfo pAllocateMemInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = aligned_size * num_buffers;
	VkDeviceMemory origin_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &origin_memory);
	assert(result == VK_SUCCESS);
	assert(origin_memory != VK_NULL_HANDLE);
	VkDeviceMemory target_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &target_memory);
	assert(result == VK_SUCCESS);
	assert(target_memory != VK_NULL_HANDLE);

	VkDeviceSize offset = 0;
	for (unsigned i = 0; i < num_buffers; i++)
	{
		trace_vkBindBufferMemory(vulkan.device, origin_buffers.at(i), origin_memory, offset);
		trace_vkBindBufferMemory(vulkan.device, target_buffers.at(i), target_memory, offset);
		offset += aligned_size;
	}

	char* data = nullptr;
	result = trace_vkMapMemory(vulkan.device, origin_memory, 0, num_buffers * aligned_size, 0, (void**)&data);
	assert(result == VK_SUCCESS);
	offset = 0;
	for (unsigned i = 0; i < num_buffers; i++)
	{
		memset(data + offset, i, aligned_size);
		offset += aligned_size;
	}
	if (map_variant == 1 || map_variant == 2) trace_vkUnmapMemory(vulkan.device, origin_memory);
	if (map_variant == 2) trace_vkMapMemory(vulkan.device, origin_memory, 10, 20, 0, (void**)&data);

	VkCommandPoolCreateInfo command_pool_create_info = {};
	command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool_create_info.queueFamilyIndex = 0; // TBD

	VkCommandPool command_pool;
	result = trace_vkCreateCommandPool(vulkan.device, &command_pool_create_info, NULL, &command_pool);
	check(result);

	VkCommandBufferAllocateInfo command_buffer_allocate_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
	command_buffer_allocate_info.commandPool = command_pool;
	command_buffer_allocate_info.commandBufferCount = num_buffers + 1;
	std::vector<VkCommandBuffer> command_buffers(num_buffers + 1);
	result = trace_vkAllocateCommandBuffers(vulkan.device, &command_buffer_allocate_info, command_buffers.data());
	check(result);
	VkCommandBufferBeginInfo command_buffer_begin_info = {};
	command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VkFence fence;
	VkFenceCreateInfo fence_create_info = {};
	fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = trace_vkCreateFence(vulkan.device, &fence_create_info, NULL, &fence);
	check(result);
	VkMemoryBarrier memory_barrier = {};
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
	// Many commands
	for (unsigned i = 0; i < num_buffers; i++)
	{
		result = trace_vkBeginCommandBuffer(command_buffers[i], &command_buffer_begin_info);
		check(result);
		VkBufferCopy region;
		region.srcOffset = 0;
		region.dstOffset = 0;
		region.size = buffer_size;
		trace_vkCmdCopyBuffer(command_buffers[i], origin_buffers[i], target_buffers[i], 1, &region);
		trace_vkCmdPipelineBarrier(command_buffers[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
		result = trace_vkEndCommandBuffer(command_buffers[i]);
		check(result);
	}
	// Single command
	result = trace_vkBeginCommandBuffer(command_buffers.at(num_buffers), &command_buffer_begin_info);
	check(result);
	for (unsigned i = 0; i < num_buffers; i++)
	{
		VkBufferCopy region;
		region.srcOffset = 0;
		region.dstOffset = 0;
		region.size = buffer_size;
		trace_vkCmdCopyBuffer(command_buffers.at(num_buffers), origin_buffers[i], target_buffers[i], 1, &region);
	}
	trace_vkCmdPipelineBarrier(command_buffers.at(num_buffers), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	result = trace_vkEndCommandBuffer(command_buffers.at(num_buffers));
	check(result);
	for (unsigned i = 0; i < num_buffers; i++)
	{
		uint64_t v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[i], VK_TRACING_OBJECT_PROPERTY_MARKED_OBJECTS_TRACETOOLTEST);
		assert(v == 2);
		v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[i], VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
		assert(v == 2);
		v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[i], VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
		assert(v == buffer_size * 2);
	}
	{
		uint64_t v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[num_buffers], VK_TRACING_OBJECT_PROPERTY_MARKED_OBJECTS_TRACETOOLTEST);
		assert(v == 2 * num_buffers);
		v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[num_buffers], VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
		assert(v == 2 * num_buffers);
		v = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)command_buffers[num_buffers], VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
		assert(v == buffer_size * 2 * num_buffers);
	}
	if (p__debug_level > 0)
	{
		printf("Status before:\n");
		for (unsigned i = 0; i < num_buffers + 1; i++)
		{
			print_cmdbuf(vulkan, command_buffers[i]);
		}
		for (unsigned i = 0; i < num_buffers; i++)
		{
			print_buffer(vulkan, origin_buffers.at(i));
			print_buffer(vulkan, target_buffers.at(i));
		}
	}
	print_memory(vulkan, origin_memory, "origin");
	print_memory(vulkan, target_memory, "target");
	assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)origin_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST) == 1);
	assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)target_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST) == 0);
	if (queue_variant == 0 || queue_variant == 4)
	{
		std::vector<VkFence> fences(num_buffers);
		for (unsigned i = 0; i < num_buffers; i++)
		{
			result = trace_vkCreateFence(vulkan.device, &fence_create_info, NULL, &fences[i]);
			check(result);
		}
		for (unsigned i = 0; i < num_buffers; i++)
		{
			if (map_variant == 0 && flush_variant == 0)
			{
				VkMappedMemoryRange range = {};
				range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
				range.memory = origin_memory;
				range.size = aligned_size;
				range.offset = aligned_size * i;
				result = trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &range);
				check(result);
			}
			VkSubmitInfo submit_info = {};
			submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit_info.commandBufferCount = 1;
			submit_info.pCommandBuffers = &command_buffers[i];
			result = trace_vkQueueSubmit(queue, 1, &submit_info, fences[i]);
			check(result);
			if (queue_variant == 0) waitfence(vulkan, fences[i]);
		}
		if (queue_variant == 4)
		{
			if (fence_variant == 0)
			{
				result = trace_vkWaitForFences(vulkan.device, num_buffers, fences.data(), VK_TRUE, UINT64_MAX);
				check(result);
			}
			else for (unsigned i = 0; i < num_buffers; i++) waitfence(vulkan, fences[i]);
		}
		result = trace_vkResetFences(vulkan.device, num_buffers, fences.data());
		check(result);
		for (unsigned i = 0; i < num_buffers; i++) trace_vkDestroyFence(vulkan.device, fences[i], nullptr);
		if (p__debug_level > 0)
		{
			printf("Status after:\n");
			print_memory(vulkan, origin_memory, "origin");
			print_memory(vulkan, target_memory, "target");
			for (unsigned i = 0; i < num_buffers; i++) print_cmdbuf(vulkan, command_buffers[i]);
			for (unsigned i = 0; i < num_buffers; i++) print_buffer(vulkan, origin_buffers.at(i));
			for (unsigned i = 0; i < num_buffers; i++) print_buffer(vulkan, target_buffers.at(i));
		}
		if (map_variant == 0) assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)origin_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST) > 0);
		if (map_variant == 1) assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)origin_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST) == 0);
	}
	else if (queue_variant == 1) // slighly "better" version
	{
		std::vector<VkSubmitInfo> submit_info(num_buffers); // doing only one submission call!
		for (unsigned i = 0; i < num_buffers; i++)
		{
			submit_info[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit_info[i].commandBufferCount = 1;
			submit_info[i].pCommandBuffers = &command_buffers[i];
		}
		if (map_variant != 1 && flush_variant == 0)
		{
			VkMappedMemoryRange range = {}; // and only one flush call!
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = origin_memory;
			if (map_variant == 2)
			{
				range.offset = 10;
				range.size = 20;
			}
			else
			{
				range.size = VK_WHOLE_SIZE;
			}
			result = trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &range);
			check(result);
		}
		result = trace_vkQueueSubmit(queue, num_buffers, submit_info.data(), fence);
		check(result);
		waitfence(vulkan, fence); // only one fence...
		if (p__debug_level > 0)
		{
			printf("Status after:\n");
			print_memory(vulkan, origin_memory, "origin");
			print_memory(vulkan, target_memory, "target");
			for (unsigned i = 0; i < num_buffers; i++)
			{
				print_cmdbuf(vulkan, command_buffers[i]);
				print_buffer(vulkan, origin_buffers.at(i));
				print_buffer(vulkan, target_buffers.at(i));
			}
		}
		if (map_variant == 0) assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)origin_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST) > 0);
		if (map_variant == 1) assert(trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)origin_memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST) == 0);
	}
	else if (queue_variant == 2) // interesting variant, serialize all the copies, but still one cmdbuffer per copy
	{
		VkSubmitInfo submit_info = {}; // doing only one submission call...
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = num_buffers;
		submit_info.pCommandBuffers = command_buffers.data(); // ... and only one submission of N command buffers
		if (map_variant != 1 && flush_variant == 0)
		{
			VkMappedMemoryRange range = {}; // and only one flush call! not N calls to flush the entire memory area...
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = origin_memory;
			if (map_variant == 2)
			{
				range.offset = 10;
				range.size = 20;
			}
			else
			{
				range.size = VK_WHOLE_SIZE;
			}
			result = trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &range);
			check(result);
		}
		result = trace_vkQueueSubmit(queue, 1, &submit_info, fence);
		check(result);
		waitfence(vulkan, fence); // only one fence...
		if (p__debug_level > 0)
		{
			printf("Status after:\n");
			print_memory(vulkan, origin_memory, "origin");
			print_memory(vulkan, target_memory, "target");
			print_cmdbuf(vulkan, command_buffers[num_buffers]);
			for (unsigned i = 0; i < num_buffers; i++)
			{
				print_buffer(vulkan, origin_buffers.at(i));
				print_buffer(vulkan, target_buffers.at(i));
			}
		}
	}
	else if (queue_variant == 3) // probably the version that makes the most sense, all copy commands in one cmdbuffer, one submit
	{
		VkSubmitInfo submit_info = {}; // doing only one submission call...
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &command_buffers[num_buffers]; // ... and only one submission
		if (map_variant != 1 && flush_variant == 0)
		{
			VkMappedMemoryRange range = {}; // and only one flush call! not N calls to flush the entire memory area...
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = origin_memory;
			if (map_variant == 2)
			{
				range.offset = 10;
				range.size = 20;
			}
			else
			{
				range.size = VK_WHOLE_SIZE;
			}
			result = trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &range);
			check(result);
		}
		result = trace_vkQueueSubmit(queue, 1, &submit_info, fence);
		check(result);
		waitfence(vulkan, fence); // only one fence...
		if (p__debug_level > 0)
		{
			printf("Status after:\n");
			print_memory(vulkan, origin_memory, "origin");
			print_memory(vulkan, target_memory, "target");
			print_cmdbuf(vulkan, command_buffers[num_buffers]);
			for (unsigned i = 0; i < num_buffers; i++)
			{
				print_buffer(vulkan, origin_buffers.at(i));
				print_buffer(vulkan, target_buffers.at(i));
			}
		}
	}

	// Cleanup...
	if (map_variant == 0 || map_variant == 2) trace_vkUnmapMemory(vulkan.device, origin_memory);
	trace_vkDestroyFence(vulkan.device, fence, nullptr);
	for (unsigned i = 0; i < num_buffers; i++)
	{
		test_destroy_buffer(vulkan, i, origin_memory, origin_buffers.at(i), i * aligned_size, buffer_size);
		test_destroy_buffer(vulkan, i, target_memory, target_buffers.at(i), i * aligned_size, buffer_size);
	}

	trace_vkFreeMemory(vulkan.device, origin_memory, nullptr);
	trace_vkFreeMemory(vulkan.device, target_memory, nullptr);
	trace_vkFreeCommandBuffers(vulkan.device, command_pool, num_buffers + 1, command_buffers.data());
	trace_vkDestroyCommandPool(vulkan.device, command_pool, nullptr);
	test_done(vulkan);
}

static void test_buffer(lava_file_reader& t, uint32_t device_index, uint32_t buffer_index)
{
	const trackeddevice& device_data = VkDevice_index.at(device_index);
	VkDevice device = index_to_VkDevice.at(device_index);
	assert(buffer_index % 2 == 0); // every second buffer is target, which is off-limits
	suballoc_location loc = device_data.allocator->find_buffer_memory(buffer_index);
	assert(loc.size >= buffer_size);
	char* ptr = nullptr;
	VkResult result = wrap_vkMapMemory(device, loc.memory, loc.offset, loc.size, 0, (void**)&ptr);
	assert(result == VK_SUCCESS);
	uint32_t changed = t.read_patch(ptr, loc.size);
	if (changed == 0) spurious_checks++;
	assert(changed == buffer_size || changed == 0);
	wrap_vkUnmapMemory(device, loc.memory);
}

static bool getnext(lava_file_reader& t)
{
	const uint8_t instrtype = t.step();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == VKDESTROYDEVICE) // vkDestroyDevice
		{
			assert(index_to_VkCommandBuffer.size() == num_buffers + 1);
			assert(index_to_VkCommandPool.size() == 1);
		}
		return (apicall != 1); // was not vkDestroyInstance
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else if (instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_IMAGE_UPDATE2)
	{
		assert(false); // should not happen here!
		update_image_packet(instrtype, t);
	}
	else if (instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_BUFFER_UPDATE2)
	{
		DLOG2("Update buffer packet on thread %d", t.thread_index());
		const uint32_t device_index = t.read_handle(DEBUGPARAM("VkDevice"));
		const uint32_t buffer_index = t.read_handle(DEBUGPARAM("VkBuffer"));
		if (instrtype == PACKET_BUFFER_UPDATE2)
		{
			(void)t.read_uint64_t();
			(void)t.read_uint16_t();
		}
		test_buffer(t, device_index, buffer_index);
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		assert(false);
		update_tensor_packet(instrtype, t);
	}
	else assert(false);
	return true;
}

static void vkCreateDevice_callback(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	const uint32_t device_index = index_to_VkDevice.index(*pDevice);
	assert(device_index != UINT32_MAX);
	const trackeddevice& device_data = VkDevice_index.at(device_index);
	device_data.self_test();
	device_data.allocator->self_test();
}

static void vkDestroyDevice_callback(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	const uint32_t device_index = index_to_VkDevice.index(device);
	const trackeddevice& device_data = VkDevice_index.at(device_index);
	device_data.self_test();
}

static void retrace_4()
{
	std::string name = filename() + ".vk";
	printf("Running %s\n", name.c_str());
	lava_reader r(name);
	lava_file_reader& t = r.file_reader(0);
	vkCreateDevice_callbacks.push_back(vkCreateDevice_callback);
	vkDestroyDevice_callbacks.push_back(vkDestroyDevice_callback);
	while (getnext(t)) {}
}

int main(int argc, char** argv)
{
	int remaining = argc - 1; // zeroth is name of program
	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-b", "--buffer-size", remaining))
		{
			buffer_size = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-c", "--buffer-count", remaining))
		{
			num_buffers = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-H", "--heap-size", remaining))
		{
			p__suballocator_heap_size = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-q", "--queue-variant", remaining))
		{
			queue_variant = get_int(argv[++i], remaining);
			if (queue_variant < 0 || queue_variant > 4)
			{
				usage();
			}
		}
		else if (match(argv[i], "-F", "--flush-variant", remaining))
		{
			flush_variant = get_int(argv[++i], remaining);
			if (flush_variant < 0 || flush_variant > 1)
			{
				usage();
			}
		}
		else if (match(argv[i], "-m", "--map-variant", remaining))
		{
			map_variant = get_int(argv[++i], remaining);
			if (map_variant < 0 || map_variant > 2)
			{
				usage();
			}
		}
		else if (match(argv[i], "-f", "--queue-variant", remaining))
		{
			fence_variant = get_int(argv[++i], remaining);
			if (fence_variant < 0 || fence_variant > 1)
			{
				usage();
			}
		}
	}
	if (remaining)
	{
		usage();
	}
	printf("Running tracing4 with queue variant %d, map variant %d\n", queue_variant, map_variant);
	trace_4();
	retrace_4();
	if (spurious_checks) printf("%d spurious buffer diffs generated!\n", spurious_checks); // this is bad
	return 0;
}
