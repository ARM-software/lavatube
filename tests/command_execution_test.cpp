#include "execute_commands.h"
#include "markings.h"
#include "read_auto.h"
#include "suballocator.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <list>
#include <vector>

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#if (__clang_major__ > 12) || (!defined(__llvm__) && defined(__GNUC__))
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#ifndef __clang__
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

static const unsigned char command_execution_compute_spv[] = {
	0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x08, 0x00,
	0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x47, 0x4c, 0x53, 0x4c, 0x2e, 0x73, 0x74, 0x64, 0x2e, 0x34, 0x35, 0x30,
	0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x06, 0x00, 0x04, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc2, 0x01, 0x00, 0x00,
	0x05, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x4f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x42, 0x75, 0x66, 0x66, 0x65, 0x72,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x6f, 0x75, 0x74, 0x70,
	0x75, 0x74, 0x5f, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x05, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x49, 0x6e, 0x70, 0x75,
	0x74, 0x42, 0x75, 0x66, 0x66, 0x65, 0x72, 0x00, 0x06, 0x00, 0x05, 0x00,
	0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0x61, 0x6c, 0x75,
	0x65, 0x00, 0x00, 0x00, 0x05, 0x00, 0x06, 0x00, 0x0e, 0x00, 0x00, 0x00,
	0x69, 0x6e, 0x70, 0x75, 0x74, 0x5f, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72,
	0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x48, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00,
	0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00,
	0x19, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x09, 0x00, 0x00, 0x00,
	0x21, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x47, 0x00, 0x03, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x48, 0x00, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00, 0x0c, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x47, 0x00, 0x03, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
	0x47, 0x00, 0x04, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x0e, 0x00, 0x00, 0x00,
	0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
	0x16, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00,
	0x13, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x21, 0x00, 0x03, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x1e, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
	0x20, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00,
	0x0a, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x2b, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x03, 0x00, 0x0c, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x0d, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00,
	0x0d, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x20, 0x00, 0x04, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
	0x12, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x17, 0x00, 0x04, 0x00,
	0x15, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x2c, 0x00, 0x06, 0x00, 0x15, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00,
	0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
	0x36, 0x00, 0x05, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x02, 0x00,
	0x05, 0x00, 0x00, 0x00, 0x41, 0x00, 0x05, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
	0x3d, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x80, 0x00, 0x05, 0x00, 0x06, 0x00, 0x00, 0x00,
	0x13, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
	0x41, 0x00, 0x05, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00,
	0x14, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x01, 0x00,
	0x38, 0x00, 0x01, 0x00,
};

#include "command_execution_shaders.inc"

void usage()
{
	printf("command_execution_test %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
#endif
	printf("-p/--perf RUNS         Run in performance mode, executing RUNS times\n");
}

static std::vector<uint32_t> shader_code_from_bytes(const unsigned char* data, size_t size)
{
	assert(size % sizeof(uint32_t) == 0);
	std::vector<uint32_t> code(size / sizeof(uint32_t));
	std::memcpy(code.data(), data, size);
	return code;
}

static std::vector<uint32_t> compute_shader_code()
{
	return shader_code_from_bytes(command_execution_compute_spv, sizeof(command_execution_compute_spv));
}

static std::vector<uint32_t> plain_copy_shader_code()
{
	return shader_code_from_bytes(command_execution_plain_copy_spv, command_execution_plain_copy_spv_len);
}

static std::vector<uint32_t> bda_push_read_shader_code()
{
	return shader_code_from_bytes(command_execution_bda_push_read_spv, command_execution_bda_push_read_spv_len);
}

static std::vector<uint32_t> bda_buffer_read_shader_code()
{
	return shader_code_from_bytes(command_execution_bda_buffer_read_spv, command_execution_bda_buffer_read_spv_len);
}

static std::vector<uint32_t> bda_interleave_copy_shader_code()
{
	return shader_code_from_bytes(command_execution_bda_interleave_copy_spv, command_execution_bda_interleave_copy_spv_len);
}

static std::vector<uint32_t> bda_interleave_output_shader_code()
{
	return shader_code_from_bytes(command_execution_bda_interleave_output_spv, command_execution_bda_interleave_output_spv_len);
}

static std::vector<uint32_t> bda_two_store_shader_code()
{
	return shader_code_from_bytes(command_execution_bda_two_store_direct_spv, command_execution_bda_two_store_direct_spv_len);
}

static void execute_null()
{
	trackeddevice device_data;
	device_data.index = 0;

	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;

	address_remapper<trackedobject> device_address_remapping;
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};

	assert(execute_commands(data));
	assert(cmdbuffer_data.commands.empty());
	assert(data.stats.commands == 0);
	assert(data.stats.execution_commands == 0);
	assert(global_output_rewrite_queue.empty());
	assert(descriptor_buffer_payloads.empty());
}

static change_source make_source(uint32_t packet)
{
	change_source source;
	source.packet = packet;
	source.frame = 0;
	source.thread = 0;
	source.call_id = VKCMDUPDATEBUFFER;
	return source;
}

