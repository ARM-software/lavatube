#include "tests/common.h"

int main()
{
	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_3;
	vulkan_setup_t vulkan = test_init("tracing_null_memory", reqs);
	trace_vkFreeMemory(vulkan.device, VK_NULL_HANDLE, nullptr);
	test_done(vulkan);
	return 0;
}
