// Basic coverage for VK_EXT_headless_surface. Skips if the extension is not available.

#include <vector>
#include <string>

#include "vulkan/vulkan.h"
#include "util.h"
#include "write_auto.h"

#include "tests/tests.h"

#define check(result) \
	if (result != VK_SUCCESS) \
	{ \
		fprintf(stderr, "Error 0x%04x: %s\n", result, errorString(result)); \
	} \
	assert(result == VK_SUCCESS);

static bool has_instance_extension(const char* name)
{
	uint32_t propertyCount = 0;
	VkResult result = trace_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr);
	if (result != VK_SUCCESS || propertyCount == 0)
	{
		return false;
	}
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = trace_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, supported_extensions.data());
	if (result != VK_SUCCESS)
	{
		return false;
	}
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, name) == 0) return true;
	}
	return false;
}

int main()
{
	if (!has_instance_extension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
	{
		printf("SKIP: VK_EXT_headless_surface not available on this system.\n");
		return 77;
	}

	if (!has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME))
	{
		printf("SKIP: VK_KHR_surface not available on this system.\n");
		return 77;
	}

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "tracing_headless";
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.pEngineName = "lavatube-tests";
	app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	app.apiVersion = VK_API_VERSION_1_1;

	std::vector<const char*> enabledExtensions;
	enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	enabledExtensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);

	VkInstanceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &app;
	createInfo.enabledExtensionCount = enabledExtensions.size();
	createInfo.ppEnabledExtensionNames = enabledExtensions.data();

	VkInstance instance = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateInstance(&createInfo, nullptr, &instance);
	check(result);

	VkHeadlessSurfaceCreateInfoEXT surfaceInfo = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT, nullptr };
	surfaceInfo.flags = 0;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	result = trace_vkCreateHeadlessSurfaceEXT(instance, &surfaceInfo, nullptr, &surface);
	check(result);
	assert(surface != VK_NULL_HANDLE);

	trace_vkDestroySurfaceKHR(instance, surface, nullptr);
	trace_vkDestroyInstance(instance, nullptr);
	return 0;
}