static bool same_source(const change_source& a, const change_source& b)
{
	return a.packet == b.packet && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
}

static void init_buffer(uint32_t index, VkDeviceSize size)
{
	trackedbuffer& buffer = VkBuffer_index[index];
	buffer.index = index;
	buffer.parent_device_index = 0;
	buffer.object_type = VK_OBJECT_TYPE_BUFFER;
	buffer.size = size;
	buffer.flags = 0;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.usage2 = 0;
	buffer.reqs.requirements.size = size;
	buffer.reqs.requirements.alignment = 1;
	buffer.reqs.requirements.memoryTypeBits = 1;
}

static void execute_copy_buffer()
{
	constexpr VkDeviceSize buffer_size = 16;
	constexpr VkDeviceSize src_offset = 2;
	constexpr VkDeviceSize dst_offset = 5;
	constexpr VkDeviceSize copy_size = 4;

	VkBuffer_index.clear();
	VkBuffer_index.resize(2);
	init_buffer(0, buffer_size);
	init_buffer(1, buffer_size);

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);

	suballoc_location src = allocator.find_buffer_memory(0);
	suballoc_location dst = allocator.find_buffer_memory(1);
	std::memset(src.memory, 0, src.size);
	std::memset(dst.memory, 0, dst.size);
	const char payload[] = { 'l', 'a', 'v', 'a' };
	std::memcpy(reinterpret_cast<char*>(src.memory) + src_offset, payload, sizeof(payload));

	const change_source source = make_source(7);
	VkBuffer_index[0].source.register_source(src_offset, copy_size, source);

	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;
	trackedcommand command { VKCMDCOPYBUFFER };
	command.data.copy_buffer.src_buffer_index = 0;
	command.data.copy_buffer.dst_buffer_index = 1;
	command.data.copy_buffer.regionCount = 1;
	command.data.copy_buffer.pRegions = static_cast<VkBufferCopy*>(std::malloc(sizeof(VkBufferCopy)));
	assert(command.data.copy_buffer.pRegions);
	command.data.copy_buffer.pRegions[0] = { src_offset, dst_offset, copy_size };
	cmdbuffer_data.commands.push_back(command);

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};

	assert(execute_commands(data));
	assert(std::memcmp(reinterpret_cast<char*>(dst.memory) + dst_offset, payload, sizeof(payload)) == 0);
	assert(data.stats.commands == 1);
	assert(data.stats.execution_commands == 0);

	change_source copied_source;
	assert(VkBuffer_index[1].source.try_get_source(dst_offset, copy_size, copied_source));
	assert(same_source(copied_source, source));
	assert(global_output_rewrite_queue.empty());
	assert(descriptor_buffer_payloads.empty());

	allocator.destroy();
}

static uint32_t load_u32(const suballoc_location& loc, VkDeviceSize offset)
{
	uint32_t value = 0;
	std::memcpy(&value, reinterpret_cast<char*>(loc.memory) + offset, sizeof(value));
	return value;
}

static void store_u32(const suballoc_location& loc, VkDeviceSize offset, uint32_t value)
{
	std::memcpy(reinterpret_cast<char*>(loc.memory) + offset, &value, sizeof(value));
}

static uint32_t add_compute_pipeline(uint64_t unique_index, const std::vector<uint32_t>& code)
{
	const uint32_t pipeline_index = (uint32_t)VkPipeline_index.size();
	VkPipeline_index.resize(pipeline_index + 1);
	trackedpipeline& pipeline = VkPipeline_index[pipeline_index];
	pipeline.index = pipeline_index;
	pipeline.device_index = 0;
	pipeline.type = VK_PIPELINE_BIND_POINT_COMPUTE;
	pipeline.shader_stages.resize(1);
	shader_stage& stage = pipeline.shader_stages[0];
	stage.index = 0;
	stage.unique_index = unique_index;
	stage.device_index = 0;
	stage.flags = 0;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.name = "main";
	stage.code = code;
	return pipeline_index;
}

static void init_compute_pipeline(uint64_t unique_index, const std::vector<uint32_t>& code)
{
	VkPipeline_index.clear();
	add_compute_pipeline(unique_index, code);
}

static void add_bind_compute_pipeline(trackedcmdbuffer& cmdbuffer_data, uint32_t pipeline_index = 0)
{
	trackedcommand bind_pipeline { VKCMDBINDPIPELINE };
	bind_pipeline.data.bind_pipeline.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
	bind_pipeline.data.bind_pipeline.pipeline_index = pipeline_index;
	cmdbuffer_data.commands.push_back(bind_pipeline);
}

