#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "decode/vulkan_api_call_reader.h"
#include "util.h"
#include "write.h"
#include "write_auto.h"

using gfxrecon::decode::VulkanApiCallReader;
using gfxrecon::decode::VulkanNativeCallContext;
using gfxrecon::decode::VulkanNativeCallbackEntry;
using gfxrecon::decode::VulkanNativeCallbackRegistrationError;
using gfxrecon::decode::VulkanNativeCallError;
using gfxrecon::decode::VulkanNativeMissingCallbackPolicy;
using gfxrecon::decode::VulkanMemoryUpdate;
using gfxrecon::decode::VulkanDeviceMemoryProperties;

static void set_captured_return_value(void*, const VulkanNativeCallContext& call)
{
	lava_file_writer& file = lava_writer::instance().file_writer();
	memset(&file.use_result, 0, sizeof(file.use_result));
	if (call.return_value_size == 0) return;
	if (!call.return_value || call.return_value_size > sizeof(file.use_result))
	{
		DIE("Invalid captured return value for %s: %zu bytes", call.name ? call.name : "unknown", call.return_value_size);
	}
	memcpy(&file.use_result, call.return_value, call.return_value_size);
}

static VkResult VKAPI_CALL import_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
	VkPhysicalDevice* pPhysicalDevices)
{
	const VkResult result = trace_vkEnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
	if (result != VK_SUCCESS || !pPhysicalDeviceCount || !pPhysicalDevices) return result;

	lava_file_writer& file = lava_writer::instance().file_writer();
	assert(file.current.packet > 0);
	change_source creation = file.current;
	creation.packet--;
	for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++)
	{
		const VkPhysicalDevice physical_device = pPhysicalDevices[i];
		if (physical_device == VK_NULL_HANDLE || file.parent->records.VkPhysicalDevice_index.contains(physical_device)) continue;
		auto* data = file.parent->records.VkPhysicalDevice_index.add(physical_device, creation,
			file.parent->desired_output_handle_index(physical_device));
		data->enter_initialized();
	}
	return result;
}

static void VKAPI_CALL import_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties)
{
	trace_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
	if (!pProperties) return;
	auto* data = lava_writer::instance().records.VkPhysicalDevice_index.at(physicalDevice);
	if (data) data->deviceType = pProperties->deviceType;
}

static VkResult import_vkCreateSurfaceKHR(VkInstance instance, VkStructureType sType, const void* pNext,
	VkFlags flags, VkSurfaceKHR* pSurface, const char* name, lava_function_id id)
{
	assert(pSurface);
	lava_file_writer& file = lava_writer::instance().file_writer();
	surface_create_packet packet;
	packet.instance = instance;
	packet.stored_sType = sType;
	packet.pNext = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(pNext));
	packet.flags = flags;
	packet.retval = file.use_result.result;
	if (packet.retval == VK_SUCCESS && *pSurface != VK_NULL_HANDLE) packet.surface_index = fake_index(*pSurface);
	tool_write_vkCreateSurfaceKHR_packet(packet, name, id);
	return packet.retval;
}

static VkResult VKAPI_CALL import_vkCreateHeadlessSurfaceEXT(VkInstance instance,
	const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
	(void)pAllocator;
	assert(pCreateInfo);
	return import_vkCreateSurfaceKHR(instance, pCreateInfo->sType, pCreateInfo->pNext, pCreateInfo->flags, pSurface,
		"vkCreateHeadlessSurfaceEXT", VKCREATEHEADLESSSURFACEEXT);
}

#ifdef VK_USE_PLATFORM_XLIB_KHR
static VkResult VKAPI_CALL import_vkCreateXlibSurfaceKHR(VkInstance instance,
	const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
	(void)pAllocator;
	assert(pCreateInfo);
	return import_vkCreateSurfaceKHR(instance, pCreateInfo->sType, pCreateInfo->pNext, pCreateInfo->flags, pSurface,
		"vkCreateXlibSurfaceKHR", VKCREATEXLIBSURFACEKHR);
}
#endif

#ifdef VK_USE_PLATFORM_XCB_KHR
static VkResult VKAPI_CALL import_vkCreateXcbSurfaceKHR(VkInstance instance,
	const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
	(void)pAllocator;
	assert(pCreateInfo);
	return import_vkCreateSurfaceKHR(instance, pCreateInfo->sType, pCreateInfo->pNext, pCreateInfo->flags, pSurface,
		"vkCreateXcbSurfaceKHR", VKCREATEXCBSURFACEKHR);
}
#endif

