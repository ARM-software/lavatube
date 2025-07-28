#include <thread>

#include "tests/common.h"
#include "util_auto.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#define TEST_NAME_3 "tracing_3"
#define NUM_BUFFERS 48

#define THREADS 20
static lava_reader* reader = nullptr;
static std::atomic_int read_tid;
static lava_writer& writer = lava_writer::instance();
static std::atomic_int used[THREADS + 1];
static vulkan_setup_t vulkan;
static std::atomic_int mCallNo;

static void thread_test_stress()
{
	if (random() % 5 == 1) usleep(random() % 3 * 10000); // introduce some pseudo-random timings
	set_thread_name("stress thread");
	lava_file_writer& file = writer.file_writer();
	int tid = file.thread_index();
	assert(tid < THREADS + 1);
	assert(used[tid] == 0);
	used[tid] = 1;
	assert(vulkan.device != VK_NULL_HANDLE);

	VkCommandPool cmdpool;
	VkCommandPoolCreateInfo cmdcreateinfo = {};
	cmdcreateinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdcreateinfo.flags = 0;
	cmdcreateinfo.queueFamilyIndex = 0;
	VkResult result = trace_vkCreateCommandPool(vulkan.device, &cmdcreateinfo, nullptr, &cmdpool);
	check(result);
	std::string tmpstr = "Our temporary command pool for tid " + _to_string(tid);
	test_set_name(vulkan.device, VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)cmdpool, tmpstr.c_str());

	if (random() % 5 == 1) usleep(random() % 3 * 10000); // introduce some pseudo-random timings

	std::vector<VkCommandBuffer> cmdbuffers(10);
	VkCommandBufferAllocateInfo pAllocateInfo = {};
	pAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	pAllocateInfo.commandBufferCount = 10;
	pAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	pAllocateInfo.commandPool = cmdpool;
	pAllocateInfo.pNext = nullptr;
	result = trace_vkAllocateCommandBuffers(vulkan.device, &pAllocateInfo, cmdbuffers.data());
	check(result);

	if (random() % 5 == 1) usleep(random() % 3 * 10000); // introduce some pseudo-random timings

	trace_vkFreeCommandBuffers(vulkan.device, cmdpool, cmdbuffers.size(), cmdbuffers.data());
	trace_vkDestroyCommandPool(vulkan.device, cmdpool, nullptr);

	if (random() % 5 == 1) usleep(random() % 3 * 10000); // introduce some pseudo-random timings
}

static void trace_3()
{
	vulkan_req_t reqs;
	vulkan = test_init(TEST_NAME_3, reqs);

	std::vector<std::thread*> threads(THREADS);
	for (auto& t : threads)
	{
		t = new std::thread(thread_test_stress);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();

	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	const uint8_t instrtype = t.step();
	if (instrtype == PACKET_VULKAN_API_CALL)
	{
		assert(instrtype == 2);
		const uint16_t apicall = t.read_apicall();
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else if (instrtype != 0) ABORT("Unexpected packet type %d in thread %d", (int)instrtype, (int)t.thread_index());
	t.parent->allocator.self_test();
	return (instrtype != 0);
}

void read_test_stress()
{
	const int mytid = read_tid.fetch_add(1);
	lava_file_reader& r = reader->file_reader(mytid);
	while (getnext(r)) {}
}

void read_test()
{
	mCallNo.store(0);
	read_tid = 0;
	reader = new lava_reader(TEST_NAME_3 ".vk");
	std::vector<std::thread*> threads(THREADS + 1); // main thread + 20 helper threads
	for (auto& t : threads)
	{
		t = new std::thread(read_test_stress);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();
	int remaining = reader->allocator.self_test();
	assert(remaining == 0); // everything should be destroyed now
	delete reader;
	reader = nullptr;
}

int main()
{
	trace_3();
	read_test();
	return 0;
}
