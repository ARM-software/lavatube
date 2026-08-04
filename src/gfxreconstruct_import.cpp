#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
using gfxrecon::decode::VulkanResourceInitializationBegin;
using gfxrecon::decode::VulkanBufferInitialization;
using gfxrecon::decode::VulkanImageInitialization;
using gfxrecon::decode::VulkanResourceInitializationEnd;
using gfxrecon::decode::VulkanDeviceAddressFixup;
using gfxrecon::decode::VulkanDeviceAddressFixups;
using gfxrecon::decode::VulkanShaderGroupHandleFixup;
using gfxrecon::decode::VulkanShaderGroupHandleFixups;
using gfxrecon::decode::VulkanUnhandledMetaCommand;

struct gfxreconstruct_import_context
{
	std::unordered_map<uint64_t, std::vector<VulkanDeviceAddressFixup>> device_address_fixups;
	std::unordered_map<uint64_t, std::vector<VulkanShaderGroupHandleFixup>> shader_group_handle_fixups;
	std::unordered_set<VkDeviceMemory> warned_non_host_visible_memory;
	bool ignore_memory_marking_fixups = false;
};

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

static VkResult VKAPI_CALL import_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	trackedphysicaldevice* physical_device_data = lava_writer::instance().records.VkPhysicalDevice_index.at(physicalDevice);
	if (!physical_device_data->has_imported_memory_properties)
	{
		DIE("Missing gfxreconstruct memory properties for imported physical device[%u]", physical_device_data->index);
	}
	VkPhysicalDeviceMemoryProperties memory_properties = physical_device_data->imported_memory_properties;
	trace_vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory_properties);
	return trace_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
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

static void import_memory_update(void* user_data, const VulkanMemoryUpdate& update)
{
	auto* context = static_cast<gfxreconstruct_import_context*>(user_data);
	if (update.memory == VK_NULL_HANDLE || !update.data || update.data_size == 0)
	{
		DIE("Invalid gfxreconstruct memory update at block %lu", (unsigned long)update.block_index);
	}
	lava_writer& writer = lava_writer::instance();
	trackedmemory* memory_data = writer.records.VkDeviceMemory_index.at(update.memory);
	const uint64_t relation_id = reinterpret_cast<uintptr_t>(update.memory);
	if ((memory_data->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
	{
		if (context->warned_non_host_visible_memory.insert(update.memory).second)
		{
			WLOG("Ignoring gfxreconstruct memory updates for non-host-visible VkDeviceMemory[%u]", memory_data->index);
		}
		context->device_address_fixups.erase(relation_id);
		context->shader_group_handle_fixups.erase(relation_id);
		return;
	}
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
		std::vector<VkMarkingTypeARM> marking_types;
		std::vector<VkMarkingSubTypeARM> marking_subtypes;
		std::vector<VkDeviceSize> marking_offsets;
		const auto address_it = context->device_address_fixups.find(relation_id);
		if (address_it != context->device_address_fixups.end() && object_data->object_type == VK_OBJECT_TYPE_BUFFER)
		{
			for (const VulkanDeviceAddressFixup& fixup : address_it->second)
			{
				if (fixup.data_offset < overlap_start || fixup.data_offset + sizeof(VkDeviceAddress) > overlap_end) continue;
				VkMarkingSubTypeARM subtype = {};
				const VkAccelerationStructureKHR acceleration_structure =
					reinterpret_cast<VkAccelerationStructureKHR>(static_cast<uintptr_t>(fixup.object_id));
				subtype.deviceAddressType = writer.records.VkAccelerationStructureKHR_index.contains(acceleration_structure)
					? VK_DEVICE_ADDRESS_TYPE_ACCELERATION_STRUCTURE_ARM : VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM;
				marking_types.push_back(VK_MARKING_TYPE_DEVICE_ADDRESS_ARM);
				marking_subtypes.push_back(subtype);
				marking_offsets.push_back(fixup.data_offset - object_start);
			}
		}
		const auto shader_it = context->shader_group_handle_fixups.find(relation_id);
		if (shader_it != context->shader_group_handle_fixups.end() && object_data->object_type == VK_OBJECT_TYPE_BUFFER)
		{
			for (const VulkanShaderGroupHandleFixup& fixup : shader_it->second)
			{
				if (fixup.group_size != gfxrecon::decode::kVulkanShaderGroupHandleSize)
				{
					WLOG("Ignoring gfxreconstruct shader group handle fixup with unsupported size %u", fixup.group_size);
					continue;
				}
				if (fixup.data_offset < overlap_start || fixup.data_offset + fixup.group_size > overlap_end) continue;
				VkMarkingSubTypeARM subtype = {};
				marking_types.push_back(VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM);
				marking_subtypes.push_back(subtype);
				marking_offsets.push_back(fixup.data_offset - object_start);
			}
		}
		VkMarkedOffsetsARM markings = { VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM, nullptr };
		markings.count = marking_offsets.size();
		markings.pMarkingTypes = marking_types.data();
		markings.pSubTypes = marking_subtypes.data();
		markings.pOffsets = marking_offsets.data();
		write_object_update_packet(writer.file_writer(), devices.at(object_data->parent_device_index), object_data,
			overlap_start - object_start, data, overlap_end - overlap_start, markings.count ? &markings : nullptr);
		emitted++;
	}
	context->device_address_fixups.erase(relation_id);
	context->shader_group_handle_fixups.erase(relation_id);
	if (emitted == 0)
	{
		WLOG("gfxreconstruct memory update at block %lu did not overlap a bound buffer, image, or tensor",
			(unsigned long)update.block_index);
	}
}

static void import_device_address_fixups(void* user_data, const VulkanDeviceAddressFixups& fixups)
{
	auto* context = static_cast<gfxreconstruct_import_context*>(user_data);
	if (context->ignore_memory_marking_fixups) return;
	context->device_address_fixups[fixups.relation_id] =
		std::vector<VulkanDeviceAddressFixup>(fixups.fixups, fixups.fixups + fixups.fixup_count);
}

static void import_shader_group_handle_fixups(void* user_data, const VulkanShaderGroupHandleFixups& fixups)
{
	auto* context = static_cast<gfxreconstruct_import_context*>(user_data);
	if (context->ignore_memory_marking_fixups) return;
	context->shader_group_handle_fixups[fixups.relation_id] =
		std::vector<VulkanShaderGroupHandleFixup>(fixups.fixups, fixups.fixups + fixups.fixup_count);
}

static void warn_unhandled_metacommand(void*, const VulkanUnhandledMetaCommand& command)
{
	WLOG("Unhandled gfxreconstruct metacommand 0x%x at block %lu", command.metadata_id,
		(unsigned long)command.block_index);
}

static void import_device_memory_properties(void*, const VulkanDeviceMemoryProperties& properties)
{
	lava_file_writer& file = lava_writer::instance().file_writer();
	if (!file.parent->records.VkPhysicalDevice_index.contains(properties.physical_device))
	{
		auto* data = file.parent->records.VkPhysicalDevice_index.add(properties.physical_device, file.current,
			file.parent->desired_output_handle_index(properties.physical_device));
		data->enter_initialized();
	}
	trackedphysicaldevice* physical_device_data = file.parent->records.VkPhysicalDevice_index.at(properties.physical_device);
	physical_device_data->imported_memory_properties = properties.memory_properties;
	physical_device_data->has_imported_memory_properties = true;
}

static void import_resource_initialization_begin(void*, const VulkanResourceInitializationBegin& initialization)
{
	(void)initialization;
}

static VkResult VKAPI_CALL import_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer)
{
	VkBufferCreateInfo create_info = *pCreateInfo;
	create_info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	return trace_vkCreateBuffer(device, &create_info, pAllocator, pBuffer);
}

