#include "tests/common.h"
#include "packfile.h"
#include "generated/read_auto.h"
#include "external/tracetooltests/include/vulkan_utility.h"

#include <atomic>

#define TEST_NAME "tracing_unused"

static constexpr const char* UNUSED_INSTANCE_EXTENSION = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
static constexpr const char* UNUSED_DEVICE_EXTENSION = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;

struct replay_observation
{
	bool saw_instance_create = false;
	bool saw_device_create = false;
	bool has_instance_extension = false;
	bool has_device_extension = false;
	bool has_sync2_feature_struct = false;
	bool sampler_anisotropy_enabled = false;
};

static replay_observation observation;

static bool getnext(lava_file_reader& t)
{
	bool done = false;
	const uint8_t instrtype = t.step();
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
	return !done;
}

static bool extension_list_contains(const char* const* names, uint32_t count, const char* name)
{
	for (uint32_t i = 0; i < count; i++)
	{
		if (strcmp(names[i], name) == 0) return true;
	}
	return false;
}

#ifdef DEBUG
static bool json_array_contains(const Json::Value& array, const char* name)
{
	if (!array.isArray()) return false;
	for (const Json::Value& item : array)
	{
		if (item.asString() == name) return true;
	}
	return false;
}
#endif

static void record_vkCreateInstance(callback_context& cb, const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
	(void)pAllocator;
	(void)pInstance;
	assert(cb.result.vkresult == VK_SUCCESS);
	assert(!observation.saw_instance_create);
	observation.saw_instance_create = true;
	observation.has_instance_extension = extension_list_contains(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount, UNUSED_INSTANCE_EXTENSION);
}

static void record_vkCreateDevice(callback_context& cb, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	(void)physicalDevice;
	(void)pAllocator;
	(void)pDevice;
	assert(cb.result.vkresult == VK_SUCCESS);
	assert(!observation.saw_device_create);
	observation.saw_device_create = true;
	observation.has_device_extension = extension_list_contains(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount, UNUSED_DEVICE_EXTENSION);

	const VkPhysicalDeviceFeatures2* feat2 =
		(const VkPhysicalDeviceFeatures2*)find_extension(pCreateInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
	assert(feat2 != nullptr);
	observation.sampler_anisotropy_enabled = feat2->features.samplerAnisotropy == VK_TRUE;

	const VkPhysicalDeviceSynchronization2Features* sync2 =
		(const VkPhysicalDeviceSynchronization2Features*)find_extension(pCreateInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
	observation.has_sync2_feature_struct = sync2 != nullptr;
}

static void trace()
{
	vulkan_req_t reqs;
	reqs.apiVersion = VK_API_VERSION_1_2;
	reqs.instance_extensions.push_back(UNUSED_INSTANCE_EXTENSION);
	reqs.device_extensions.push_back(UNUSED_DEVICE_EXTENSION);
	reqs.reqfeat2.features.samplerAnisotropy = VK_TRUE;

	VkPhysicalDeviceSynchronization2Features sync2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, nullptr };
	sync2.synchronization2 = VK_FALSE;

	// Keep the requested feature chain minimal and end it with an extension-specific feature struct.
	reqs.reqfeat12.pNext = &sync2;
	reqs.reqfeat13.pNext = nullptr;

	vulkan_setup_t vulkan = test_init(TEST_NAME, reqs);
	test_done(vulkan);
}

static void verify_metadata()
{
	Json::Value meta = packed_json("metadata.json", TEST_NAME ".api");

	assert(json_array_contains(meta["instanceRequested"]["removedExtensions"], UNUSED_INSTANCE_EXTENSION));
	assert(!json_array_contains(meta["instanceRequested"]["enabledExtensions"], UNUSED_INSTANCE_EXTENSION));

	assert(json_array_contains(meta["deviceRequested"]["removedExtensions"], UNUSED_DEVICE_EXTENSION));
	assert(!json_array_contains(meta["deviceRequested"]["enabledExtensions"], UNUSED_DEVICE_EXTENSION));

	assert(json_array_contains(meta["deviceRequested"]["removedFeatures"]["VkPhysicalDeviceFeatures"], "samplerAnisotropy"));
	assert(meta["deviceRequested"]["VkPhysicalDeviceFeatures"]["samplerAnisotropy"].asBool() == false);
}

static void replay_and_verify(bool skip_remove_unused)
{
	const uint_fast8_t old_skip = p__skip_remove_unused;
	p__skip_remove_unused = skip_remove_unused ? 1 : 0;

	observation = {};
	clear_callbacks();
	reset_for_tools();
	test_register_replay_callbacks();
	vkCreateInstance_callbacks.push_back(record_vkCreateInstance);
	vkCreateDevice_callbacks.push_back(record_vkCreateDevice);

	{
		lava_reader r(TEST_NAME ".api");
		lava_file_reader& t = r.file_reader(0);
		while (getnext(t)) {}
	}

	assert(observation.saw_instance_create);
	assert(observation.saw_device_create);
	assert(observation.has_instance_extension == skip_remove_unused);
	assert(observation.has_device_extension == skip_remove_unused);
	assert(observation.has_sync2_feature_struct == skip_remove_unused);
	assert(observation.sampler_anisotropy_enabled == skip_remove_unused);

	clear_callbacks();
	reset_for_tools();
	p__skip_remove_unused = old_skip;
}

int main()
{
	trace();
	verify_metadata();
	replay_and_verify(false);
	replay_and_verify(true);
	return 0;
}
