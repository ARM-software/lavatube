// Do not add stuff to this test. And if you do, remember to manually add corresponding getnext() calls below.

#include <vector>
#include <string>

#include "vulkan/vulkan.h"
#include "util.h"
#include "util_auto.h"
#include "read_auto.h"
#include "write_auto.h"

#include "tests/tests.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

#define TEST_NAME_1 "tracing_1_1"
#define TEST_NAME_2 "tracing_1_2"
#define NUM_BUFFERS 48
#define NUM_CMDBUFFERS 10
#define BUFFER_SIZE (1024 * 1024)

#define check(result) \
	if (result != VK_SUCCESS) \
	{ \
		fprintf(stderr, "Error: 0x%04x\n", result); \
	} \
	assert(result == VK_SUCCESS);

static VkPhysicalDeviceMemoryProperties memory_properties = {};

static VkBool32 messenger_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT           messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT                  messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT*      pCallbackData,
    void*                                            pUserData)
{
	fprintf(stderr, "messenger: %s\n", pCallbackData->pMessage);
	return VK_TRUE;
}

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
	fprintf(stderr, "report: %s\n", pMessage);
	return VK_TRUE;
}

static uint32_t get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties)
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

static void trace_1()
{
	// Create instance
	PFN_vkVoidFunction nothing = trace_vkGetInstanceProcAddr(nullptr, "nothing");
	assert(nothing == nullptr);
	VkInstanceCreateInfo pCreateInfo = {};
	VkInstance instance;
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = TEST_NAME_1;
	app.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 );
	app.pEngineName = "testEngine";
	app.engineVersion = VK_MAKE_VERSION( 1, 0, 0 );
	app.apiVersion = VK_API_VERSION_1_1;
	pCreateInfo.pApplicationInfo = &app;
	pCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	pCreateInfo.enabledExtensionCount = 0;
	VkResult result = trace_vkCreateInstance(&pCreateInfo, NULL, &instance);
	check(result);
	nothing = trace_vkGetInstanceProcAddr(nullptr, "nothing");
	assert(nothing == nullptr);
	nothing = trace_vkGetInstanceProcAddr(instance, "nothing");
	assert(nothing == nullptr);
	trace_vkDestroyInstance(instance, nullptr);
}