static VkResult VKAPI_CALL import_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
	assert(pCreateInfo);
	assert(pSwapchain);
	const VkResult result = trace_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
	if (result == VK_SUCCESS)
	{
		auto* surface_data = lava_writer::instance().records.VkSurfaceKHR_index.at(pCreateInfo->surface);
		if (surface_data->width == 0)
		{
			surface_data->width = pCreateInfo->imageExtent.width;
			surface_data->height = pCreateInfo->imageExtent.height;
		}
	}
	return result;
}

static void prepare_import_binding(trackedobject* object_data, trackedmemory* memory_data, VkDeviceSize memory_offset)
{
	assert(object_data);
	assert(memory_data);
	if (object_data->backing != VK_NULL_HANDLE)
	{
		DIE("Cannot rebind imported %s[%u]", pretty_print_VkObjectType(object_data->object_type), object_data->index);
	}
	if (object_data->object_type != VK_OBJECT_TYPE_BUFFER)
	{
		if (object_data->req.size == 0)
		{
			DIE("Missing memory requirements for imported %s[%u]", pretty_print_VkObjectType(object_data->object_type), object_data->index);
		}
		object_data->size = object_data->req.size;
	}
	if (object_data->size == 0 || memory_offset > memory_data->allocationSize ||
		object_data->size > memory_data->allocationSize - memory_offset)
	{
		DIE("Invalid imported binding for %s[%u]: allocation size %lu, offset %lu, object size %lu",
			pretty_print_VkObjectType(object_data->object_type), object_data->index,
			(unsigned long)memory_data->allocationSize, (unsigned long)memory_offset, (unsigned long)object_data->size);
	}
	object_data->memory_flags = memory_data->propertyFlags;
}

static void finish_import_binding(trackedobject* object_data, trackedmemory* memory_data, VkDeviceMemory memory,
	VkDeviceSize memory_offset)
{
	lava_writer& writer = lava_writer::instance();
	lava::lock_guard guard(writer.memory_mutex);
	object_data->backing = memory;
	object_data->offset = memory_offset;
	object_data->accessible = (memory_data->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
	if (object_data->object_type == VK_OBJECT_TYPE_IMAGE)
	{
		const trackedimage* image_data = static_cast<const trackedimage*>(object_data);
		object_data->accessible = object_data->accessible && image_data->tiling != TILING_OPTIMAL;
	}
	memory_data->bind(object_data);
	object_data->enter_bound();
}

static VkResult VKAPI_CALL import_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
	VkDeviceSize memoryOffset)
{
	lava_writer& writer = lava_writer::instance();
	trackedbuffer* object_data = writer.records.VkBuffer_index.at(buffer);
	trackedmemory* memory_data = writer.records.VkDeviceMemory_index.at(memory);
	if (writer.file_writer().use_result.result == VK_SUCCESS) prepare_import_binding(object_data, memory_data, memoryOffset);
	const VkResult result = trace_vkBindBufferMemory(device, buffer, memory, memoryOffset);
	if (result == VK_SUCCESS) finish_import_binding(object_data, memory_data, memory, memoryOffset);
	return result;
}

static VkResult VKAPI_CALL import_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
	VkDeviceSize memoryOffset)
{
	lava_writer& writer = lava_writer::instance();
	trackedimage* object_data = writer.records.VkImage_index.at(image);
	trackedmemory* memory_data = writer.records.VkDeviceMemory_index.at(memory);
	if (writer.file_writer().use_result.result == VK_SUCCESS) prepare_import_binding(object_data, memory_data, memoryOffset);
	const VkResult result = trace_vkBindImageMemory(device, image, memory, memoryOffset);
	if (result == VK_SUCCESS) finish_import_binding(object_data, memory_data, memory, memoryOffset);
	return result;
}

static VkResult import_vkBindBufferMemory2_common(VkDevice device, uint32_t bindInfoCount,
	const VkBindBufferMemoryInfo* pBindInfos, bool khr)
{
	lava_writer& writer = lava_writer::instance();
	if (writer.file_writer().use_result.result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			prepare_import_binding(writer.records.VkBuffer_index.at(pBindInfos[i].buffer),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memoryOffset);
		}
	}
	const VkResult result = khr ? trace_vkBindBufferMemory2KHR(device, bindInfoCount, pBindInfos)
		: trace_vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
	if (result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			finish_import_binding(writer.records.VkBuffer_index.at(pBindInfos[i].buffer),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memory, pBindInfos[i].memoryOffset);
		}
	}
	return result;
}

static VkResult VKAPI_CALL import_vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
	const VkBindBufferMemoryInfo* pBindInfos)
{
	return import_vkBindBufferMemory2_common(device, bindInfoCount, pBindInfos, false);
}

static VkResult VKAPI_CALL import_vkBindBufferMemory2KHR(VkDevice device, uint32_t bindInfoCount,
	const VkBindBufferMemoryInfo* pBindInfos)
{
	return import_vkBindBufferMemory2_common(device, bindInfoCount, pBindInfos, true);
}