static void import_buffer_initialization(void*, const VulkanBufferInitialization& initialization)
{
	if (initialization.buffer == VK_NULL_HANDLE || !initialization.data || initialization.data_size == 0)
	{
		DIE("Invalid gfxreconstruct buffer initialization at block %lu", (unsigned long)initialization.block_index);
	}
	lava_writer& writer = lava_writer::instance();
	trackedbuffer* buffer_data = writer.records.VkBuffer_index.at(initialization.buffer);
	trackeddevice* device_data = writer.records.VkDevice_index.at(initialization.device);
	if (initialization.data_size > buffer_data->size)
	{
		DIE("Out-of-range gfxreconstruct initialization for buffer[%u]: buffer size %lu, update size %lu",
			buffer_data->index, (unsigned long)buffer_data->size, (unsigned long)initialization.data_size);
	}
	lava_file_writer& file = writer.file_writer();
	file.begin_packet(PACKET_BUFFER_INITIALIZATION);
	file.write_handle(device_data);
	file.write_handle(buffer_data);
	file.write_uint64_t(initialization.data_size);
	file.write_array(initialization.data, initialization.data_size);
	buffer_data->updates++;
	buffer_data->written += initialization.data_size;
	file.end_packet();
}

static void import_image_initialization(void*, const VulkanImageInitialization& initialization)
{
	if (initialization.image == VK_NULL_HANDLE || !initialization.data || initialization.data_size == 0)
	{
		DIE("Invalid gfxreconstruct image initialization at block %lu", (unsigned long)initialization.block_index);
	}
	lava_writer& writer = lava_writer::instance();
	trackedimage* image_data = writer.records.VkImage_index.at(initialization.image);
	trackeddevice* device_data = writer.records.VkDevice_index.at(initialization.device);
	lava_file_writer& file = writer.file_writer();
	file.begin_packet(PACKET_IMAGE_INITIALIZATION);
	file.write_handle(device_data);
	file.write_handle(image_data);
	file.write_uint32_t(initialization.aspect_mask);
	file.write_uint32_t(initialization.layout);
	file.write_uint32_t(initialization.level_count);
	file.write_array(initialization.level_sizes, initialization.level_count);
	file.write_uint64_t(initialization.data_size);
	file.write_array(initialization.data, initialization.data_size);
	image_data->updates++;
	image_data->written += initialization.data_size;
	file.end_packet();
}

