// Exercise VK_EXT_headless_surface with a swapchain.

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

static bool has_device_extension(VkPhysicalDevice physical, const char* name)
{
	uint32_t propertyCount = 0;
	VkResult result = trace_vkEnumerateDeviceExtensionProperties(physical, nullptr, &propertyCount, nullptr);
	if (result != VK_SUCCESS || propertyCount == 0)
	{
		return false;
	}
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = trace_vkEnumerateDeviceExtensionProperties(physical, nullptr, &propertyCount, supported_extensions.data());
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

static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkCompositeAlphaFlagsKHR supported)
{
	if (supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
	if (supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
}

static VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes)
{
	for (const VkPresentModeKHR mode : modes)
	{
		if (mode == VK_PRESENT_MODE_FIFO_KHR) return mode;
	}
	for (const VkPresentModeKHR mode : modes)
	{
		if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
	}
	for (const VkPresentModeKHR mode : modes)
	{
		if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
	}
	return modes[0];
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
	app.pApplicationName = "tracing_headless2";
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.pEngineName = "lavatube-tests";
	app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	app.apiVersion = VK_API_VERSION_1_1;

	std::vector<const char*> instance_extensions;
	instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	instance_extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);

	VkInstanceCreateInfo instance_info = {};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app;
	instance_info.enabledExtensionCount = instance_extensions.size();
	instance_info.ppEnabledExtensionNames = instance_extensions.data();

	VkInstance instance = VK_NULL_HANDLE;
	VkResult result = trace_vkCreateInstance(&instance_info, nullptr, &instance);
	check(result);

	uint32_t num_devices = 0;
	result = trace_vkEnumeratePhysicalDevices(instance, &num_devices, nullptr);
	check(result);
	if (num_devices == 0)
	{
		printf("SKIP: No physical devices found.\n");
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}
	std::vector<VkPhysicalDevice> physical_devices(num_devices);
	result = trace_vkEnumeratePhysicalDevices(instance, &num_devices, physical_devices.data());
	check(result);
	VkPhysicalDevice physical = physical_devices[0];

	if (!has_device_extension(physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
	{
		printf("SKIP: VK_KHR_swapchain not available on this device.\n");
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}

	VkHeadlessSurfaceCreateInfoEXT surface_info = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT, nullptr };
	surface_info.flags = 0;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	result = trace_vkCreateHeadlessSurfaceEXT(instance, &surface_info, nullptr, &surface);
	check(result);

	uint32_t queue_family_count = 0;
	trace_vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_family_count, nullptr);
	std::vector<VkQueueFamilyProperties> queue_props(queue_family_count);
	trace_vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_family_count, queue_props.data());

	uint32_t queue_family_index = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_count; i++)
	{
		if (queue_props[i].queueCount == 0) continue;
		if ((queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
		VkBool32 present = VK_FALSE;
		trace_vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
		if (present)
		{
			queue_family_index = i;
			break;
		}
	}

	if (queue_family_index == UINT32_MAX)
	{
		printf("SKIP: No present-capable graphics queue for headless surface.\n");
		trace_vkDestroySurfaceKHR(instance, surface, nullptr);
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}

	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = {};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = queue_family_index;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &queue_priority;

	const char* device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo device_info = {};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 1;
	device_info.ppEnabledExtensionNames = device_extensions;

	VkDevice device = VK_NULL_HANDLE;
	result = trace_vkCreateDevice(physical, &device_info, nullptr, &device);
	check(result);

	VkSurfaceCapabilitiesKHR caps = {};
	result = trace_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
	check(result);

	uint32_t format_count = 0;
	result = trace_vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
	check(result);
	if (format_count == 0)
	{
		printf("SKIP: No surface formats reported.\n");
		trace_vkDestroyDevice(device, nullptr);
		trace_vkDestroySurfaceKHR(instance, surface, nullptr);
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	result = trace_vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
	check(result);
	VkSurfaceFormatKHR chosen_format = formats[0];
	for (const VkSurfaceFormatKHR& f : formats)
	{
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			chosen_format = f;
			break;
		}
	}

	uint32_t present_count = 0;
	result = trace_vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count, nullptr);
	check(result);
	if (present_count == 0)
	{
		printf("SKIP: No present modes reported.\n");
		trace_vkDestroyDevice(device, nullptr);
		trace_vkDestroySurfaceKHR(instance, surface, nullptr);
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}
	std::vector<VkPresentModeKHR> present_modes(present_count);
	result = trace_vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count, present_modes.data());
	check(result);
	VkPresentModeKHR present_mode = choose_present_mode(present_modes);

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX || extent.height == UINT32_MAX || extent.width == 0 || extent.height == 0)
	{
		extent.width = 64;
		extent.height = 64;
	}

	uint32_t image_count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
	{
		image_count = caps.maxImageCount;
	}

	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((caps.supportedUsageFlags & usage) == 0)
	{
		usage = caps.supportedUsageFlags;
	}
	if (usage == 0)
	{
		printf("SKIP: No supported image usage flags.\n");
		trace_vkDestroyDevice(device, nullptr);
		trace_vkDestroySurfaceKHR(instance, surface, nullptr);
		trace_vkDestroyInstance(instance, nullptr);
		return 0;
	}

	VkSwapchainCreateInfoKHR swapchain_info = {};
	swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_info.surface = surface;
	swapchain_info.minImageCount = image_count;
	swapchain_info.imageFormat = chosen_format.format;
	swapchain_info.imageColorSpace = chosen_format.colorSpace;
	swapchain_info.imageExtent = extent;
	swapchain_info.imageArrayLayers = 1;
	swapchain_info.imageUsage = usage;
	swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_info.preTransform = caps.currentTransform;
	swapchain_info.compositeAlpha = choose_composite_alpha(caps.supportedCompositeAlpha);
	swapchain_info.presentMode = present_mode;
	swapchain_info.clipped = VK_TRUE;
	swapchain_info.oldSwapchain = VK_NULL_HANDLE;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	result = trace_vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain);
	check(result);
	assert(swapchain != VK_NULL_HANDLE);

	uint32_t swapchain_image_count = 0;
	result = trace_vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, nullptr);
	check(result);
	assert(swapchain_image_count > 0);
	std::vector<VkImage> swapchain_images(swapchain_image_count);
	result = trace_vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, swapchain_images.data());
	check(result);

	trace_vkDestroySwapchainKHR(device, swapchain, nullptr);
	trace_vkDestroyDevice(device, nullptr);
	trace_vkDestroySurfaceKHR(instance, surface, nullptr);
	trace_vkDestroyInstance(instance, nullptr);
	return 0;
}