static VkResult import_vkBindImageMemory2_common(VkDevice device, uint32_t bindInfoCount,
	const VkBindImageMemoryInfo* pBindInfos, bool khr)
{
	lava_writer& writer = lava_writer::instance();
	if (writer.file_writer().use_result.result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			prepare_import_binding(writer.records.VkImage_index.at(pBindInfos[i].image),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memoryOffset);
		}
	}
	const VkResult result = khr ? trace_vkBindImageMemory2KHR(device, bindInfoCount, pBindInfos)
		: trace_vkBindImageMemory2(device, bindInfoCount, pBindInfos);
	if (result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			finish_import_binding(writer.records.VkImage_index.at(pBindInfos[i].image),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memory, pBindInfos[i].memoryOffset);
		}
	}
	return result;
}

static VkResult VKAPI_CALL import_vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount,
	const VkBindImageMemoryInfo* pBindInfos)
{
	return import_vkBindImageMemory2_common(device, bindInfoCount, pBindInfos, false);
}

static VkResult VKAPI_CALL import_vkBindImageMemory2KHR(VkDevice device, uint32_t bindInfoCount,
	const VkBindImageMemoryInfo* pBindInfos)
{
	return import_vkBindImageMemory2_common(device, bindInfoCount, pBindInfos, true);
}

static void VKAPI_CALL import_vkGetTensorMemoryRequirementsARM(VkDevice device,
	const VkTensorMemoryRequirementsInfoARM* pInfo, VkMemoryRequirements2* pMemoryRequirements)
{
	trace_vkGetTensorMemoryRequirementsARM(device, pInfo, pMemoryRequirements);
	if (!pInfo || !pMemoryRequirements) return;
	trackedtensor* object_data = lava_writer::instance().records.VkTensorARM_index.at(pInfo->tensor);
	object_data->req = pMemoryRequirements->memoryRequirements;
	object_data->size = object_data->req.size;
}

static VkResult VKAPI_CALL import_vkBindTensorMemoryARM(VkDevice device, uint32_t bindInfoCount,
	const VkBindTensorMemoryInfoARM* pBindInfos)
{
	lava_writer& writer = lava_writer::instance();
	if (writer.file_writer().use_result.result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			prepare_import_binding(writer.records.VkTensorARM_index.at(pBindInfos[i].tensor),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memoryOffset);
		}
	}
	const VkResult result = trace_vkBindTensorMemoryARM(device, bindInfoCount, pBindInfos);
	if (result == VK_SUCCESS)
	{
		for (uint32_t i = 0; i < bindInfoCount; i++)
		{
			finish_import_binding(writer.records.VkTensorARM_index.at(pBindInfos[i].tensor),
				writer.records.VkDeviceMemory_index.at(pBindInfos[i].memory), pBindInfos[i].memory, pBindInfos[i].memoryOffset);
		}
	}
	return result;
}

static void import_memory_update(void*, const VulkanMemoryUpdate& update)
{
	if (update.memory == VK_NULL_HANDLE || !update.data || update.data_size == 0)
	{
		DIE("Invalid gfxreconstruct memory update at block %lu", (unsigned long)update.block_index);
	}
	lava_writer& writer = lava_writer::instance();
	trackedmemory* memory_data = writer.records.VkDeviceMemory_index.at(update.memory);
	if (update.memory_offset > memory_data->allocationSize ||
		update.data_size > memory_data->allocationSize - update.memory_offset)
	{
		DIE("Out-of-range gfxreconstruct memory update at block %lu: allocation size %lu, offset %lu, update size %lu",
			(unsigned long)update.block_index, (unsigned long)memory_data->allocationSize,
			(unsigned long)update.memory_offset, (unsigned long)update.data_size);
	}

	lava::lock_guard guard(writer.memory_mutex);
	const uint64_t update_start = update.memory_offset;
	const uint64_t update_end = update_start + update.data_size;
	uint32_t emitted = 0;
	for (const auto& pair : memory_data->usage)
	{
		trackedobject* object_data = pair.second;
		const uint64_t object_start = object_data->offset;
		const uint64_t object_end = object_start + object_data->size;
		const uint64_t overlap_start = std::max(update_start, object_start);
		const uint64_t overlap_end = std::min(update_end, object_end);
		if (overlap_start >= overlap_end) continue;
		const auto& devices = writer.records.VkDevice_index.iterate();
		if (object_data->parent_device_index >= devices.size())
		{
			DIE("Missing device for imported %s[%u]", pretty_print_VkObjectType(object_data->object_type), object_data->index);
		}
		const char* data = reinterpret_cast<const char*>(update.data) + overlap_start - update_start;
		write_object_update_packet(writer.file_writer(), devices.at(object_data->parent_device_index), object_data,
			overlap_start - object_start, data, overlap_end - overlap_start);
		emitted++;
	}
	if (emitted == 0)
	{
		WLOG("gfxreconstruct memory update at block %lu did not overlap a bound buffer, image, or tensor",
			(unsigned long)update.block_index);
	}
}