static void add_bind_descriptorset(trackedcmdbuffer& cmdbuffer_data, uint32_t descriptorset_index)
{
	trackedcommand bind_descriptorset { VKCMDBINDDESCRIPTORSETS };
	bind_descriptorset.data.bind_descriptorsets.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
	bind_descriptorset.data.bind_descriptorsets.layout = VK_NULL_HANDLE;
	bind_descriptorset.data.bind_descriptorsets.firstSet = 0;
	bind_descriptorset.data.bind_descriptorsets.descriptorSetCount = 1;
	bind_descriptorset.data.bind_descriptorsets.pDescriptorSets = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t)));
	assert(bind_descriptorset.data.bind_descriptorsets.pDescriptorSets);
	bind_descriptorset.data.bind_descriptorsets.pDescriptorSets[0] = descriptorset_index;
	bind_descriptorset.data.bind_descriptorsets.dynamicOffsetCount = 0;
	bind_descriptorset.data.bind_descriptorsets.pDynamicOffsets = nullptr;
	cmdbuffer_data.commands.push_back(bind_descriptorset);
}

static void add_dispatch(trackedcmdbuffer& cmdbuffer_data, const change_source& source)
{
	trackedcommand dispatch { VKCMDDISPATCH };
	dispatch.source = source;
	cmdbuffer_data.commands.push_back(dispatch);
}

constexpr uint32_t compute_shader_input_value = 41;
constexpr uint32_t compute_shader_expected_output = 42;
constexpr VkDeviceSize compute_shader_buffer_size = sizeof(uint32_t);

struct compute_shader_fixture
{
	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	trackeddevice device_data;
	trackedcmdbuffer cmdbuffer_data;
	address_remapper<trackedobject> device_address_remapping;
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;
	command_execution_data data;

	compute_shader_fixture()
		: data {
			.device_data = device_data,
			.cmdbuffer_data = cmdbuffer_data,
			.device_address_remapping = device_address_remapping,
			.global_output_rewrite_queue = global_output_rewrite_queue,
			.pending_descriptor_rewrites = pending_descriptor_rewrites,
			.descriptor_buffer_payloads = descriptor_buffer_payloads,
		}
	{
		VkBuffer_index.clear();
		VkBuffer_index.resize(2);
		init_buffer(0, compute_shader_buffer_size);
		init_buffer(1, compute_shader_buffer_size);
		VkBuffer_index[0].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

		allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
		allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
		allocator.add_trackedobject(0, 2, VkBuffer_index[1]);

		suballoc_location input = allocator.find_buffer_memory(0);
		suballoc_location output = allocator.find_buffer_memory(1);
		std::memset(input.memory, 0, input.size);
		std::memset(output.memory, 0, output.size);
		store_u32(input, 0, compute_shader_input_value);
		VkBuffer_index[0].source.register_source(0, sizeof(uint32_t), make_source(11));

		init_compute_pipeline(1, compute_shader_code());

		cmdbuffer_data.index = 0;
		add_bind_compute_pipeline(cmdbuffer_data);
		add_dispatch(cmdbuffer_data, make_source(12));

		device_data.index = 0;
		device_data.allocator = &allocator;

		data.descriptorsets[0][0] = { &VkBuffer_index[0], 0, compute_shader_buffer_size };
		data.descriptorsets[0][1] = { &VkBuffer_index[1], 0, compute_shader_buffer_size };
	}

	~compute_shader_fixture()
	{
		allocator.destroy();
	}

	uint32_t output_value()
	{
		return load_u32(allocator.find_buffer_memory(1), 0);
	}

	shader_stage& stage()
	{
		return VkPipeline_index[0].shader_stages[0];
	}
};

static void execute_compute_shader()
{
	compute_shader_fixture fixture;

	assert(execute_commands(fixture.data));
	assert(fixture.output_value() == compute_shader_expected_output);
	assert(fixture.stage().calls == 1);
	assert(fixture.data.stats.commands == 2);
	assert(fixture.data.stats.execution_commands == 1);
	assert(fixture.descriptor_buffer_payloads.empty());
}

static void execute_compute_shader_copy_provenance()
{
	constexpr VkDeviceSize buffer_size = sizeof(uint32_t) * 2;
	constexpr uint32_t first_value = 0x11223344;
	constexpr uint32_t second_value = 0x55667788;

	VkBuffer_index.clear();
	VkBuffer_index.resize(2);
	init_buffer(0, buffer_size);
	init_buffer(1, buffer_size);
	VkBuffer_index[0].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);

	suballoc_location input = allocator.find_buffer_memory(0);
	suballoc_location output = allocator.find_buffer_memory(1);
	std::memset(input.memory, 0, input.size);
	std::memset(output.memory, 0, output.size);
	store_u32(input, 0, first_value);
	store_u32(input, sizeof(uint32_t), second_value);
	const change_source source = make_source(21);
	VkBuffer_index[0].source.register_source(0, buffer_size, source);

	init_compute_pipeline(2, plain_copy_shader_code());

	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;
	add_bind_compute_pipeline(cmdbuffer_data);
	add_dispatch(cmdbuffer_data, make_source(22));

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};
	data.descriptorsets[0][0] = { &VkBuffer_index[0], 0, buffer_size };
	data.descriptorsets[0][1] = { &VkBuffer_index[1], 0, buffer_size };

	assert(execute_commands(data));
	assert(load_u32(output, 0) == first_value);
	assert(load_u32(output, sizeof(uint32_t)) == second_value);
	assert(VkPipeline_index[0].shader_stages[0].calls == 1);
	assert(data.stats.commands == 2);
	assert(data.stats.execution_commands == 1);

	change_source copied_source;
	assert(VkBuffer_index[1].source.try_get_source(0, buffer_size, copied_source));
	assert(same_source(copied_source, source));
	assert(global_output_rewrite_queue.empty());
	assert(descriptor_buffer_payloads.empty());

	allocator.destroy();
}

