#include "tests/common.h"
#include "util.h"

static VkPhysicalDeviceMemoryProperties memory_properties = {};

static VkBool32 report_callback(
    VkDebugReportFlagsEXT                       flags,
    VkDebugReportObjectTypeEXT                  objectType,
    uint64_t                                    object,
    size_t                                      location,
    int32_t                                     messageCode,
    const char*                                 pLayerPrefix,
    const char*                                 pMessage,
    void*                                       pUserData)
{
	if (((flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) || (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT)) && !is_debug()) return VK_TRUE;
	fprintf(stderr, "report: %s\n", pMessage);
	return VK_TRUE;
}

void print_cmdbuf(vulkan_setup_t& vulkan, VkCommandBuffer cmdbuf)
{
	const unsigned index = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	const unsigned objs = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_MARKED_OBJECTS_TRACETOOLTEST);
	const unsigned ranges = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
	const unsigned bytes = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
	const unsigned updates = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_UPDATES_COUNT_TRACETOOLTEST);
	const unsigned written = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_UPDATES_BYTES_TRACETOOLTEST);
	VkCommandPool pool = (VkCommandPool)trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cmdbuf, VK_TRACING_OBJECT_PROPERTY_BACKING_STORE_TRACETOOLTEST);
	const unsigned poolindex = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)pool, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	printf("cmdbuf[%u] touched(objs=%u ranges=%u bytes=%u) objects(updates=%u bytes=%u) pool=%u\n", index, objs, ranges, bytes, updates, written, poolindex);
}

void print_memory(vulkan_setup_t& vulkan, VkDeviceMemory memory, const char* name)
{
	const unsigned index = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	const unsigned ranges = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST);
	const unsigned bytes = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST);
	const unsigned size = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_SIZE_TRACETOOLTEST);
	printf("%s : memory[%u] size=%u exposed(ranges=%u bytes=%u)\n", name, index, size, ranges, bytes);
}

void print_buffer(vulkan_setup_t& vulkan, VkBuffer buffer)
{
	const unsigned index = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	const unsigned count = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_UPDATES_COUNT_TRACETOOLTEST);
	const unsigned written = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_UPDATES_BYTES_TRACETOOLTEST);
	const unsigned size = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_SIZE_TRACETOOLTEST);
	VkDeviceMemory memory = (VkDeviceMemory)trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, VK_TRACING_OBJECT_PROPERTY_BACKING_STORE_TRACETOOLTEST);
	const unsigned memindex = trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(vulkan.device, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)memory, VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST);
	printf("buffer[%u] updates=(count=%u bytes=%u) size=%u backed by memory_index=%u\n", index, count, written, size, memindex);
}

void test_set_name(VkDevice device, VkObjectType type, uint64_t handle, const char* name)
{
	VkDebugUtilsObjectNameInfoEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr };
	info.objectType = type;
	info.objectHandle = handle;
	info.pObjectName = name;
	trace_vkSetDebugUtilsObjectNameEXT(device, &info);
}

void test_done(vulkan_setup_t vulkan)
{
	trace_vkDestroyDevice(vulkan.device, nullptr);
	trace_vkDestroyInstance(vulkan.instance, nullptr);
}

vulkan_setup_t test_init(const std::string& testname, vulkan_req_t& reqs, size_t chunk_size)
{
	const char* wsi = getenv("LAVATUBE_WINSYS");
	vulkan_setup_t vulkan;

	if (chunk_size != 0) // change default chunk size
	{
		lava_writer& instance = lava_writer::instance();
		lava_file_writer& writer = instance.file_writer();
		writer.change_default_chunk_size(chunk_size);
	}

	// Create instance
	VkInstanceCreateInfo pCreateInfo = {};
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = testname.c_str();
	app.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 );
	app.pEngineName = "testEngine";
	app.engineVersion = VK_MAKE_VERSION( 1, 0, 0 );
	app.apiVersion = reqs.apiVersion;
	pCreateInfo.pApplicationInfo = &app;
	pCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

	std::vector<const char*> enabledExtensions;
	uint32_t propertyCount = 0;
	VkResult result = trace_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = trace_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, supported_extensions.data());
	assert(result == VK_SUCCESS);
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) enabledExtensions.push_back(s.extensionName);
		else if (strcmp(s.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) enabledExtensions.push_back(s.extensionName);
	}
	if (wsi && strcmp(wsi, "headless") == 0)
	{
		enabledExtensions.push_back("VK_EXT_headless_surface");
	}
