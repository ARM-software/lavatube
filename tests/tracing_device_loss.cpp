#include "tests/common.h"
#include "write.h"

int main()
{
	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init("tracing_device_loss", reqs);

	VkQueue queue = VK_NULL_HANDLE;
	trace_vkGetDeviceQueue(vulkan.device, 0, 0, &queue);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	VkResult result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	check(result);
	result = trace_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	check(result);
	result = trace_vkQueueWaitIdle(queue);
	check(result);

	test_done(vulkan);
	return 0;
}