static void import_device_memory_properties(void*, const VulkanDeviceMemoryProperties& properties)
{
	VkPhysicalDeviceMemoryProperties memory_properties = properties.memory_properties;
	trace_vkGetPhysicalDeviceMemoryProperties(properties.physical_device, &memory_properties);
}

static PFN_vkVoidFunction import_callback(const std::string& name, void* callback)
{
	if (name == "vkEnumeratePhysicalDevices")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkEnumeratePhysicalDevices);
	if (name == "vkGetPhysicalDeviceProperties")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkGetPhysicalDeviceProperties);
	if (name == "vkCreateHeadlessSurfaceEXT")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateHeadlessSurfaceEXT);
#ifdef VK_USE_PLATFORM_XLIB_KHR
	if (name == "vkCreateXlibSurfaceKHR")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateXlibSurfaceKHR);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
	if (name == "vkCreateXcbSurfaceKHR")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateXcbSurfaceKHR);
#endif
	if (name == "vkCreateSwapchainKHR")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateSwapchainKHR);
	if (name == "vkBindBufferMemory")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindBufferMemory);
	if (name == "vkBindBufferMemory2")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindBufferMemory2);
	if (name == "vkBindBufferMemory2KHR")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindBufferMemory2KHR);
	if (name == "vkBindImageMemory")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindImageMemory);
	if (name == "vkBindImageMemory2")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindImageMemory2);
	if (name == "vkBindImageMemory2KHR")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindImageMemory2KHR);
	if (name == "vkGetTensorMemoryRequirementsARM")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkGetTensorMemoryRequirementsARM);
	if (name == "vkBindTensorMemoryARM")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkBindTensorMemoryARM);
	return reinterpret_cast<PFN_vkVoidFunction>(callback);
}

static bool register_callbacks(VulkanApiCallReader& reader)
{
	std::vector<VulkanNativeCallbackEntry> callbacks;
	callbacks.reserve(write_callback_table.size());
	for (const auto& item : write_callback_table)
	{
		callbacks.push_back({ item.first.c_str(), import_callback(item.first, item.second) });
	}

	std::vector<VulkanNativeCallbackRegistrationError> rejected;
	const size_t accepted = reader.RegisterCallbacks(callbacks.data(), callbacks.size(), &rejected);
	if (accepted == 0)
	{
		ELOG("gfxreconstruct did not accept any lavatube callbacks");
		return false;
	}
	if (!rejected.empty())
	{
		WLOG("gfxreconstruct rejected %zu callbacks that are not available in its Vulkan headers", rejected.size());
	}
	return true;
}

static void print_native_error(const VulkanNativeCallError& error)
{
	ELOG("Conversion stopped at gfxreconstruct call %lu (thread %lu): %s (call id %u)",
		(unsigned long)error.call_info.index, (unsigned long)error.call_info.thread_id,
		error.name.c_str(), error.call_id);
}

static void print_usage(const char* program)
{
	fprintf(stderr, "Usage: %s input.gfxr output.api\n", program);
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		print_usage(argv[0]);
		return 2;
	}

	VulkanApiCallReader reader;
	if (!register_callbacks(reader)) return 1;
	if (!reader.SetPreCallHook(set_captured_return_value, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct return-value hook");
		return 1;
	}
	if (!reader.SetMemoryUpdateCallback(import_memory_update, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct memory-update callback");
		return 1;
	}
	if (!reader.SetDeviceMemoryPropertiesCallback(import_device_memory_properties, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct device-memory-properties callback");
		return 1;
	}
	if (!reader.SetMissingCallbackPolicy(VulkanNativeMissingCallbackPolicy::kFail))
	{
		ELOG("Failed to enable strict gfxreconstruct callback checking");
		return 1;
	}
	if (!reader.Initialize(argv[1]))
	{
		ELOG("Failed to open gfxreconstruct capture %s", argv[1]);
		return 1;
	}

	lava_writer& writer = lava_writer::instance();
	writer.run = false;
	writer.use_dense_output_handle_indices();
	writer.set_output(argv[2]);
	const bool processed = reader.ProcessAllFrames();
	writer.serialize();
	writer.finish();

	if (!processed)
	{
		if (reader.HasNativeCallError()) print_native_error(reader.GetLastNativeCallError());
		else ELOG("Failed while reading gfxreconstruct capture %s", argv[1]);
		return 1;
	}

	printf("Converted %s to %s\n", argv[1], argv[2]);
	return 0;
}
