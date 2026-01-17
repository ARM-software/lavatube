// Verify buffer memory requirements alignments match between trace and replay paths

#include "tests/common.h"
#include "memory.h"
#include "packfile.h"
#include "vk_wrapper_auto.h"
#include <stdio.h>
#include <assert.h>
#include <unordered_map>
#include <vector>

#define TEST_NAME "tracing_alignment"

static std::unordered_map<uint32_t, VkDeviceSize> trace_alignments;
static std::unordered_map<uint32_t, VkDeviceSize> replay_alignments;
static std::vector<uint32_t> buffer_indices;
static size_t replay_call_index = 0;

static trackedbuffer make_trackedbuffer(const VkBufferCreateInfo& info, uint32_t idx)
{
	trackedbuffer t;
	t.index = idx;
	t.size = info.size;
	t.flags = info.flags;
	t.sharingMode = info.sharingMode;
	t.usage = info.usage;
	t.object_type = VK_OBJECT_TYPE_BUFFER;
	t.enter_initialized();
	return t;
}

static void trace()
{
	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_3;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	VkResult result;

	trace_alignments.clear();
	replay_alignments.clear();
	buffer_indices.clear();

	const VkDeviceSize sizes[] = { 64, 4096 + 13, 1024 * 1024 + 7 };
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
	{
		info.size = sizes[i];
		VkBuffer buffer = VK_NULL_HANDLE;
		result = trace_vkCreateBuffer(vulkan.device, &info, nullptr, &buffer);
		check(result);
		char name[32];
		snprintf(name, sizeof(name), "alignment-buffer-%u", i);
		test_set_name(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, name);

		VkMemoryRequirements raw_req = {};
		wrap_vkGetBufferMemoryRequirements(vulkan.device, buffer, &raw_req);
		assert(raw_req.alignment != 0);

		const uint32_t index = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);

		VkMemoryRequirements traced_req = {};
		trace_vkGetBufferMemoryRequirements(vulkan.device, buffer, &traced_req);
		assert(traced_req.alignment == raw_req.alignment);

		trackedbuffer tracked = make_trackedbuffer(info, index);
		memory_requirements replay_req = get_trackedbuffer_memory_requirements(vulkan.device, tracked);
		assert(replay_req.requirements.alignment == raw_req.alignment);

		trace_alignments[index] = traced_req.alignment;
		buffer_indices.push_back(index);
		trace_vkDestroyBuffer(vulkan.device, buffer, nullptr);
		ILOG("Trace %d align=%d buffer=%lu", (int)index, (int)raw_req.alignment, (unsigned long)buffer);
	}
	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		const uint16_t apicall = t.read_apicall();
		if (apicall == 1) done = true; // vkDestroyInstance
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
	}
	else if (instrtype == PACKET_TENSOR_UPDATE)
	{
		update_tensor_packet(instrtype, t);
	}
	else assert(false);
	t.parent->allocator.self_test();
	return !done;
}

static void record_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements)
{
	(void)device;
	(void)buffer;
	assert(pMemoryRequirements != nullptr);
	assert(replay_call_index < buffer_indices.size());
	const uint32_t idx = buffer_indices.at(replay_call_index++);
	replay_alignments[idx] = pMemoryRequirements->alignment;
	ILOG("Found replay alignment idx=%d align=%d", (int)idx, (int)pMemoryRequirements->alignment);
}

static void retrace()
{
	// Verify trace file itself
	Json::Value tracking = packed_json("tracking.json", TEST_NAME ".vk");
	unsigned matched = 0;
	for (const auto& entry : tracking["VkBuffer"])
	{
		const uint32_t index = entry["index"].asUInt();
		assert(entry.isMember("req_alignment"));
		const VkDeviceSize recorded = entry["req_alignment"].asUInt();
		assert(recorded == trace_alignments.at(index));
		matched++;
	}
	assert(matched == trace_alignments.size());

	// Replay it
	lava_reader r(TEST_NAME ".vk");
	replay_call_index = 0;
	replay_alignments.clear();
	vkGetBufferMemoryRequirements_callbacks.push_back(record_vkGetBufferMemoryRequirements);
	lava_file_reader& t = r.file_reader(0);
	int remaining = r.allocator.self_test();
	assert(remaining == 0);
	while (getnext(t)) {}
	remaining = r.allocator.self_test();
	assert(remaining == 0);
	assert(replay_call_index == buffer_indices.size());
	assert(replay_alignments.size() == trace_alignments.size());

	// Check data we collected during run
	for (const auto& it : trace_alignments)
	{
		assert(replay_alignments.count(it.first) > 0);
		assert(replay_alignments.at(it.first) == it.second);
	}
	for (const auto& entry : tracking["VkBuffer"])
	{
		const uint32_t idx = entry["index"].asUInt();
		assert(replay_alignments.count(idx) > 0);
		assert(replay_alignments.at(idx) == trace_alignments.at(idx));
	}
}

int main()
{
	trace();
	retrace();
	return 0;
}