static void execute_compute_shader_bda_unbound_input()
{
	constexpr VkDeviceSize source_buffer_size = sizeof(uint32_t);
	constexpr VkDeviceSize output_buffer_size = sizeof(uint32_t);
	constexpr uint32_t input_value = 0x12345678;
	constexpr VkDeviceAddress capture_address = 0x440100000ull;

	VkBuffer_index.clear();
	VkBuffer_index.resize(2);
	init_buffer(0, source_buffer_size);
	init_buffer(1, output_buffer_size);
	VkBuffer_index[0].usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[0].capture_device_address = capture_address;
	VkBuffer_index[0].device_address = capture_address;

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);

	suballoc_location source_buffer = allocator.find_buffer_memory(0);
	suballoc_location output_buffer = allocator.find_buffer_memory(1);
	std::memset(source_buffer.memory, 0, source_buffer.size);
	std::memset(output_buffer.memory, 0, output_buffer.size);
	store_u32(source_buffer, 0, input_value);
	const change_source source = make_source(31);
	VkBuffer_index[0].source.register_source(0, source_buffer_size, source);

	init_compute_pipeline(3, bda_push_read_shader_code());

	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;
	trackedcommand push_constants { VKCMDPUSHCONSTANTS };
	push_constants.source = make_source(32);
	push_constants.data.push_constants.offset = 0;
	push_constants.data.push_constants.size = sizeof(VkDeviceAddress);
	push_constants.data.push_constants.values = static_cast<char*>(std::malloc(sizeof(VkDeviceAddress)));
	assert(push_constants.data.push_constants.values);
	std::memcpy(push_constants.data.push_constants.values, &capture_address, sizeof(capture_address));
	cmdbuffer_data.commands.push_back(push_constants);
	add_bind_compute_pipeline(cmdbuffer_data);
	add_dispatch(cmdbuffer_data, make_source(33));

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	device_address_remapping.add(capture_address, &VkBuffer_index[0]);
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};
	data.descriptorsets[0][0] = { &VkBuffer_index[1], 0, output_buffer_size };

	assert(!VkBuffer_index[0].is_state(trackedobject::states::bound));
	assert(execute_commands(data));
	assert(load_u32(output_buffer, 0) == input_value);
	assert(VkPipeline_index[0].shader_stages[0].calls == 1);
	assert(data.stats.commands == 3);
	assert(data.stats.execution_commands == 1);

	change_source copied_source;
	assert(VkBuffer_index[1].source.try_get_source(0, output_buffer_size, copied_source));
	assert(same_source(copied_source, source));
	assert(global_output_rewrite_queue.size() == 1);
	const address_rewrite& rewrite = global_output_rewrite_queue.front();
	assert(same_source(rewrite.source, push_constants.source));
	assert(rewrite.markings);
	assert(rewrite.markings->count == 1);
	assert(rewrite.markings->pMarkingTypes[0] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pOffsets[0] == 0);
	assert(descriptor_buffer_payloads.empty());
	for (address_rewrite& entry : global_output_rewrite_queue)
	{
		free_marked_offsets(entry.markings);
	}

	allocator.destroy();
}

