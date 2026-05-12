#include "tests/common.h"
#include "util_auto.h"

#include <array>
#include <map>
#include <vector>

#define TEST_NAME "tracing_dynamic_descriptors"

struct traced_buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t index = CONTAINER_INVALID_INDEX;
};

struct descriptor_layout
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
};

struct expected_update
{
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct patch_span
{
	uint32_t offset = 0;
	uint32_t size = 0;
};

static uint32_t dynamic_alignment = 0;
static uint32_t dynamic_range = 0;
static uint32_t buffer_size = 0;
static uint32_t memory_type_index = UINT32_MAX;

static std::vector<traced_buffer> buffers;
static std::vector<VkDescriptorSetLayout> set_layouts;
static std::vector<VkPipelineLayout> pipeline_layouts;
static std::vector<expected_update> expected_updates;
static std::vector<uint32_t> expected_absent_updates;
static std::map<uint32_t, std::vector<patch_span>> seen_updates;

static traced_buffer create_traced_buffer(const vulkan_setup_t& vulkan, uint8_t fill)
{
	traced_buffer out;
	VkResult result = VK_SUCCESS;
	for (unsigned attempt = 0; attempt < 32; attempt++)
	{
		VkBufferCreateInfo buffer_info = {};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = buffer_size;
		buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer buffer = VK_NULL_HANDLE;
		result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &buffer);
		check(result);
		const uint64_t backing = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER,
			(uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_BACKING_STORE_TRACETOOLTEST);
		if (backing == 0)
		{
			out.buffer = buffer;
			break;
		}
	}
	assert(out.buffer != VK_NULL_HANDLE);

	VkMemoryRequirements req = {};
	trace_vkGetBufferMemoryRequirements(vulkan.device, out.buffer, &req);
	assert(req.size >= buffer_size);
	if (memory_type_index == UINT32_MAX)
	{
		memory_type_index = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}

	VkMemoryAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.allocationSize = req.size;
	alloc_info.memoryTypeIndex = memory_type_index;
	result = trace_vkAllocateMemory(vulkan.device, &alloc_info, nullptr, &out.memory);
	check(result);

	VkBindBufferMemoryInfo bind_info = {};
	bind_info.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
	bind_info.buffer = out.buffer;
	bind_info.memory = out.memory;
	bind_info.memoryOffset = 0;
	result = trace_vkBindBufferMemory2(vulkan.device, 1, &bind_info);
	check(result);

	void* ptr = nullptr;
	result = trace_vkMapMemory(vulkan.device, out.memory, 0, buffer_size, 0, &ptr);
	check(result);
	memset(ptr, fill, buffer_size);
	VkMappedMemoryRange flush = {};
	flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	flush.memory = out.memory;
	flush.offset = 0;
	flush.size = buffer_size;
	result = trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
	check(result);
	trace_vkUnmapMemory(vulkan.device, out.memory);

	out.index = (uint32_t)trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER,
		(uint64_t)out.buffer, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	assert(out.index != CONTAINER_INVALID_INDEX);
	buffers.push_back(out);
	return out;
}

static descriptor_layout create_dynamic_layout(const vulkan_setup_t& vulkan, std::initializer_list<uint32_t> counts)
{
	descriptor_layout out;
	VkResult result = VK_SUCCESS;
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	bindings.reserve(counts.size());
	uint32_t binding = 0;
	for (uint32_t count : counts)
	{
		VkDescriptorSetLayoutBinding item = {};
		item.binding = binding++;
		item.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		item.descriptorCount = count;
		item.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bindings.push_back(item);
	}

	VkDescriptorSetLayoutCreateInfo set_info = {};
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = (uint32_t)bindings.size();
	set_info.pBindings = bindings.data();
	result = trace_vkCreateDescriptorSetLayout(vulkan.device, &set_info, nullptr, &out.set_layout);
	check(result);

	VkPipelineLayoutCreateInfo pipeline_info = {};
	pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_info.setLayoutCount = 1;
	pipeline_info.pSetLayouts = &out.set_layout;
	result = trace_vkCreatePipelineLayout(vulkan.device, &pipeline_info, nullptr, &out.pipeline_layout);
	check(result);

	set_layouts.push_back(out.set_layout);
	pipeline_layouts.push_back(out.pipeline_layout);
	return out;
}

static VkDescriptorSet allocate_descriptor_set(const vulkan_setup_t& vulkan, VkDescriptorPool pool, VkDescriptorSetLayout layout)
{
	VkResult result = VK_SUCCESS;
	VkDescriptorSetAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &layout;

	VkDescriptorSet set = VK_NULL_HANDLE;
	result = trace_vkAllocateDescriptorSets(vulkan.device, &alloc_info, &set);
	check(result);
	return set;
}