static void trace_2(int variant)
{
	const char* wsi = getenv("LAVATUBE_WINSYS");

	// Create instance
	VkInstanceCreateInfo pCreateInfo = {};
	VkInstance instance;
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	std::string appname = std::string(TEST_NAME_2) + "_" + _to_string(variant);
	app.pApplicationName = appname.c_str();
	app.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 );
	if (variant == 0) app.pEngineName = "testEngine";
	app.engineVersion = VK_MAKE_VERSION( 1, 0, 0 );
	app.apiVersion = VK_API_VERSION_1_1;
	pCreateInfo.pApplicationInfo = &app;
	pCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

	std::vector<const char*> enabledExtensions;
	uint32_t propertyCount = 0;
	VkResult result = trace_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr);
	assert(result == VK_SUCCESS);
	std::vector<VkExtensionProperties> supported_extensions(propertyCount);
	result = wrap_vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, supported_extensions.data());
	assert(result == VK_SUCCESS);
	for (const VkExtensionProperties& s : supported_extensions)
	{
		if (strcmp(s.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) enabledExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
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

	VkDebugReportCallbackCreateInfoEXT debugcallbackext = {};
	debugcallbackext.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
	debugcallbackext.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT
				| VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
	debugcallbackext.pfnCallback = report_callback;
	debugcallbackext.pUserData = nullptr;

	VkDebugUtilsMessengerCreateInfoEXT messext = {};
	messext.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	messext.pNext = &debugcallbackext;
	messext.flags = 0;
	messext.pfnUserCallback = messenger_callback;
	messext.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messext.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	pCreateInfo.pNext = &messext;

	result = trace_vkCreateInstance(&pCreateInfo, NULL, &instance);
	check(result);

	// Create logical device
	uint32_t num_devices = 0;
	result = trace_vkEnumeratePhysicalDevices(instance, &num_devices, nullptr);
	check(result);
	assert(result == VK_SUCCESS);
	assert(num_devices > 0);
	std::vector<VkPhysicalDevice> physical_devices(num_devices);
	result = trace_vkEnumeratePhysicalDevices(instance, &num_devices, physical_devices.data());
	check(result);
	assert(result == VK_SUCCESS);
	assert(num_devices == physical_devices.size());
	printf("Found %d physical devices!\n", (int)num_devices);
	VkPhysicalDevice &dev = physical_devices[0]; // just grab first one
	VkDeviceQueueCreateInfo queueCreateInfo = {};
	queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo.queueFamilyIndex = 0;
	queueCreateInfo.queueCount = 1;
	float queuePriorities[] = { 1.0f };
	queueCreateInfo.pQueuePriorities = queuePriorities;
	VkPhysicalDevice16BitStorageFeatures pd16bit = {};
	pd16bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
	VkDeviceCreateInfo deviceInfo = {};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.pNext = &pd16bit;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueCreateInfo;
	deviceInfo.enabledLayerCount = 0;
	deviceInfo.ppEnabledLayerNames = nullptr;
	VkPhysicalDeviceFeatures features = {};
	deviceInfo.pEnabledFeatures = &features;
	VkDevice device;
	result = trace_vkCreateDevice(dev, &deviceInfo, NULL, &device);
	check(result);

	VkCommandPool cmdpool;
	VkCommandPoolCreateInfo cmdcreateinfo = {};
	cmdcreateinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	if (variant == 0) cmdcreateinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (variant == 1) cmdcreateinfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	cmdcreateinfo.queueFamilyIndex = 0;
	result = trace_vkCreateCommandPool(device, &cmdcreateinfo, nullptr, &cmdpool);
	check(result);

	std::vector<VkCommandBuffer> cmdbuffers(NUM_CMDBUFFERS);
	VkCommandBufferAllocateInfo pAllocateInfo = {};
	pAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	pAllocateInfo.commandBufferCount = NUM_CMDBUFFERS;
	pAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	pAllocateInfo.commandPool = cmdpool;
	result = trace_vkAllocateCommandBuffers(device, &pAllocateInfo, cmdbuffers.data());
	check(result);

	result = trace_vkResetCommandPool(device, cmdpool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
	assert(result == VK_SUCCESS);

	VkSemaphoreCreateInfo semaphoreCreateInfo = {};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (variant == 1) semaphoreCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VkSemaphore pSemaphore = VK_NULL_HANDLE;
	result = trace_vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &pSemaphore);
	assert(pSemaphore != 0);
	check(result);

	VkSamplerCreateInfo samplerCreateInfo = {};
	samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCreateInfo.flags = 0;
	samplerCreateInfo.magFilter = VK_FILTER_NEAREST;
	samplerCreateInfo.minFilter = VK_FILTER_NEAREST;
	samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.mipLodBias = 0.25;
	samplerCreateInfo.anisotropyEnable = VK_FALSE;
	samplerCreateInfo.maxAnisotropy = 0.0f;
	samplerCreateInfo.compareEnable = VK_FALSE;
	samplerCreateInfo.compareOp = VK_COMPARE_OP_NEVER;
	samplerCreateInfo.minLod = 0.1f;
	samplerCreateInfo.maxLod = 0.9f;
	samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;
	VkSampler sampler;
	trace_vkCreateSampler(device, &samplerCreateInfo, nullptr, &sampler);

	trace_vkDestroySampler(device, sampler, nullptr);

	VkBuffer buffer[NUM_BUFFERS];
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = BUFFER_SIZE;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		result = trace_vkCreateBuffer(device, &bufferCreateInfo, nullptr, &buffer[i]);
	}

	trace_vkGetPhysicalDeviceMemoryProperties(dev, &memory_properties);

	VkMemoryRequirements req;
	trace_vkGetBufferMemoryRequirements(device, buffer[0], &req);
	uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VkMemoryAllocateInfo pAllocateMemInfo = {};
	pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
	pAllocateMemInfo.allocationSize = req.size * NUM_BUFFERS;
	VkDeviceMemory pMemory = 0;
	result = trace_vkAllocateMemory(device, &pAllocateMemInfo, nullptr, &pMemory);
	assert(result == VK_SUCCESS);
	assert(pMemory != 0);

	VkDeviceSize offset = 0;
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkBindBufferMemory(device, buffer[i], pMemory, offset);
		offset += req.size;
	}

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent.width = 255;
	imageInfo.extent.height = 255;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image;
	result = trace_vkCreateImage(device, &imageInfo, nullptr, &image);
	assert(result == VK_SUCCESS);
	trace_vkDestroyImage(device, image, nullptr); // destroy image before binding it
	result = trace_vkCreateImage(device, &imageInfo, nullptr, &image); // create again
	assert(result == VK_SUCCESS);
	trace_vkGetImageMemoryRequirements(device, image, &req);
	VkDeviceMemory imagememory = 0;
	pAllocateMemInfo.memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, 0);
	pAllocateMemInfo.allocationSize = req.size;
	result = trace_vkAllocateMemory(device, &pAllocateMemInfo, nullptr, &imagememory);
	assert(result == VK_SUCCESS);
	assert(pMemory != 0);
	result = trace_vkBindImageMemory(device, image, imagememory, 0);

	result = trace_vkDeviceWaitIdle(device);
	assert(result == VK_SUCCESS);

	trace_vkDestroyImage(device, image, nullptr);
	trace_vkDestroyImage(device, VK_NULL_HANDLE, nullptr);

	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		trace_vkDestroyBuffer(device, buffer[i], nullptr);
	}
	trace_vkDestroyBuffer(device, VK_NULL_HANDLE, nullptr);

	trace_vkFreeMemory(device, imagememory, nullptr);
	trace_vkFreeMemory(device, pMemory, nullptr);
	trace_vkDestroySemaphore(device, pSemaphore, nullptr);
	trace_vkDestroySemaphore(device, VK_NULL_HANDLE, nullptr);
	trace_vkFreeCommandBuffers(device, cmdpool, cmdbuffers.size(), cmdbuffers.data());
	trace_vkDestroyCommandPool(device, cmdpool, nullptr);
	trace_vkDestroyDevice(device, nullptr);
	trace_vkDestroyInstance(instance, nullptr);
}