static void execute_compute_shader_bda_copied_address_chain()
{
	constexpr VkDeviceSize address_buffer_size = sizeof(VkDeviceAddress);
	constexpr VkDeviceSize output_buffer_size = sizeof(uint32_t);
	constexpr uint32_t target_value = 0xaabbccdd;
	constexpr VkDeviceAddress target_address = 0x660100000ull;

	VkBuffer_index.clear();
	VkBuffer_index.resize(4);
	init_buffer(0, address_buffer_size);
	init_buffer(1, address_buffer_size);
	init_buffer(2, output_buffer_size);
	init_buffer(3, output_buffer_size);
	VkBuffer_index[0].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[2].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[3].usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[3].capture_device_address = target_address;
	VkBuffer_index[3].device_address = target_address;

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);
	allocator.add_trackedobject(0, 3, VkBuffer_index[2]);
	allocator.add_trackedobject(0, 4, VkBuffer_index[3]);

	suballoc_location original_address = allocator.find_buffer_memory(0);
	suballoc_location copied_address = allocator.find_buffer_memory(1);
	suballoc_location output = allocator.find_buffer_memory(2);
	suballoc_location target = allocator.find_buffer_memory(3);
	std::memset(original_address.memory, 0, original_address.size);
	std::memset(copied_address.memory, 0, copied_address.size);
	std::memset(output.memory, 0, output.size);
	std::memset(target.memory, 0, target.size);
	store_u32(target, 0, target_value);

	VkDescriptorSet_index.clear();
	VkDescriptorSet_index.resize(2);
	VkDescriptorSet_index[0].bound_buffers[0] = { &VkBuffer_index[0], 0, address_buffer_size };
	VkDescriptorSet_index[0].bound_buffers[1] = { &VkBuffer_index[1], 0, address_buffer_size };
	VkDescriptorSet_index[1].bound_buffers[0] = { &VkBuffer_index[1], 0, address_buffer_size };
	VkDescriptorSet_index[1].bound_buffers[1] = { &VkBuffer_index[2], 0, output_buffer_size };

	VkPipeline_index.clear();
	const uint32_t copy_pipeline_index = add_compute_pipeline(4, plain_copy_shader_code());
	const uint32_t bda_pipeline_index = add_compute_pipeline(5, bda_buffer_read_shader_code());

	const change_source update_source = make_source(41);
	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;

	trackedcommand update_address { VKCMDUPDATEBUFFER };
	update_address.source = update_source;
	update_address.data.update_buffer.offset = 0;
	update_address.data.update_buffer.size = sizeof(VkDeviceAddress);
	update_address.data.update_buffer.buffer_index = 0;
	update_address.data.update_buffer.values = static_cast<char*>(std::malloc(sizeof(VkDeviceAddress)));
	assert(update_address.data.update_buffer.values);
	std::memcpy(update_address.data.update_buffer.values, &target_address, sizeof(target_address));
	cmdbuffer_data.commands.push_back(update_address);

	add_bind_descriptorset(cmdbuffer_data, 0);
	add_bind_compute_pipeline(cmdbuffer_data, copy_pipeline_index);
	add_dispatch(cmdbuffer_data, make_source(42));
	add_bind_descriptorset(cmdbuffer_data, 1);
	add_bind_compute_pipeline(cmdbuffer_data, bda_pipeline_index);
	add_dispatch(cmdbuffer_data, make_source(43));

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	device_address_remapping.add(target_address, &VkBuffer_index[3]);
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};

	assert(execute_commands(data));
	VkDeviceAddress copied_value = 0;
	std::memcpy(&copied_value, copied_address.memory, sizeof(copied_value));
	assert(copied_value == target_address);
	assert(load_u32(output, 0) == target_value);
	assert(VkPipeline_index[copy_pipeline_index].shader_stages[0].calls == 1);
	assert(VkPipeline_index[bda_pipeline_index].shader_stages[0].calls == 1);
	assert(data.stats.commands == 7);
	assert(data.stats.execution_commands == 2);

	change_source copied_source;
	assert(VkBuffer_index[1].source.try_get_source(0, address_buffer_size, copied_source));
	assert(same_source(copied_source, update_source));
	assert(global_output_rewrite_queue.size() == 1);
	const address_rewrite& rewrite = global_output_rewrite_queue.front();
	assert(same_source(rewrite.source, update_source));
	assert(rewrite.object_type == VK_OBJECT_TYPE_UNKNOWN);
	assert(rewrite.object_index == CONTAINER_NULL_VALUE);
	assert(rewrite.markings);
	assert(rewrite.markings->count == 1);
	assert(rewrite.markings->pMarkingTypes[0] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pOffsets[0] == 0);
	assert(descriptor_buffer_payloads.empty());
	for (address_rewrite& entry : global_output_rewrite_queue)
	{
		free_marked_offsets(entry.markings);
	}

	allocator.destroy();
}

