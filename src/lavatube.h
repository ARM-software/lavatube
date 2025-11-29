// Vulkan trace common code

#pragma once

#define LAVATUBE_VERSION_MAJOR 0
#define LAVATUBE_VERSION_MINOR 0
#define LAVATUBE_VERSION_PATCH 1

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "vk_wrapper_auto.h"
#include "util.h"
#include "rangetracking.h"
#include "vulkan_ext.h"
#include "containers.h"
#include "util_auto.h"

#include <unordered_set>
#include <list>
#include <vulkan/vk_icd.h>
#include <type_traits>

// Basically just assuming this will be fixed in later versions of the standard and that existing
// implementations will continue doing the only sensible thing here.
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

class lava_file_reader;
class lava_file_writer;

using lava_trace_func = PFN_vkVoidFunction;

#define LAVATUBE_VIRTUAL_QUEUE 0xbeefbeef

enum
{
	PACKET_VULKAN_API_CALL = 2,
	PACKET_THREAD_BARRIER = 3,
	PACKET_IMAGE_UPDATE = 4,
	PACKET_BUFFER_UPDATE = 5,
	PACKET_VULKANSC_API_CALL = 6,
	PACKET_TENSOR_UPDATE = 7,
};

struct trackable
{
	uintptr_t magic = ICD_LOADER_MAGIC; // in case we want to pass this around as a vulkan object; must be first
	uint32_t index = UINT32_MAX;
	enum class states { uninitialized, initialized, created, destroyed };
	uint8_t state = (uint8_t)states::uninitialized;
	std::string name;
	trackable() {}
	change_source creation;
	change_source last_modified; // used by tracer to track thread synchronization dependencies and by post-processor
	change_source destroyed;

	bool is_state(states s) const { return (uint8_t)s == state; }
	void set_state(states s) { state = (uint8_t)s; }

	void enter_initialized() // call after initialized to transition state and verify contents
	{
		assert(is_state(states::uninitialized));
		set_state(states::initialized);
		self_test();
	}

	void enter_created() // tracer initializes and creates at same time, replayer does it in two steps
	{
		assert(is_state(states::initialized) || is_state(states::uninitialized));
		set_state(states::created);
		self_test();
	}

	void enter_destroyed()
	{
		assert(is_state(states::created));
		set_state(states::destroyed);
		self_test();
	}

	void self_test() const
	{
		// We assume child classes put their fields after parent classes. This is not guaranteed
		// by the standard. If this is not happening, we're in trouble, so assert on it.
		static_assert(offsetof(trackable, magic) == 0, "ICD loader magic must be at offset zero!");
		static_assert(std::is_standard_layout_v<trackable> == true); // only applies to base class

		assert(is_state(states::uninitialized) != (index != UINT32_MAX));
		if (is_state(states::created)) creation.self_test();
		if (is_state(states::created)) last_modified.self_test();
		if (is_state(states::destroyed)) destroyed.self_test();
	}
};

struct trackedobject;

struct trackedmemory : trackable
{
	using trackable::trackable; // inherit constructor
	VkMemoryPropertyFlags propertyFlags = 0;
	/// Current mappping offset
	VkDeviceSize offset = 0;
	/// Current mapping size
	VkDeviceSize size = 0;
	/// Total size
	VkDeviceSize allocationSize = 0;
	/// Sparse copy of entire memory object. Compare against it when diffing
	/// using the touched ranges below. We only do this for memory objects that
	/// are mapped at least once.
	char* clone = nullptr;
	/// Mapped memory area
	char* ptr = nullptr;
	/// If we use external memory, keep track of our allocation
	char* extmem = nullptr;
	/// Tracking all memory exposed to client through memory mapping.
	exposure exposed;
	/// Native handle of the memory
	VkDeviceMemory backing = VK_NULL_HANDLE;
	/// Data structure used to find aliasing objects to make sure we recreate them together again on replay.
	/// For now, this only supports 1-to-1 aliasing. Only used during capture.
	std::multimap<VkDeviceSize, trackedobject*> aliasing;

	void bind(trackedobject* obj);
	void unbind(trackedobject* obj);