#ifdef VK_USE_PLATFORM_XCB_KHR
	else
	{
		enabledExtensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
	}
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	enabledExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
#ifdef VALIDATION
	const char *validationLayerNames[] = { "VK_LAYER_LUNARG_standard_validation" };
	pCreateInfo.enabledLayerCount = 1;
	pCreateInfo.ppEnabledLayerNames = validationLayerNames;
#endif
	enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	if (enabledExtensions.size() > 0)
	{
		pCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
	}
	pCreateInfo.enabledExtensionCount = enabledExtensions.size();

	VkDebugReportCallbackCreateInfoEXT debugcallbackext = { VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT, nullptr };
	debugcallbackext.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT
				| VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
	debugcallbackext.pfnCallback = report_callback;
	debugcallbackext.pUserData = nullptr;
	pCreateInfo.pNext = &debugcallbackext;
	result = trace_vkCreateInstance(&pCreateInfo, NULL, &vulkan.instance);
	check(result);

	// Select physical device
	uint32_t num_devices = 0;
	result = trace_vkEnumeratePhysicalDevices(vulkan.instance, &num_devices, nullptr);
	check(result);
	assert(result == VK_SUCCESS);
	assert(num_devices > 0);
	std::vector<VkPhysicalDevice> physical_devices(num_devices);
	result = trace_vkEnumeratePhysicalDevices(vulkan.instance, &num_devices, physical_devices.data());
	check(result);
	assert(num_devices == physical_devices.size());
	printf("Found %d physical devices!\n", (int)num_devices);
	vulkan.physical = physical_devices[0]; // just grab first one

	// Create logical device
	VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr };
	queueCreateInfo.queueFamilyIndex = 0;
	queueCreateInfo.queueCount = 1;
	float queuePriorities[] = { 1.0f };
	queueCreateInfo.pQueuePriorities = queuePriorities;
	VkPhysicalDeviceProperties2 vprops = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, nullptr };
	trace_vkGetPhysicalDeviceProperties2(vulkan.physical, &vprops);
	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr };
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueCreateInfo;
	deviceInfo.enabledLayerCount = 0;
	deviceInfo.ppEnabledLayerNames = nullptr;
	enabledExtensions.clear();
	supported_extensions.clear();
	propertyCount = 0;
	result = trace_vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	supported_extensions.resize(propertyCount);
	result = trace_vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &propertyCount, supported_extensions.data());
	assert(result == VK_SUCCESS);
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, VK_EXT_TOOLING_INFO_EXTENSION_NAME) == 0 && vprops.properties.vendorID != 0x8086) // Intel driver seems bugged here, reports it but does not support it?
		{
			enabledExtensions.push_back(s.extensionName);
		}
		if (strcmp(s.extensionName, VK_TRACETOOLTEST_TRACE_HELPERS_EXTENSION_NAME) == 0) enabledExtensions.push_back(s.extensionName);
		if (strcmp(s.extensionName, VK_TRACETOOLTEST_OBJECT_PROPERTY_EXTENSION_NAME) == 0) enabledExtensions.push_back(s.extensionName);
	}
	if (enabledExtensions.size() > 0)
	{
		deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();
	}
	deviceInfo.enabledExtensionCount = enabledExtensions.size();
	if (VK_VERSION_MAJOR(reqs.apiVersion) >= 1 && VK_VERSION_MINOR(reqs.apiVersion) >= 2)
	{
		deviceInfo.pNext = &reqs.reqfeat2;
	}
	else // Vulkan 1.1 or below
	{
		deviceInfo.pEnabledFeatures = &reqs.reqfeat2.features;
	}
	result = trace_vkCreateDevice(vulkan.physical, &deviceInfo, NULL, &vulkan.device);
	check(result);
	test_set_name(vulkan.device, VK_OBJECT_TYPE_DEVICE, (uint64_t)vulkan.device, "Our device");

	trace_vkGetPhysicalDeviceMemoryProperties(vulkan.physical, &memory_properties);

	return vulkan;
}

uint32_t get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
	{
		if (type_filter & (1 << i) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	assert(false);
	return 0xffff; // satisfy compiler
}