static void execute_compute_shader_bda_two_lane_output_provenance()
{
	constexpr VkDeviceSize address_buffer_size = sizeof(VkDeviceAddress) * 2;
	constexpr VkDeviceSize color_buffer_size = sizeof(uint32_t) * 4;
	constexpr VkDeviceSize output_buffer_size = sizeof(uint32_t) * 4;
	constexpr VkDeviceAddress output_address = 0x770100000ull;
	constexpr uint32_t color0_lo = 0x12340000;
	constexpr uint32_t color0_hi = 0x56780000;
	constexpr uint32_t color1_lo = 0x12340001;
	constexpr uint32_t color1_hi = 0x56780001;

	VkBuffer_index.clear();
	VkBuffer_index.resize(3);
	init_buffer(0, address_buffer_size);
	init_buffer(1, color_buffer_size);
	init_buffer(2, output_buffer_size);
	VkBuffer_index[0].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[2].usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[2].capture_device_address = output_address;
	VkBuffer_index[2].device_address = output_address;

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);
	allocator.add_trackedobject(0, 3, VkBuffer_index[2]);

	suballoc_location address_buffer = allocator.find_buffer_memory(0);
	suballoc_location color_buffer = allocator.find_buffer_memory(1);
	suballoc_location output_buffer = allocator.find_buffer_memory(2);
	std::memset(address_buffer.memory, 0, address_buffer.size);
	std::memset(color_buffer.memory, 0, color_buffer.size);
	std::memset(output_buffer.memory, 0, output_buffer.size);

	const VkDeviceAddress address0 = output_address;
	const VkDeviceAddress address1 = output_address + sizeof(VkDeviceAddress);
	std::memcpy(address_buffer.memory, &address0, sizeof(address0));
	std::memcpy(reinterpret_cast<char*>(address_buffer.memory) + sizeof(VkDeviceAddress), &address1, sizeof(address1));
	store_u32(color_buffer, 0, color0_lo);
	store_u32(color_buffer, sizeof(uint32_t), color0_hi);
	store_u32(color_buffer, sizeof(uint32_t) * 2, color1_lo);
	store_u32(color_buffer, sizeof(uint32_t) * 3, color1_hi);

	const change_source address_source = make_source(51);
	const change_source color_source = make_source(52);
	VkBuffer_index[0].source.register_source(0, address_buffer_size, address_source, 1, 0, VK_OBJECT_TYPE_BUFFER, 0);
	VkBuffer_index[1].source.register_source(0, color_buffer_size, color_source, 1, 0, VK_OBJECT_TYPE_BUFFER, 1);

	VkDescriptorSet_index.clear();
	VkDescriptorSet_index.resize(1);
	VkDescriptorSet_index[0].bound_buffers[0] = { &VkBuffer_index[0], 0, address_buffer_size };
	VkDescriptorSet_index[0].bound_buffers[1] = { &VkBuffer_index[1], 0, color_buffer_size };

	init_compute_pipeline(6, bda_two_store_shader_code());

	trackedcmdbuffer cmdbuffer_data;
	cmdbuffer_data.index = 0;
	add_bind_descriptorset(cmdbuffer_data, 0);
	add_bind_compute_pipeline(cmdbuffer_data);
	add_dispatch(cmdbuffer_data, make_source(53));

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	device_address_remapping.add(output_address, &VkBuffer_index[2]);
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	command_execution_data data {
		.device_data = device_data,
		.cmdbuffer_data = cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};

	assert(execute_commands(data));
	assert(load_u32(output_buffer, 0) == color0_lo);
	assert(load_u32(output_buffer, sizeof(uint32_t)) == color0_hi);
	assert(load_u32(output_buffer, sizeof(uint32_t) * 2) == color1_lo);
	assert(load_u32(output_buffer, sizeof(uint32_t) * 3) == color1_hi);
	assert(VkPipeline_index[0].shader_stages[0].calls == 1);
	assert(data.stats.commands == 3);
	assert(data.stats.execution_commands == 1);

	change_source output_source0;
	change_source output_source1;
	assert(VkBuffer_index[2].source.try_get_source(0, sizeof(VkDeviceAddress), output_source0));
	assert(VkBuffer_index[2].source.try_get_source(sizeof(VkDeviceAddress), sizeof(VkDeviceAddress), output_source1));
	assert(same_source(output_source0, color_source));
	assert(same_source(output_source1, color_source));
	assert(global_output_rewrite_queue.size() == 1);
	const address_rewrite& rewrite = global_output_rewrite_queue.front();
	assert(same_source(rewrite.source, address_source));
	assert(rewrite.object_type == VK_OBJECT_TYPE_UNKNOWN);
	assert(rewrite.object_index == CONTAINER_NULL_VALUE);
	assert(rewrite.markings);
	assert(rewrite.markings->count == 2);
	assert(rewrite.markings->pMarkingTypes[0] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pMarkingTypes[1] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pOffsets[0] == 0);
	assert(rewrite.markings->pOffsets[1] == sizeof(VkDeviceAddress));
	assert(descriptor_buffer_payloads.empty());
	for (address_rewrite& entry : global_output_rewrite_queue)
	{
		free_marked_offsets(entry.markings);
	}

	allocator.destroy();
}