	void self_test() const
	{
		static_assert(offsetof(trackedmemory, magic) == 0, "ICD loader magic must be at offset zero!"); \
		assert(backing != VK_NULL_HANDLE);
		assert(offset + size <= allocationSize);
		assert(exposed.span().last <= allocationSize);
		assert(allocationSize != VK_WHOLE_SIZE);
		trackable::self_test();
		assert(p__external_memory == 1 || extmem == nullptr);
	}
};

struct trackedphysicaldevice : trackable
{
	using trackable::trackable; // inherit constructor
	std::vector<VkQueueFamilyProperties> queueFamilyProperties;

	std::unordered_set<std::string> presented_device_extensions; // from tool to app
	std::unordered_set<std::string> supported_device_extensions; // supported in driver
	std::vector<VkExtensionProperties> device_extension_properties; // equal to presented set, but as vector and with version info
};

struct trackeddevice : trackable
{
	using trackable::trackable; // inherit constructor
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	/// Trust host to notify us about memory updates?
	bool explicit_host_updates = false;

	std::unordered_set<std::string> requested_device_extensions; // from app to tool
	std::unordered_set<std::string> enabled_device_extensions; // from replay tool to driver
};

/// Anything that is bound to device memory should inherit from this structure.
struct trackedobject : trackable
{
	enum class states : uint8_t { uninitialized, initialized, created, destroyed, bound }; // must add new states at the end
	using trackable::trackable; // inherit constructor
	VkDeviceMemory backing = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	VkDeviceSize offset = 0; // our offset into our backing memory
	VkMemoryRequirements req = {};
	VkObjectType object_type = VK_OBJECT_TYPE_UNKNOWN;
	uint64_t written = 0; // bytes written out for this object
	uint32_t updates = 0; // number of times it was updated
	bool accessible = false; // whether our backing memory is host visible and understandable
	int source = 0; // code line that is the last source for us to be scanned, only for debugging
	/// Do we alias another object in memory? if we alias 1-to-1, both point to each other, otherwise only the child points to
	/// the parent object.
	VkObjectType alias_type = VK_OBJECT_TYPE_UNKNOWN;
	uint32_t alias_index = UINT32_MAX;
	VkDeviceAddress device_address = 0;
	VkMemoryPropertyFlags memory_flags = 0;

	bool is_state(states s) const { return (uint8_t)s == state; }
	void set_state(states s) { state = (uint8_t)s; }

	void enter_bound()
	{
		assert(is_state(states::created));
		set_state(states::bound);
		self_test();
	}

	void enter_destroyed() // must override to allow exit from bound
	{
		assert(is_state(states::created) || is_state(states::bound));
		set_state(states::destroyed);
		self_test();
	}

	void self_test() const
	{
		static_assert(offsetof(trackedobject, magic) == 0, "ICD loader magic must be at offset zero!"); \
		if (is_state(states::bound)) assert(backing != VK_NULL_HANDLE);
		assert(object_type != VK_OBJECT_TYPE_UNKNOWN);
		assert(size != VK_WHOLE_SIZE);
		trackable::self_test();
	}
};

struct trackedshadermodule : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t device_index = UINT32_MAX;
	bool enables_device_address = false;
	size_t size = 0;
	std::vector<uint32_t> code; // only for replayer
	uint32_t calls = 0; // numbere of times this shader was called
};

/// Tracking device address candidates
struct remap_candidate
{
	VkDeviceAddress address; // contained value
	VkDeviceSize offset; // the offset of the candidate
	change_source source; // last write to memory area from which we came

	remap_candidate(VkDeviceAddress a, VkDeviceSize b, change_source c) { address = a; offset = b; source = c; }
	remap_candidate(const remap_candidate& c) { address = c.address; offset = c.offset; source = c.source; }
	remap_candidate(remap_candidate&& c) { address = c.address; offset = c.offset; source = c.source; }
};