static void import_resource_initialization_end(void*, const VulkanResourceInitializationEnd& initialization)
{
	(void)initialization;
}

static PFN_vkVoidFunction import_callback(const std::string& name, void* callback)
{
	if (name == "vkEnumeratePhysicalDevices")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkEnumeratePhysicalDevices);
	if (name == "vkGetPhysicalDeviceProperties")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkGetPhysicalDeviceProperties);
	if (name == "vkCreateDevice")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateDevice);
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
	if (name == "vkCreateBuffer")
		return reinterpret_cast<PFN_vkVoidFunction>(import_vkCreateBuffer);
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
	printf("Usage: %s [options] input.gfxr output.api\n", program);
	printf("-h/--help                          This help\n");
	printf("--ignore-memory-marking-fixups     Ignore device-address and shader-group-handle fixup metacommands\n");
}

void usage()
{
	print_usage("lava-gfxr-import");
	exit(-1);
}

int main(int argc, char** argv)
{
	int remaining = argc - 1;
	std::string input_filename;
	std::string output_filename;
	gfxreconstruct_import_context import_context;
	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			print_usage(argv[0]);
			return 0;
		}
		else if (match(argv[i], nullptr, "--ignore-memory-marking-fixups", remaining))
		{
			import_context.ignore_memory_marking_fixups = true;
		}
		else if (strcmp(argv[i], "--") == 0)
		{
			remaining--;
			if (remaining > 0) input_filename = get_str(argv[++i], remaining);
			if (remaining > 0) output_filename = get_str(argv[++i], remaining);
			if (remaining > 0)
			{
				print_usage(argv[0]);
				return 2;
			}
			break;
		}
		else
		{
			input_filename = get_str(argv[i], remaining);
			if (remaining > 0) output_filename = get_str(argv[++i], remaining);
			if (remaining > 0)
			{
				print_usage(argv[0]);
				return 2;
			}
		}
	}
	if (input_filename.empty() || output_filename.empty())
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
	if (!reader.SetMemoryUpdateCallback(import_memory_update, &import_context))
	{
		ELOG("Failed to install the gfxreconstruct memory-update callback");
		return 1;
	}
	if (!reader.SetDeviceMemoryPropertiesCallback(import_device_memory_properties, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct device-memory-properties callback");
		return 1;
	}
	if (!reader.SetResourceInitializationBeginCallback(import_resource_initialization_begin, nullptr) ||
		!reader.SetBufferInitializationCallback(import_buffer_initialization, nullptr) ||
		!reader.SetImageInitializationCallback(import_image_initialization, nullptr) ||
		!reader.SetResourceInitializationEndCallback(import_resource_initialization_end, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct resource-initialization callbacks");
		return 1;
	}
	if (!reader.SetDeviceAddressFixupCallback(import_device_address_fixups, &import_context) ||
		!reader.SetShaderGroupHandleFixupCallback(import_shader_group_handle_fixups, &import_context) ||
		!reader.SetUnhandledMetaCommandCallback(warn_unhandled_metacommand, nullptr))
	{
		ELOG("Failed to install the gfxreconstruct metacommand callbacks");
		return 1;
	}
	if (!reader.SetMissingCallbackPolicy(VulkanNativeMissingCallbackPolicy::kFail))
	{
		ELOG("Failed to enable strict gfxreconstruct callback checking");
		return 1;
	}
	if (!reader.Initialize(input_filename.c_str()))
	{
		ELOG("Failed to open gfxreconstruct capture %s", input_filename.c_str());
		return 1;
	}

	lava_writer& writer = lava_writer::instance();
	writer.run = false;
	writer.use_dense_output_handle_indices();
	writer.set_output(output_filename);
	const bool processed = reader.ProcessAllFrames();
	writer.serialize();
	writer.finish();

	if (!processed)
	{
		if (reader.HasNativeCallError()) print_native_error(reader.GetLastNativeCallError());
		else ELOG("Failed while reading gfxreconstruct capture %s", input_filename.c_str());
		return 1;
	}

	printf("Converted %s to %s\n", input_filename.c_str(), output_filename.c_str());
	return 0;
}
