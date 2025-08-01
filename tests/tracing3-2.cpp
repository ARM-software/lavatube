#include <thread>
#include <mutex>
#include <unordered_set>

#include "tests/common.h"

#define THREADS 20
#define BUFFERS 20
static vulkan_setup_t vulkan;
static VkCommandPool pools[THREADS];
static VkCommandBuffer cmds[THREADS * BUFFERS];
static std::unordered_set<VkCommandBuffer> used;
static std::mutex order;

#pragma GCC diagnostic ignored "-Wunused-variable"

static void hack_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
{
	order.lock();
	trace_vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
	for (unsigned i = 0; i < commandBufferCount; i++)
	{
		used.erase(pCommandBuffers[i]);
	}
	order.unlock();
}

static VkResult hack_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{
	order.lock();
	trace_vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
	for (unsigned i = 0; i < pAllocateInfo->commandBufferCount; i++)
	{
		assert(used.count(pCommandBuffers[i]) == 0);
		used.insert(pCommandBuffers[i]);
	}
	order.unlock();
	return VK_SUCCESS;
}

static void thread_test_stress(VkCommandPool *cmdpool, VkCommandBuffer* cmdbuffers)
{
	VkCommandBufferAllocateInfo pAllocateInfo = {};
	pAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	pAllocateInfo.commandBufferCount = BUFFERS;
	pAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	pAllocateInfo.commandPool = *cmdpool;
	pAllocateInfo.pNext = nullptr;
	VkResult result = hack_vkAllocateCommandBuffers(vulkan.device, &pAllocateInfo, cmdbuffers);
	check(result);
	hack_vkFreeCommandBuffers(vulkan.device, *cmdpool, BUFFERS, cmdbuffers);
}

int main()
{
	vulkan_req_t reqs;
	vulkan = test_init("wtf", reqs);
	std::vector<std::thread*> threads(THREADS);
	for (int k = 0; k < THREADS; k++)
	{
		VkCommandPoolCreateInfo cmdcreateinfo = {};
		cmdcreateinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cmdcreateinfo.flags = 0;
		cmdcreateinfo.queueFamilyIndex = 0;
		VkResult result = trace_vkCreateCommandPool(vulkan.device, &cmdcreateinfo, nullptr, &pools[k]);
		check(result);
	}
	int k = 0;
	for (auto& t : threads)
	{

		t = new std::thread(thread_test_stress, &pools[k], &cmds[k * BUFFERS]);
		k++;
	}
	k = 0;
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
		trace_vkDestroyCommandPool(vulkan.device, pools[k], nullptr);
		k++;
	}
	threads.clear();
	test_done(vulkan);
	return 0;
}