static void submit_bind(const vulkan_setup_t& vulkan, VkCommandPool cmdpool, VkQueue queue, VkPipelineLayout layout,
	VkDescriptorSet set, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets)
{
	VkResult result = VK_SUCCESS;
	VkCommandBufferAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool = cmdpool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	result = trace_vkAllocateCommandBuffers(vulkan.device, &alloc_info, &cmd);
	check(result);

	VkCommandBufferBeginInfo begin_info = {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = trace_vkBeginCommandBuffer(cmd, &begin_info);
	check(result);
	trace_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, dynamic_offset_count, dynamic_offsets);
	result = trace_vkEndCommandBuffer(cmd);
	check(result);

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	check(result);
	result = trace_vkQueueWaitIdle(queue);
	check(result);
	trace_vkFreeCommandBuffers(vulkan.device, cmdpool, 1, &cmd);
}

static void trace_null_dynamic_slot(const vulkan_setup_t& vulkan, VkDescriptorPool pool, VkCommandPool cmdpool, VkQueue queue)
{
	const descriptor_layout layout = create_dynamic_layout(vulkan, { 3 });
	const VkDescriptorSet set = allocate_descriptor_set(vulkan, pool, layout.set_layout);

	const traced_buffer first = create_traced_buffer(vulkan, 0x11);
	const traced_buffer third = create_traced_buffer(vulkan, 0x33);

	std::array<VkDescriptorBufferInfo, 3> infos = {};
	infos[0].buffer = first.buffer;
	infos[0].offset = 0;
	infos[0].range = dynamic_range;
	infos[1].buffer = VK_NULL_HANDLE;
	infos[1].offset = 0;
	infos[1].range = dynamic_range;
	infos[2].buffer = third.buffer;
	infos[2].offset = 0;
	infos[2].range = dynamic_range;

	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.dstArrayElement = 0;
	write.descriptorCount = (uint32_t)infos.size();
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	write.pBufferInfo = infos.data();
	trace_vkUpdateDescriptorSets(vulkan.device, 1, &write, 0, nullptr);

	const uint32_t dynamic_offsets[3] = { dynamic_alignment, dynamic_alignment * 2, dynamic_alignment * 3 };
	submit_bind(vulkan, cmdpool, queue, layout.pipeline_layout, set, 3, dynamic_offsets);

	expected_updates.push_back({ first.index, dynamic_offsets[0], dynamic_range });
	expected_updates.push_back({ third.index, dynamic_offsets[2], dynamic_range });
}

static void trace_dynamic_array_copy(const vulkan_setup_t& vulkan, VkDescriptorPool pool, VkCommandPool cmdpool, VkQueue queue)
{
	const descriptor_layout layout = create_dynamic_layout(vulkan, { 3 });
	const VkDescriptorSet src_set = allocate_descriptor_set(vulkan, pool, layout.set_layout);
	const VkDescriptorSet dst_set = allocate_descriptor_set(vulkan, pool, layout.set_layout);

	const traced_buffer src0 = create_traced_buffer(vulkan, 0x40);
	const traced_buffer src1 = create_traced_buffer(vulkan, 0x41);
	const traced_buffer src2 = create_traced_buffer(vulkan, 0x42);
	const traced_buffer dst0 = create_traced_buffer(vulkan, 0x50);
	const traced_buffer dst1 = create_traced_buffer(vulkan, 0x51);
	const traced_buffer dst2 = create_traced_buffer(vulkan, 0x52);

	std::array<VkDescriptorBufferInfo, 3> src_infos = {};
	src_infos[0].buffer = src0.buffer;
	src_infos[0].offset = 0;
	src_infos[0].range = dynamic_range;
	src_infos[1].buffer = src1.buffer;
	src_infos[1].offset = 0;
	src_infos[1].range = dynamic_range;
	src_infos[2].buffer = src2.buffer;
	src_infos[2].offset = 0;
	src_infos[2].range = dynamic_range;

	std::array<VkDescriptorBufferInfo, 3> dst_infos = {};
	dst_infos[0].buffer = dst0.buffer;
	dst_infos[0].offset = 0;
	dst_infos[0].range = dynamic_range;
	dst_infos[1].buffer = dst1.buffer;
	dst_infos[1].offset = 0;
	dst_infos[1].range = dynamic_range;
	dst_infos[2].buffer = dst2.buffer;
	dst_infos[2].offset = 0;
	dst_infos[2].range = dynamic_range;

	VkWriteDescriptorSet writes[2] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = src_set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = (uint32_t)src_infos.size();
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[0].pBufferInfo = src_infos.data();

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = dst_set;
	writes[1].dstBinding = 0;
	writes[1].descriptorCount = (uint32_t)dst_infos.size();
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[1].pBufferInfo = dst_infos.data();

	trace_vkUpdateDescriptorSets(vulkan.device, 2, writes, 0, nullptr);

	VkCopyDescriptorSet copy = {};
	copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
	copy.srcSet = src_set;
	copy.dstSet = dst_set;
	copy.srcBinding = 0;
	copy.srcArrayElement = 1;
	copy.dstBinding = 0;
	copy.dstArrayElement = 2;
	copy.descriptorCount = 1;
	trace_vkUpdateDescriptorSets(vulkan.device, 0, nullptr, 1, &copy);

	const uint32_t dynamic_offsets[3] = { dynamic_alignment, dynamic_alignment * 2, dynamic_alignment * 3 };
	submit_bind(vulkan, cmdpool, queue, layout.pipeline_layout, dst_set, 3, dynamic_offsets);

	expected_updates.push_back({ dst0.index, dynamic_offsets[0], dynamic_range });
	expected_updates.push_back({ dst1.index, dynamic_offsets[1], dynamic_range });
	expected_updates.push_back({ src1.index, dynamic_offsets[2], dynamic_range });
	expected_absent_updates.push_back(src0.index);
	expected_absent_updates.push_back(src2.index);
	expected_absent_updates.push_back(dst2.index);
}

