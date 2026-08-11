// Regression test for capture of host-written index buffers used by vkCmdDrawIndexed.

#include "tests/common.h"
#include "external/tracetooltests/include/vulkan_ext.h"
#include "tracetooltests/src/vulkan_dynamic_rendering_frag.inc"
#include "tracetooltests/src/vulkan_dynamic_rendering_vert.inc"

#define TEST_NAME "tracing_indexed_draw"

static VkShaderModule create_shader(VkDevice device, const unsigned char* code, size_t size)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr };
	info.codeSize = size;
	info.pCode = reinterpret_cast<const uint32_t*>(code);
	VkShaderModule shader = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateShaderModule(device, &info, nullptr, &shader);
	check(result);
	return shader;
}

static VkPipeline create_pipeline(VkDevice device, VkPipelineLayout layout, VkFormat format)
{
	VkShaderModule vert = create_shader(device, vulkan_dynamic_rendering_vert_spv, vulkan_dynamic_rendering_vert_spv_len);
	VkShaderModule frag = create_shader(device, vulkan_dynamic_rendering_frag_spv, vulkan_dynamic_rendering_frag_spv_len);

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr };
	VkPipelineInputAssemblyStateCreateInfo input_assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr };
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rasterization = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, nullptr };
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState attachment = {};
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr };
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;
	VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;
	VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr };
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &format;

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &input_assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
	check(result);

	trace_vkDestroyShaderModule(device, frag, nullptr);
	trace_vkDestroyShaderModule(device, vert, nullptr);
	return pipeline;
}

int main()
{
	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_3;
	reqs.reqfeat13.dynamicRendering = VK_TRUE;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result;

	VkQueue queue = VK_NULL_HANDLE;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);

	const uint32_t indices[3] = { 0, 1, 2 };
	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.size = sizeof(indices);
	buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer index_buffer = VK_NULL_HANDLE;
	result = trace_vkCreateBuffer(vulkan.device, &buffer_info, nullptr, &index_buffer);
	check(result);
	VkMemoryRequirements buffer_requirements;
	trace_vkGetBufferMemoryRequirements(vulkan.device, index_buffer, &buffer_requirements);
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	allocation.allocationSize = buffer_requirements.size;
	allocation.memoryTypeIndex = get_device_memory_type(buffer_requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory index_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &allocation, nullptr, &index_memory);
	check(result);
	void* mapped = nullptr;
	result = trace_vkMapMemory(vulkan.device, index_memory, 0, sizeof(indices), 0, &mapped);
	check(result);
	memcpy(mapped, indices, sizeof(indices));
	trace_vkUnmapMemory(vulkan.device, index_memory);
	result = trace_vkBindBufferMemory(vulkan.device, index_buffer, index_memory, 0);
	check(result);

	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr };
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent = { 16, 16, 1 };
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image = VK_NULL_HANDLE;
	result = trace_vkCreateImage(vulkan.device, &image_info, nullptr, &image);
	check(result);
	VkMemoryRequirements image_requirements;
	trace_vkGetImageMemoryRequirements(vulkan.device, image, &image_requirements);
	allocation.allocationSize = image_requirements.size;
	allocation.memoryTypeIndex = get_device_memory_type(image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory image_memory = VK_NULL_HANDLE;
	result = trace_vkAllocateMemory(vulkan.device, &allocation, nullptr, &image_memory);
	check(result);
	result = trace_vkBindImageMemory(vulkan.device, image, image_memory, 0);
	check(result);
	VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr };
	view_info.image = image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	VkImageView view = VK_NULL_HANDLE;
	result = trace_vkCreateImageView(vulkan.device, &view_info, nullptr, &view);
	check(result);

	VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr };
	VkPipelineLayout layout = VK_NULL_HANDLE;
	result = trace_vkCreatePipelineLayout(vulkan.device, &layout_info, nullptr, &layout);
	check(result);
	VkPipeline pipeline = create_pipeline(vulkan.device, layout, format);

	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
	pool_info.queueFamilyIndex = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	result = trace_vkCreateCommandPool(vulkan.device, &pool_info, nullptr, &pool);
	check(result);
	VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
	command_info.commandPool = pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	result = trace_vkAllocateCommandBuffers(vulkan.device, &command_info, &command_buffer);
	check(result);
	VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
	result = trace_vkBeginCommandBuffer(command_buffer, &begin_info);
	check(result);
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr };
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = view_info.subresourceRange;
	trace_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkRenderingAttachmentInfo color_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, nullptr };
	color_attachment.imageView = view;
	color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rendering_info = { VK_STRUCTURE_TYPE_RENDERING_INFO, nullptr };
	rendering_info.renderArea.extent = { 16, 16 };
	rendering_info.layerCount = 1;
	rendering_info.colorAttachmentCount = 1;
	rendering_info.pColorAttachments = &color_attachment;
	trace_vkCmdBeginRendering(command_buffer, &rendering_info);
	trace_vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	VkViewport viewport = { 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
	trace_vkCmdSetViewport(command_buffer, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, { 16, 16 } };
	trace_vkCmdSetScissor(command_buffer, 0, 1, &scissor);
	trace_vkCmdBindIndexBuffer(command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT32);
	trace_vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0, 0);
	trace_vkCmdEndRendering(command_buffer);
	result = trace_vkEndCommandBuffer(command_buffer);
	check(result);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer;
	result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	check(result);

	const unsigned update_count = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device,
		VK_OBJECT_TYPE_BUFFER, (uint64_t)index_buffer, VK_TRACING_OBJECT_PROPERTY_UPDATES_COUNT_TRACETOOLTEST);
	const unsigned update_bytes = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device,
		VK_OBJECT_TYPE_BUFFER, (uint64_t)index_buffer, VK_TRACING_OBJECT_PROPERTY_UPDATES_BYTES_TRACETOOLTEST);
	assert(update_count == 1);
	assert(update_bytes == sizeof(indices));
	result = trace_vkQueueWaitIdle(queue);
	check(result);

	trace_vkDestroyCommandPool(vulkan.device, pool, nullptr);
	trace_vkDestroyPipeline(vulkan.device, pipeline, nullptr);
	trace_vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
	trace_vkDestroyImageView(vulkan.device, view, nullptr);
	trace_vkDestroyImage(vulkan.device, image, nullptr);
	trace_vkFreeMemory(vulkan.device, image_memory, nullptr);
	trace_vkDestroyBuffer(vulkan.device, index_buffer, nullptr);
	trace_vkFreeMemory(vulkan.device, index_memory, nullptr);
	test_done(vulkan);
	return 0;
}