struct trackedbuffer : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkBufferCreateFlags flags = VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM;
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;
	VkBufferUsageFlags2 usage2 = 0;

	void self_test() const
	{
		static_assert(offsetof(trackedbuffer, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(flags != VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(usage != VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM);
		assert(object_type == VK_OBJECT_TYPE_BUFFER);
		if (is_state(states::bound)) assert(size != 0);
		assert(candidates.size() == candidate_lookup.size());
		trackedobject::self_test();
	}

	// -- The below is only used during remap post-processing --

	std::list<remap_candidate> candidates;
	std::unordered_map<VkDeviceSize, std::list<remap_candidate>::iterator> candidate_lookup;

	void add_candidate(VkDeviceSize off, VkDeviceAddress candidate, change_source origin)
	{
		assert(candidate_lookup.count(off) == 0);
		candidates.emplace_back(candidate, off, origin);
		candidate_lookup[off] = --candidates.end();
	}

	void remove_candidate(VkDeviceSize off)
	{
		assert(candidate_lookup.count(off) > 0);
		auto it = candidate_lookup.at(off);
		candidate_lookup.erase(off);
		candidates.erase(it);
	}
};

struct trackedtensor : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkTensorTilingARM tiling = VK_TENSOR_TILING_MAX_ENUM_ARM;
	VkFormat format = VK_FORMAT_MAX_ENUM;
	std::vector<int64_t> dimensions;
	std::vector<int64_t> strides;
	VkTensorUsageFlagsARM usage = 0;

	void self_test() const
	{
		static_assert(offsetof(trackedtensor, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(format != VK_FORMAT_MAX_ENUM);
		assert(tiling != VK_TENSOR_TILING_MAX_ENUM_ARM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(object_type == VK_OBJECT_TYPE_TENSOR_ARM);
		if (is_state(states::bound)) assert(size != 0);
		trackedobject::self_test();
	}
};

struct trackedaccelerationstructure : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkBuffer buffer = VK_NULL_HANDLE;
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkAccelerationStructureTypeKHR type = VK_ACCELERATION_STRUCTURE_TYPE_MAX_ENUM_KHR;
	VkDeviceSize offset = 0;
	VkAccelerationStructureCreateFlagsKHR flags = VK_ACCELERATION_STRUCTURE_CREATE_FLAG_BITS_MAX_ENUM_KHR;

	void self_test() const
	{
		static_assert(offsetof(trackedaccelerationstructure, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(buffer != VK_NULL_HANDLE);
		assert(buffer_index != CONTAINER_INVALID_INDEX);
		assert(type != VK_ACCELERATION_STRUCTURE_TYPE_MAX_ENUM_KHR);
		assert(object_type == VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR);
	}
};

struct trackedimage : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkImageTiling tiling = VK_IMAGE_TILING_MAX_ENUM;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM;
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkImageType imageType = VK_IMAGE_TYPE_MAX_ENUM;
	VkImageCreateFlags flags = VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM;
	VkFormat format = VK_FORMAT_MAX_ENUM;
	bool is_swapchain_image = false;
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
	VkImageLayout currentLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
	VkExtent3D extent {};
	uint32_t mipLevels = 0;
	uint32_t arrayLayers = 0;

	void self_test() const
	{
		static_assert(offsetof(trackedimage, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(tiling != VK_IMAGE_TILING_MAX_ENUM);
		assert(usage != VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(imageType != VK_IMAGE_TYPE_MAX_ENUM);
		assert(flags != VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM);
		assert(format != VK_FORMAT_MAX_ENUM);
		assert(initialLayout != VK_IMAGE_LAYOUT_MAX_ENUM);
		assert(currentLayout != VK_IMAGE_LAYOUT_MAX_ENUM);
		assert(object_type == VK_OBJECT_TYPE_IMAGE);
		trackedobject::self_test();
	}
};

struct trackedimageview : trackable
{
	using trackable::trackable; // inherit constructor
	VkImage image = VK_NULL_HANDLE;
	uint32_t image_index = CONTAINER_INVALID_INDEX;
	VkImageSubresourceRange subresourceRange = {};
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageViewType viewType = (VkImageViewType)0;
	VkComponentMapping components = {};
	VkImageViewCreateFlags flags = (VkImageViewCreateFlags)0;

	void self_test() const
	{
		static_assert(offsetof(trackedimageview, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(image != VK_NULL_HANDLE);
		assert(image_index != CONTAINER_INVALID_INDEX);
		assert(format != VK_FORMAT_UNDEFINED);
		trackable::self_test();
	}
};

struct trackedbufferview : trackable
{
	using trackable::trackable; // inherit constructor
	VkBuffer buffer = VK_NULL_HANDLE;
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkDeviceSize offset = 0;
	VkDeviceSize range = VK_WHOLE_SIZE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkBufferViewCreateFlags flags = (VkBufferViewCreateFlags)0;

	void self_test() const
	{
		static_assert(offsetof(trackedbufferview, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(buffer != VK_NULL_HANDLE);
		assert(buffer_index != CONTAINER_INVALID_INDEX);
		assert(format != VK_FORMAT_UNDEFINED);
		trackable::self_test();
	}
};

struct trackedswapchain : trackable
{
	using trackable::trackable; // inherit constructor
	VkSwapchainCreateInfoKHR info = {};

	void self_test() const
	{
		static_assert(offsetof(trackedswapchain, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(info.sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
		trackable::self_test();
	}
};

struct trackedswapchain_replay : trackedswapchain
{
	using trackedswapchain::trackedswapchain; // inherit constructor
	std::vector<VkImage> pSwapchainImages;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	uint32_t next_swapchain_image = 0;
	uint32_t next_stored_image = 0;
	std::vector<VkImage> virtual_images;
	std::vector<VkCommandBuffer> virtual_cmdbuffers;
	VkCommandPool virtual_cmdpool = VK_NULL_HANDLE;
	VkSemaphore virtual_semaphore = VK_NULL_HANDLE;
	VkImageCopy virtual_image_copy_region = {};
	std::vector<VkFence> virtual_fences;
	std::vector<bool> inflight; // is this entry in use already
	bool initialized = false;
	VkDevice device = VK_NULL_HANDLE;

	void self_test() const
	{
		static_assert(offsetof(trackedswapchain_replay, magic) == 0, "ICD loader magic must be at offset zero!");
		if (!initialized) return;
		assert(swapchain != VK_NULL_HANDLE);
		if (p__virtualswap)
		{
			assert(virtual_cmdpool != VK_NULL_HANDLE);
			assert(virtual_semaphore != VK_NULL_HANDLE);
			for (const auto& v : virtual_cmdbuffers) { assert(v != VK_NULL_HANDLE); (void)v; }
			for (const auto& v : virtual_images) { assert(v != VK_NULL_HANDLE); (void)v; }
			for (const auto& v : virtual_fences) { assert(v != VK_NULL_HANDLE); (void)v; }
		}
		trackedswapchain::self_test();
	}
};

struct trackedfence : trackable
{
	using trackable::trackable; // inherit constructor
	VkFenceCreateFlags flags = (VkFenceCreateFlags)0;

	// tracer only
	int frame_delay = -1; // delay fuse uninitialized
};

struct shader_stage // post-processor only
{
	uint32_t index = CONTAINER_INVALID_INDEX; // our position in our local array of stages
	VkPipelineShaderStageCreateFlags flags = VK_PIPELINE_SHADER_STAGE_CREATE_FLAG_BITS_MAX_ENUM;
	VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
	VkShaderModule module = VK_NULL_HANDLE;
	std::string name;
	std::vector<VkSpecializationMapEntry> specialization_constants;
	std::vector<char> specialization_data;
};

struct trackedpipeline : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t device_index = UINT32_MAX;
	VkPipelineBindPoint type = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	VkPipelineCreateFlags flags = 0;
	VkPipelineCache cache = VK_NULL_HANDLE;
	std::vector<shader_stage> shader_stages; // only set for postprocessing

	void self_test() const
	{
		static_assert(offsetof(trackedpipeline, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(type != VK_PIPELINE_BIND_POINT_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackedpipelinelayout : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t push_constant_space_used = 0;
	VkPipelineLayoutCreateFlags flags = 0;
	std::vector<VkDescriptorSetLayout> layouts;

	void self_test() const
	{
		trackable::self_test();
	}
};

struct trackedcommand // does _not_ inherit trackable
{
	lava_function_id id;
	union data
	{
		struct bind_descriptorsets
		{
			VkPipelineBindPoint pipelineBindPoint;
			VkPipelineLayout layout;
			uint32_t firstSet;
			uint32_t descriptorSetCount;
			uint32_t* pDescriptorSets; // indices
			uint32_t dynamicOffsetCount;
			uint32_t* pDynamicOffsets;
		} bind_descriptorsets;
		struct bind_pipeline
		{
			VkPipelineBindPoint pipelineBindPoint;
			uint32_t pipeline_index;
		} bind_pipeline;
		struct push_constants
		{
			uint32_t offset;
			uint32_t size;
			char* values;
		} push_constants;
		struct update_buffer
		{
			uint32_t offset;
			uint32_t size;
			uint32_t buffer_index;
			char* values;
		} update_buffer;
		struct copy_buffer
		{
			uint32_t src_buffer_index;
			uint32_t dst_buffer_index;
			uint32_t regionCount;
			VkBufferCopy* pRegions;
		} copy_buffer;
	} data;
};

struct trackedcmdbuffer : trackable
{
	using trackable::trackable; // inherit constructor
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
	std::list<trackedcommand> commands; // track select commands for later processing

	void self_test() const
	{
		static_assert(offsetof(trackedcmdbuffer, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(device != VK_NULL_HANDLE);
		assert(physicalDevice != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		trackable::self_test();
	}
};

struct trackedcmdbuffer_trace : trackedcmdbuffer
{
	using trackedcmdbuffer::trackedcmdbuffer; // inherit constructor
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_MAX_ENUM;
	struct
	{
		trackedbuffer* buffer_data = nullptr;
		VkDeviceSize offset = 0;
		VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM; // VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32 or VK_INDEX_TYPE_UINT8_EXT
	} indexBuffer;
	std::unordered_map<trackedobject*, exposure> touched; // track memory updates

	void touch_index_buffer(VkDeviceSize firstIndex, VkDeviceSize indexCount)
	{
		VkDeviceSize size = indexCount;
		VkDeviceSize multiplier = 1;

		if (indexBuffer.indexType == VK_INDEX_TYPE_UINT16) multiplier = 2;
		else if (indexBuffer.indexType == VK_INDEX_TYPE_UINT32) multiplier = 4;

		VkDeviceSize offset = indexBuffer.offset + firstIndex * multiplier;

		if (size == VK_WHOLE_SIZE) size = indexBuffer.buffer_data->size - offset; // indirect indexed draw
		else size *= multiplier;

		assert(offset + size <= indexBuffer.buffer_data->size);
		touch(indexBuffer.buffer_data, offset, size, __LINE__);
	}

	void touch(trackedobject* data, VkDeviceSize offset, VkDeviceSize size, unsigned source)
	{
		if (!data->accessible) return;
		data->source = source;
		if (size == VK_WHOLE_SIZE) size = data->size - offset;
		touched[data].add_os(offset, size);
	}

	void touch_merge(const std::unordered_map<trackedobject*, exposure>& other)
	{
		for (const auto& pair : other)
		{
			if (touched.count(pair.first) > 0) for (auto& r : pair.second.list()) touched[pair.first].add(r.first, r.last); // merge in
			else touched[pair.first] = pair.second; // just insert
		}
	}

	void self_test() const
	{
		static_assert(offsetof(trackedcmdbuffer_trace, magic) == 0, "ICD loader magic must be at offset zero!");
		for (const auto& pair : touched) { assert(pair.first->accessible); pair.first->self_test(); pair.second.self_test(); }
		assert(pool != VK_NULL_HANDLE);
		assert(device != VK_NULL_HANDLE);
		assert(physicalDevice != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		assert(level != VK_COMMAND_BUFFER_LEVEL_MAX_ENUM);
		trackedcmdbuffer::self_test();
	}
};

struct buffer_access
{
	trackedbuffer* buffer_data;
	VkDeviceSize offset;
	VkDeviceSize size; // not sure if we need this
};

struct trackeddescriptorsetlayout : trackable
{
	using trackable::trackable; // inherit constructor
	VkDeviceSize size = 0;
	std::unordered_map<int, VkDeviceSize> offsets;
};

struct trackeddescriptorset : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
	VkDescriptorPool pool = VK_NULL_HANDLE;

	// postprocess only
	std::unordered_map<uint32_t, buffer_access> bound_buffers; // binding point to buffer access
	std::unordered_map<uint32_t, VkDescriptorBufferInfo> dynamic_buffers; // must be resolved on bind

	void self_test() const
	{
		static_assert(offsetof(trackeddescriptorset, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(pool != VK_NULL_HANDLE);
		assert(pool != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		trackable::self_test();
	}
};

struct trackeddescriptorset_trace : trackeddescriptorset
{
	using trackeddescriptorset::trackeddescriptorset; // inherit constructor
	std::unordered_map<trackedobject*, exposure> touched; // track memory updates
	std::vector<VkDescriptorBufferInfo> dynamic_buffers; // must be resolved on bind

	void touch(trackedobject* data, VkDeviceSize offset, VkDeviceSize size, unsigned source)
	{
		if (!data->accessible) return;
		data->source = -(int)source;
		if (size == VK_WHOLE_SIZE) size = data->size - offset;
		touched[data].add_os(offset, size);
	}

	void self_test() const
	{
		static_assert(offsetof(trackeddescriptorset_trace, magic) == 0, "ICD loader magic must be at offset zero!");
		for (const auto& pair : touched) { pair.first->self_test(); pair.second.self_test(); }
		trackeddescriptorset::self_test();
	}
};

struct trackedqueue : trackable
{
	using trackable::trackable; // inherit constructor
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t queueIndex = UINT32_MAX;
	uint32_t queueFamily = UINT32_MAX;
	uint32_t realIndex = UINT32_MAX;
	uint32_t realFamily = UINT32_MAX;
	VkQueue realQueue = VK_NULL_HANDLE;
	VkQueueFlags queueFlags = VK_QUEUE_FLAG_BITS_MAX_ENUM;

	void self_test() const
	{
		static_assert(offsetof(trackedqueue, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(device != VK_NULL_HANDLE);
		assert(physicalDevice != VK_NULL_HANDLE);
		assert(queueFlags != VK_QUEUE_FLAG_BITS_MAX_ENUM);
		assert(realQueue != VK_NULL_HANDLE);
		assert(queueFlags != VK_QUEUE_FLAG_BITS_MAX_ENUM);
		assert(queueIndex != UINT32_MAX);
		assert(realIndex != UINT32_MAX);
		assert(queueFamily != UINT32_MAX);
		assert(realFamily != UINT32_MAX);
		trackable::self_test();
	}
};

struct trackedevent_trace : trackable
{
	using trackable::trackable; // inherit constructor
};

struct trackeddescriptorpool_trace : trackable
{
	using trackable::trackable; // inherit constructor
};

struct trackedcommandpool_trace : trackable
{
	using trackable::trackable; // inherit constructor
	std::unordered_set<trackedcmdbuffer_trace*> commandbuffers;
};

struct trackedrenderpass : trackable
{
	using trackable::trackable; // inherit constructor
	std::vector<VkAttachmentDescription> attachments;
};

struct trackedframebuffer : trackable
{
	using trackable::trackable; // inherit constructor
	std::vector<trackedimageview*> imageviews;
	VkFramebufferCreateFlags flags = (VkFramebufferCreateFlags)0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t layers = 0;

	void self_test() const
	{
		static_assert(offsetof(trackedframebuffer, magic) == 0, "ICD loader magic must be at offset zero!");
		for (const auto v : imageviews) v->self_test();
		trackable::self_test();
	}
};

inline void trackedmemory::bind(trackedobject* obj)
{
	// only 1-to-1 aliasing for now
	auto it = aliasing.find(obj->offset);
	if (it != aliasing.end()) // we are aliasing
	{
		trackedobject* other = it->second;
		assert(obj->offset == it->first); // offset is the same for now
		assert(obj->size == other->size); // size is the same for now
		ILOG("We found aliasing objects %s %u and %s %u at offset %lu", pretty_print_VkObjectType(obj->object_type), obj->index,
		     pretty_print_VkObjectType(other->object_type), other->index, (unsigned long)obj->offset);
		other->alias_type = obj->object_type;
		other->alias_index = obj->index;
		obj->alias_type = other->object_type;
		obj->alias_index = other->index;
	}
	aliasing.insert({obj->offset, obj});
}

inline void trackedmemory::unbind(trackedobject* obj)
{
	auto it = aliasing.find(obj->offset);
	assert(it != aliasing.end());
	for (; it != aliasing.end(); ++it)
	{
		if (it->first == obj->offset)
		{
			aliasing.erase(it);
			return;
		}
	}
}