static void trace_dynamic_binding_copy(const vulkan_setup_t& vulkan, VkDescriptorPool pool, VkCommandPool cmdpool, VkQueue queue)
{
	const descriptor_layout layout = create_dynamic_layout(vulkan, { 1, 1 });
	const VkDescriptorSet src_set = allocate_descriptor_set(vulkan, pool, layout.set_layout);
	const VkDescriptorSet dst_set = allocate_descriptor_set(vulkan, pool, layout.set_layout);

	const traced_buffer src0 = create_traced_buffer(vulkan, 0x60);
	const traced_buffer src1 = create_traced_buffer(vulkan, 0x61);
	const traced_buffer dst0 = create_traced_buffer(vulkan, 0x70);
	const traced_buffer dst1 = create_traced_buffer(vulkan, 0x71);

	VkDescriptorBufferInfo src_infos[2] = {};
	src_infos[0].buffer = src0.buffer;
	src_infos[0].offset = 0;
	src_infos[0].range = dynamic_range;
	src_infos[1].buffer = src1.buffer;
	src_infos[1].offset = 0;
	src_infos[1].range = dynamic_range;

	VkDescriptorBufferInfo dst_infos[2] = {};
	dst_infos[0].buffer = dst0.buffer;
	dst_infos[0].offset = 0;
	dst_infos[0].range = dynamic_range;
	dst_infos[1].buffer = dst1.buffer;
	dst_infos[1].offset = 0;
	dst_infos[1].range = dynamic_range;

	VkWriteDescriptorSet writes[4] = {};
	for (uint32_t i = 0; i < 2; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = src_set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[i].pBufferInfo = &src_infos[i];

		writes[i + 2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i + 2].dstSet = dst_set;
		writes[i + 2].dstBinding = i;
		writes[i + 2].descriptorCount = 1;
		writes[i + 2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[i + 2].pBufferInfo = &dst_infos[i];
	}

	trace_vkUpdateDescriptorSets(vulkan.device, 4, writes, 0, nullptr);

	VkCopyDescriptorSet copy = {};
	copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
	copy.srcSet = src_set;
	copy.dstSet = dst_set;
	copy.srcBinding = 0;
	copy.srcArrayElement = 0;
	copy.dstBinding = 1;
	copy.dstArrayElement = 0;
	copy.descriptorCount = 1;
	trace_vkUpdateDescriptorSets(vulkan.device, 0, nullptr, 1, &copy);

	const uint32_t dynamic_offsets[2] = { dynamic_alignment, dynamic_alignment * 2 };
	submit_bind(vulkan, cmdpool, queue, layout.pipeline_layout, dst_set, 2, dynamic_offsets);

	expected_updates.push_back({ dst0.index, dynamic_offsets[0], dynamic_range });
	expected_updates.push_back({ src0.index, dynamic_offsets[1], dynamic_range });
	expected_absent_updates.push_back(src1.index);
	expected_absent_updates.push_back(dst1.index);
}

static void trace()
{
	vulkan_req_t reqs = {};
	reqs.apiVersion = VK_API_VERSION_1_1;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result = VK_SUCCESS;

	VkPhysicalDeviceProperties props = {};
	trace_vkGetPhysicalDeviceProperties(vulkan.physical, &props);
	dynamic_alignment = std::max<uint32_t>(8u, (uint32_t)props.limits.minUniformBufferOffsetAlignment);
	dynamic_range = dynamic_alignment;
	buffer_size = dynamic_alignment * 8;

	VkDescriptorPoolSize pool_size = {};
	pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	pool_size.descriptorCount = 32;

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 8;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;

	VkDescriptorPool pool = VK_NULL_HANDLE;
	result = trace_vkCreateDescriptorPool(vulkan.device, &pool_info, nullptr, &pool);
	check(result);

	VkCommandPoolCreateInfo cmdpool_info = {};
	cmdpool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdpool_info.queueFamilyIndex = 0;

	VkCommandPool cmdpool = VK_NULL_HANDLE;
	result = trace_vkCreateCommandPool(vulkan.device, &cmdpool_info, nullptr, &cmdpool);
	check(result);

	VkQueue queue = VK_NULL_HANDLE;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);
	assert(queue != VK_NULL_HANDLE);

	trace_null_dynamic_slot(vulkan, pool, cmdpool, queue);
	trace_dynamic_array_copy(vulkan, pool, cmdpool, queue);
	trace_dynamic_binding_copy(vulkan, pool, cmdpool, queue);

	for (VkPipelineLayout pipeline_layout : pipeline_layouts)
	{
		trace_vkDestroyPipelineLayout(vulkan.device, pipeline_layout, nullptr);
	}
	for (VkDescriptorSetLayout set_layout : set_layouts)
	{
		trace_vkDestroyDescriptorSetLayout(vulkan.device, set_layout, nullptr);
	}
	for (const traced_buffer& buffer : buffers)
	{
		trace_vkDestroyBuffer(vulkan.device, buffer.buffer, nullptr);
		trace_vkFreeMemory(vulkan.device, buffer.memory, nullptr);
	}
	trace_vkDestroyCommandPool(vulkan.device, cmdpool, nullptr);
	trace_vkDestroyDescriptorPool(vulkan.device, pool, nullptr);
	test_done(vulkan);
}

