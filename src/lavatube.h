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

/// pending memory checking, wait on fence
enum QueueState
{
	QUEUE_STATE_NONE = 0,
	QUEUE_STATE_PENDING_EVENTS = 2,
};

enum
{
	PACKET_API_CALL = 2,
	PACKET_THREAD_BARRIER = 3,
	PACKET_IMAGE_UPDATE = 4,
	PACKET_BUFFER_UPDATE = 5,
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
	change_source last_modified;
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

struct trackedmemory : trackable
{
	using trackable::trackable; // inherit constructor

	// the members below are ephemeral and not saved to disk:

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

	VkDeviceMemory backing = VK_NULL_HANDLE;

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
};

struct trackeddevice : trackable
{
	using trackable::trackable; // inherit constructor
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};

struct trackedobject : trackable
{
	enum class states : uint8_t { uninitialized, initialized, created, destroyed, bound }; // must add at end
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
	bool enables_device_address = false;
	size_t size = 0;
	std::vector<uint32_t> code; // only for replayer
};

struct trackedmemoryobject : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkDeviceAddress device_address = 0;
};

struct trackedbuffer : trackedmemoryobject
{
	using trackedmemoryobject::trackedmemoryobject; // inherit constructor
	VkBufferCreateFlags flags = VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM;
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;
	change_source last_write;

	void self_test() const
	{
		static_assert(offsetof(trackedbuffer, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(flags != VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(usage != VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM);
		assert(object_type == VK_OBJECT_TYPE_BUFFER);
		if (is_state(states::bound)) assert(size != 0);
		trackedobject::self_test();
	}
};

struct trackedaccelerationstructure : trackedmemoryobject
{
	using trackedmemoryobject::trackedmemoryobject; // inherit constructor
	VkBuffer buffer = VK_NULL_HANDLE;
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkAccelerationStructureTypeKHR type = VK_ACCELERATION_STRUCTURE_TYPE_MAX_ENUM_KHR;
	VkDeviceSize offset = 0;

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

struct shader_stage
{
	VkPipelineShaderStageCreateFlags flags;
	VkShaderStageFlagBits stage;
	VkShaderModule module;
	std::string name;
	std::vector<VkSpecializationMapEntry> specialization_constants;
	std::vector<char> specialization_data;
};

struct trackedpipeline : trackable
{
	using trackable::trackable; // inherit constructor
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

	void self_test() const
	{
		trackable::self_test();
	}
};

struct trackedcmdbuffer : trackable
{
	using trackable::trackable; // inherit constructor
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;

	void self_test() const
	{
		static_assert(offsetof(trackedcmdbuffer, magic) == 0, "ICD loader magic must be at offset zero!");
		assert(device != VK_NULL_HANDLE);
		assert(physicalDevice != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		trackable::self_test();
	}
};

struct trackedcommand // does _not_ inherit trackable
{
	lava_function_id id;
	union data
	{
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

struct trackedcmdbuffer_replay : trackedcmdbuffer
{
	using trackedcmdbuffer::trackedcmdbuffer; // inherit constructor

	std::list<trackedcommand> commands; // track select commands for post-processing
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

struct trackeddescriptorset : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
	VkDescriptorPool pool = VK_NULL_HANDLE;

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
