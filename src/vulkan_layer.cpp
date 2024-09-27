#include "vulkan/vulkan.h"
#include <jsoncpp/json/value.h>
#include "util.h"
#include "vk_wrapper_auto.h"
#include "write_auto.h"

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
	return trace_vkGetDeviceProcAddr(device, pName);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
	return trace_vkGetInstanceProcAddr(instance, pName);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
	return trace_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
	return trace_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
	return trace_vkEnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
	return trace_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}
