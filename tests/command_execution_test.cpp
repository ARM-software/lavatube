#include "execute_commands.h"
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

void usage()
{
	printf("command_execution_test %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
#endif
	printf("-p/--perf RUNS         Run in performance mode, executing RUNS times\n");
}

static std::vector<uint32_t> compute_shader_code()
{
	assert(sizeof(command_execution_compute_spv) % sizeof(uint32_t) == 0);
	std::vector<uint32_t> code(sizeof(command_execution_compute_spv) / sizeof(uint32_t));
	std::memcpy(code.data(), command_execution_compute_spv, sizeof(command_execution_compute_spv));
	return code;
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

static change_source make_source(uint32_t call)
{
	change_source source;
	source.call = call;
	source.frame = 0;
	source.thread = 0;
	source.call_id = VKCMDUPDATEBUFFER;
	return source;
}

static bool same_source(const change_source& a, const change_source& b)
{
	return a.call == b.call && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
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

		VkPipeline_index.clear();
		VkPipeline_index.resize(1);
		trackedpipeline& pipeline = VkPipeline_index[0];
		pipeline.index = 0;
		pipeline.device_index = 0;
		pipeline.type = VK_PIPELINE_BIND_POINT_COMPUTE;
		pipeline.shader_stages.resize(1);
		shader_stage& stage = pipeline.shader_stages[0];
		stage.index = 0;
		stage.unique_index = 1;
		stage.device_index = 0;
		stage.flags = 0;
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.name = "main";
		stage.code = compute_shader_code();

		cmdbuffer_data.index = 0;
		trackedcommand bind_pipeline { VKCMDBINDPIPELINE };
		bind_pipeline.data.bind_pipeline.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
		bind_pipeline.data.bind_pipeline.pipeline_index = 0;
		cmdbuffer_data.commands.push_back(bind_pipeline);
		trackedcommand dispatch { VKCMDDISPATCH };
		dispatch.source = make_source(12);
		cmdbuffer_data.commands.push_back(dispatch);

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