static void getnext(lava_file_reader& t, const char* expected_s)
{
	const uint8_t instrtype = t.read_uint8_t();
	if (instrtype == PACKET_API_CALL)
	{
		const uint16_t expected = retrace_getid(expected_s);
		const uint16_t apicall = t.read_apicall();
		assert(instrtype == 2);
		assert(apicall == expected);
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		assert(expected_s == nullptr);
		const uint8_t size = t.read_uint8_t();
		DLOG("PACKET_THREAD_BARRIER waiting for %d threads", size);
		for (int i = 0; i < size; i++) (void)t.read_uint32_t();
	}
	else assert(false);
}

static void retrace_1()
{
	lava_reader r(TEST_NAME_1 ".vk");
	lava_file_reader& t = r.file_reader(0);

	getnext(t, nullptr); // initial thread barrier for thread start
	getnext(t, "vkCreateInstance");
	getnext(t, nullptr); // thread barrier before destroy
	getnext(t, "vkDestroyInstance");
}

static void retrace_2(int variant)
{
	std::string testname = std::string(TEST_NAME_2) + "_" +  _to_string(variant) + ".vk";
	lava_reader r(testname);
	lava_file_reader& t = r.file_reader(0);

	getnext(t, nullptr); // initial thread barrier for thread start
	getnext(t, "vkEnumerateInstanceExtensionProperties");
	getnext(t, "vkCreateInstance");
	getnext(t, "vkEnumeratePhysicalDevices");
	getnext(t, "vkEnumeratePhysicalDevices");
	getnext(t, "vkCreateDevice");
	getnext(t, "vkCreateCommandPool");
	assert(index_to_VkCommandPool.size() == 1);
	getnext(t, "vkAllocateCommandBuffers");
	assert(index_to_VkCommandBuffer.size() == NUM_CMDBUFFERS);
	trackedcmdbuffer_replay& cmdb = VkCommandBuffer_index.at(0);
	assert(cmdb.pool == 0);
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkResetCommandPool");
	getnext(t, "vkCreateSemaphore");
	getnext(t, "vkCreateSampler");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroySampler");
	for (unsigned i = 0; i < NUM_BUFFERS; i++) getnext(t, "vkCreateBuffer");
	assert(index_to_VkBuffer.size() == NUM_BUFFERS);
	getnext(t, "vkGetPhysicalDeviceMemoryProperties");
	getnext(t, "vkGetBufferMemoryRequirements");
	getnext(t, "vkAllocateMemory");
	for (unsigned i = 0; i < NUM_BUFFERS; i++) getnext(t, "vkBindBufferMemory");
	getnext(t, "vkCreateImage");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyImage");
	getnext(t, "vkCreateImage");
	getnext(t, "vkGetImageMemoryRequirements");
	getnext(t, "vkAllocateMemory");
	getnext(t, "vkBindImageMemory");
	getnext(t, "vkDeviceWaitIdle");
	getnext(t, nullptr); // thread barrier before destroy command
	getnext(t, "vkDestroyImage");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyImage");
	trackedbuffer& b = VkBuffer_index.at(0);
	assert(b.size == BUFFER_SIZE);
	assert(b.destroyed.frame != UINT32_MAX);
	for (unsigned i = 0; i < NUM_BUFFERS; i++)
	{
		getnext(t, nullptr); // thread barrier
		getnext(t, "vkDestroyBuffer");
	}
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyBuffer");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkFreeMemory");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkFreeMemory");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroySemaphore");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroySemaphore");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkFreeCommandBuffers");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyCommandPool");
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyDevice");
	assert(index_to_VkBuffer.size() == NUM_BUFFERS); // deleted but still retained
	getnext(t, nullptr); // thread barrier
	getnext(t, "vkDestroyInstance");
}

int main(int argc, char** argv)
{
	int variant = 0;

	if (argc > 1 && argv[1][0] == '1')
	{
		printf("Runing variant 1\n");
		variant = 1;
	}

	// test initialization without any extensions
	trace_1();
	retrace_1();

	// test bigger init & destroy sequence; also check that re-initialization works
	trace_2(variant);
	retrace_2(variant);

	return 0;
}
