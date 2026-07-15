#include "tests/common.h"
#include "packfile.h"

#define TEST_NAME "tracing_blacklist"

static bool has_extension(const std::vector<VkExtensionProperties>& properties, const char* name)
{
	for (const VkExtensionProperties& property : properties)
	{
		if (strcmp(property.extensionName, name) == 0) return true;
	}
	return false;
}

static std::vector<VkExtensionProperties> instance_extensions()
{
	uint32_t count = 0;
	check(trace_vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
	std::vector<VkExtensionProperties> properties(count);
	check(trace_vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()));
	properties.resize(count);
	return properties;
}

static std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice physical_device)
{
	uint32_t count = 0;
	check(trace_vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr));
	std::vector<VkExtensionProperties> properties(count);
	check(trace_vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, properties.data()));
	properties.resize(count);
	return properties;
}

static bool json_array_contains(const Json::Value& values, const char* name)
{
	for (const Json::Value& value : values)
	{
		if (value.asString() == name) return true;
	}
	return false;
}

int main()
{
	assert(!has_extension(instance_extensions(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME));

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.apiVersion = VK_API_VERSION_1_1;
	const char* rejected_instance_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	VkInstanceCreateInfo rejected_instance_info = {};
	rejected_instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	rejected_instance_info.pApplicationInfo = &app;
	rejected_instance_info.enabledExtensionCount = 1;
	rejected_instance_info.ppEnabledExtensionNames = &rejected_instance_extension;
	VkInstance rejected_instance = VK_NULL_HANDLE;
	assert(trace_vkCreateInstance(&rejected_instance_info, nullptr, &rejected_instance) == VK_ERROR_EXTENSION_NOT_PRESENT);
	assert(rejected_instance == VK_NULL_HANDLE);

	vulkan_req_t reqs;
	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	assert(!has_extension(device_extensions(vulkan.physical), VK_ARM_TRACE_HELPERS_EXTENSION_NAME));

	const char* rejected_device_extension = VK_ARM_TRACE_HELPERS_EXTENSION_NAME;
	VkDeviceCreateInfo rejected_device_info = {};
	rejected_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	rejected_device_info.enabledExtensionCount = 1;
	rejected_device_info.ppEnabledExtensionNames = &rejected_device_extension;
	VkDevice rejected_device = VK_NULL_HANDLE;
	assert(trace_vkCreateDevice(vulkan.physical, &rejected_device_info, nullptr, &rejected_device) == VK_ERROR_EXTENSION_NOT_PRESENT);
	assert(rejected_device == VK_NULL_HANDLE);

	test_done(vulkan);
	Json::Value metadata = packed_json("metadata.json", TEST_NAME ".api");
	assert(json_array_contains(metadata["blacklistedExtensions"], VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
	assert(json_array_contains(metadata["blacklistedExtensions"], VK_ARM_TRACE_HELPERS_EXTENSION_NAME));
	assert(!json_array_contains(metadata["instancePresented"]["extensions"], VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
	assert(!json_array_contains(metadata["devicePresented"]["extensions"], VK_ARM_TRACE_HELPERS_EXTENSION_NAME));
	return 0;
}