static void parse_buffer_update(uint8_t instrtype, lava_file_reader& reader)
{
	(void)reader.read_handle(DEBUGPARAM("VkDevice"));
	const uint32_t buffer_index = reader.read_handle(DEBUGPARAM("VkBuffer"));
	if (instrtype == PACKET_BUFFER_UPDATE2)
	{
		(void)reader.read_uint64_t();
		const uint16_t flags = reader.read_uint16_t();
		assert(flags == 0);
	}

	uint64_t position = 0;
	for (;;)
	{
		const uint32_t offset = reader.read_uint32_t();
		position += offset;
		const uint32_t size = reader.read_uint32_t();
		if (offset == 0 && size == 0) break;
		seen_updates[buffer_index].push_back({ (uint32_t)position, size });
		std::vector<char> scratch(size);
		if (size > 0) reader.read_array(scratch.data(), size);
		position += size;
	}
}

static bool process_next(lava_file_reader& reader)
{
	const uint8_t instrtype = reader.step();
	if (instrtype == 0) return false;

	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		reader.read_apicall();
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		reader.read_barrier();
	}
	else if (instrtype == PACKET_IMAGE_UPDATE || instrtype == PACKET_IMAGE_UPDATE2)
	{
		update_image_packet(instrtype, reader);
	}
	else if (instrtype == PACKET_BUFFER_UPDATE || instrtype == PACKET_BUFFER_UPDATE2)
	{
		parse_buffer_update(instrtype, reader);
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		update_tensor_packet(instrtype, reader);
	}
	else
	{
		assert(false);
	}
	return true;
}

static void assert_exact_update(uint32_t buffer_index, uint32_t offset, uint32_t size)
{
	assert(buffer_index != CONTAINER_INVALID_INDEX);
	const auto it = seen_updates.find(buffer_index);
	assert(it != seen_updates.end());
	assert(it->second.size() == 1);
	assert(it->second[0].offset == offset);
	assert(it->second[0].size == size);
}

static void assert_no_update(uint32_t buffer_index)
{
	const auto it = seen_updates.find(buffer_index);
	assert(it == seen_updates.end() || it->second.empty());
}

static void retrace()
{
	lava_reader reader(TEST_NAME ".vk");
	lava_file_reader& thread = reader.file_reader(0);
	thread.run = false;
	while (process_next(thread)) {}

	for (const expected_update& item : expected_updates)
	{
		assert_exact_update(item.buffer_index, item.offset, item.size);
	}
	for (uint32_t buffer_index : expected_absent_updates)
	{
		assert_no_update(buffer_index);
	}
}

int main()
{
	trace();
	retrace();
	return 0;
}