static void execute_compute_shader_bda_interleave_copied_address_provenance()
{
	constexpr VkDeviceSize address_buffer_size = sizeof(VkDeviceAddress) * 2;
	constexpr VkDeviceSize color_buffer_size = sizeof(uint32_t) * 4;
	constexpr VkDeviceSize interleave_entry_size = sizeof(uint32_t) * 4;
	constexpr VkDeviceSize interleave_color_offset = 0;
	constexpr VkDeviceSize interleave_address_offset = sizeof(uint32_t) * 2;
	constexpr VkDeviceSize interleave_buffer_size = interleave_entry_size * 2;
	constexpr VkDeviceSize output_buffer_size = sizeof(uint32_t) * 4;
	constexpr VkDeviceAddress output_address = 0x880100000ull;
	constexpr uint32_t color0_lo = 0x22340000;
	constexpr uint32_t color0_hi = 0x66780000;
	constexpr uint32_t color1_lo = 0x22340001;
	constexpr uint32_t color1_hi = 0x66780001;

	VkBuffer_index.clear();
	VkBuffer_index.resize(4);
	init_buffer(0, address_buffer_size);
	init_buffer(1, color_buffer_size);
	init_buffer(2, interleave_buffer_size);
	init_buffer(3, output_buffer_size);
	VkBuffer_index[0].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[1].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[2].usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[3].usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer_index[3].capture_device_address = output_address;
	VkBuffer_index[3].device_address = output_address;

	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackeddatagraphpipelinesession> sessions;
	suballocator allocator;
	allocator.create(VK_NULL_HANDLE, VK_NULL_HANDLE, images, VkBuffer_index, tensors, sessions, 1, false);
	allocator.add_trackedobject(0, 1, VkBuffer_index[0]);
	allocator.add_trackedobject(0, 2, VkBuffer_index[1]);
	allocator.add_trackedobject(0, 3, VkBuffer_index[2]);
	allocator.add_trackedobject(0, 4, VkBuffer_index[3]);

	suballoc_location address_buffer = allocator.find_buffer_memory(0);
	suballoc_location color_buffer = allocator.find_buffer_memory(1);
	suballoc_location interleave_buffer = allocator.find_buffer_memory(2);
	suballoc_location output_buffer = allocator.find_buffer_memory(3);
	std::memset(address_buffer.memory, 0, address_buffer.size);
	std::memset(color_buffer.memory, 0, color_buffer.size);
	std::memset(interleave_buffer.memory, 0, interleave_buffer.size);
	std::memset(output_buffer.memory, 0, output_buffer.size);

	const VkDeviceAddress address0 = output_address;
	const VkDeviceAddress address1 = output_address + sizeof(VkDeviceAddress);
	std::memcpy(address_buffer.memory, &address0, sizeof(address0));
	std::memcpy(reinterpret_cast<char*>(address_buffer.memory) + sizeof(VkDeviceAddress), &address1, sizeof(address1));
	store_u32(color_buffer, 0, color0_lo);
	store_u32(color_buffer, sizeof(uint32_t), color0_hi);
	store_u32(color_buffer, sizeof(uint32_t) * 2, color1_lo);
	store_u32(color_buffer, sizeof(uint32_t) * 3, color1_hi);

	const change_source address_source = make_source(61);
	const change_source color_source = make_source(62);
	VkBuffer_index[0].source.register_source(0, address_buffer_size, address_source, 1, 0, VK_OBJECT_TYPE_BUFFER, 0);
	VkBuffer_index[1].source.register_source(0, color_buffer_size, color_source, 1, 0, VK_OBJECT_TYPE_BUFFER, 1);

	VkPipeline_index.clear();
	const uint32_t interleave_pipeline_index = add_compute_pipeline(7, bda_interleave_copy_shader_code());
	const uint32_t output_pipeline_index = add_compute_pipeline(8, bda_interleave_output_shader_code());

	trackeddevice device_data;
	device_data.index = 0;
	device_data.allocator = &allocator;

	address_remapper<trackedobject> device_address_remapping;
	device_address_remapping.add(output_address, &VkBuffer_index[3]);
	std::list<address_rewrite> global_output_rewrite_queue;
	std::deque<descriptor_rewrite> pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload> descriptor_buffer_payloads;

	trackedcmdbuffer interleave_cmdbuffer_data;
	interleave_cmdbuffer_data.index = 0;
	add_bind_compute_pipeline(interleave_cmdbuffer_data, interleave_pipeline_index);
	add_dispatch(interleave_cmdbuffer_data, make_source(63));

	command_execution_data interleave_data {
		.device_data = device_data,
		.cmdbuffer_data = interleave_cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};
	interleave_data.descriptorsets[0][0] = { &VkBuffer_index[0], 0, address_buffer_size };
	interleave_data.descriptorsets[0][1] = { &VkBuffer_index[1], 0, color_buffer_size };
	interleave_data.descriptorsets[0][2] = { &VkBuffer_index[2], 0, interleave_buffer_size };

	assert(execute_commands(interleave_data));
	assert(load_u32(interleave_buffer, 0) == color0_lo);
	assert(load_u32(interleave_buffer, sizeof(uint32_t)) == color0_hi);
	assert(load_u32(interleave_buffer, interleave_address_offset) == (uint32_t)address0);
	assert(load_u32(interleave_buffer, interleave_address_offset + sizeof(uint32_t)) == (uint32_t)(address0 >> 32));
	assert(load_u32(interleave_buffer, interleave_entry_size) == color1_lo);
	assert(load_u32(interleave_buffer, interleave_entry_size + sizeof(uint32_t)) == color1_hi);
	assert(load_u32(interleave_buffer, interleave_entry_size + interleave_address_offset) == (uint32_t)address1);
	assert(load_u32(interleave_buffer, interleave_entry_size + interleave_address_offset + sizeof(uint32_t)) == (uint32_t)(address1 >> 32));
	assert(VkPipeline_index[interleave_pipeline_index].shader_stages[0].calls == 1);
	assert(interleave_data.stats.commands == 2);
	assert(interleave_data.stats.execution_commands == 1);

	change_source interleave_color_source0;
	change_source interleave_color_source1;
	change_source interleave_address_source0;
	change_source interleave_address_source1;
	assert(VkBuffer_index[2].source.try_get_source(interleave_color_offset, sizeof(VkDeviceAddress), interleave_color_source0));
	assert(VkBuffer_index[2].source.try_get_source(interleave_entry_size + interleave_color_offset, sizeof(VkDeviceAddress), interleave_color_source1));
	assert(VkBuffer_index[2].source.try_get_source(interleave_address_offset, sizeof(VkDeviceAddress), interleave_address_source0));
	assert(VkBuffer_index[2].source.try_get_source(interleave_entry_size + interleave_address_offset, sizeof(VkDeviceAddress), interleave_address_source1));
	assert(same_source(interleave_color_source0, color_source));
	assert(same_source(interleave_color_source1, color_source));
	assert(same_source(interleave_address_source0, address_source));
	assert(same_source(interleave_address_source1, address_source));
	assert(global_output_rewrite_queue.empty());

	trackedcmdbuffer output_cmdbuffer_data;
	output_cmdbuffer_data.index = 0;
	add_bind_compute_pipeline(output_cmdbuffer_data, output_pipeline_index);
	add_dispatch(output_cmdbuffer_data, make_source(64));

	command_execution_data output_data {
		.device_data = device_data,
		.cmdbuffer_data = output_cmdbuffer_data,
		.device_address_remapping = device_address_remapping,
		.global_output_rewrite_queue = global_output_rewrite_queue,
		.pending_descriptor_rewrites = pending_descriptor_rewrites,
		.descriptor_buffer_payloads = descriptor_buffer_payloads,
	};
	output_data.descriptorsets[0][0] = { &VkBuffer_index[2], 0, interleave_buffer_size };

	assert(execute_commands(output_data));
	assert(load_u32(output_buffer, 0) == color0_lo);
	assert(load_u32(output_buffer, sizeof(uint32_t)) == color0_hi);
	assert(load_u32(output_buffer, sizeof(uint32_t) * 2) == color1_lo);
	assert(load_u32(output_buffer, sizeof(uint32_t) * 3) == color1_hi);
	assert(VkPipeline_index[output_pipeline_index].shader_stages[0].calls == 1);
	assert(output_data.stats.commands == 2);
	assert(output_data.stats.execution_commands == 1);

	change_source output_source0;
	change_source output_source1;
	assert(VkBuffer_index[3].source.try_get_source(0, sizeof(VkDeviceAddress), output_source0));
	assert(VkBuffer_index[3].source.try_get_source(sizeof(VkDeviceAddress), sizeof(VkDeviceAddress), output_source1));
	assert(same_source(output_source0, color_source));
	assert(same_source(output_source1, color_source));
	assert(global_output_rewrite_queue.size() == 1);
	const address_rewrite& rewrite = global_output_rewrite_queue.front();
	assert(same_source(rewrite.source, address_source));
	assert(rewrite.object_type == VK_OBJECT_TYPE_UNKNOWN);
	assert(rewrite.object_index == CONTAINER_NULL_VALUE);
	assert(rewrite.markings);
	assert(rewrite.markings->count == 2);
	assert(rewrite.markings->pMarkingTypes[0] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pMarkingTypes[1] == VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
	assert(rewrite.markings->pOffsets[0] == 0);
	assert(rewrite.markings->pOffsets[1] == sizeof(VkDeviceAddress));
	assert(descriptor_buffer_payloads.empty());
	for (address_rewrite& entry : global_output_rewrite_queue)
	{
		free_marked_offsets(entry.markings);
	}

	allocator.destroy();
}

int main(int argc, char** argv)
{
	unsigned perf_run = 0;
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
		else if (match(argv[i], "-p", "--perf", remaining))
		{
			perf_run = get_int(argv[++i], remaining);
		}
	}

	execute_null();
	execute_copy_buffer();
	execute_compute_shader();
	execute_compute_shader_copy_provenance();
	execute_compute_shader_bda_unbound_input();
	execute_compute_shader_bda_copied_address_chain();
	execute_compute_shader_bda_two_lane_output_provenance();
	execute_compute_shader_bda_interleave_copied_address_provenance();

	if (perf_run > 0)
	{
		compute_shader_fixture fixture;
		const auto start = std::chrono::steady_clock::now();
		for (unsigned i = 0; i < perf_run; i++)
		{
			const bool r = execute_commands(fixture.data);
			assert(r);
		}
		const auto end = std::chrono::steady_clock::now();
		assert(fixture.output_value() == compute_shader_expected_output);
		assert(fixture.stage().calls == perf_run);
		assert(fixture.data.stats.commands == (int)(perf_run * fixture.cmdbuffer_data.commands.size()));
		assert(fixture.data.stats.execution_commands == (int)perf_run);
		const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
		printf("Simulator initialization:   %10llu\n", (unsigned long long)fixture.data.stats.total_init_time);
		printf("Simulator run time:         %10llu\n", (unsigned long long)fixture.data.stats.total_spirv_run_time);
		printf("Combined simulator time:    %10llu\n", (unsigned long long)fixture.data.stats.total_spirv_run_time + fixture.data.stats.total_init_time);
		printf("Total run time:             %10llu\n", (unsigned long long)ns);
	}

	return 0;
}
