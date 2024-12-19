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

#include <unordered_set>
#include <list>
#include <vulkan/vk_icd.h>

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
	uintptr_t magic = ICD_LOADER_MAGIC; // in case we want to pass this around as a vulkan object
	uint32_t index = 0;
	std::string name;
	trackable() {}
	change_source creation;
	change_source last_modified;
	change_source destroyed;

	void self_test() const
	{
		creation.self_test();
		last_modified.self_test();
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
	using trackable::trackable; // inherit constructor
	VkDeviceMemory backing = (VkDeviceMemory)0;
	VkDeviceSize size = 0;
	VkDeviceSize offset = 0; // our offset into our backing memory
	VkMemoryRequirements req = {};
	VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
	uint64_t written = 0; // bytes written out for this object
	uint32_t updates = 0; // number of times it was updated
	bool accessible = false; // whether our backing memory is host visible and understandable
	int source = 0; // code line that is the last source for us to be scanned, only for debugging

	void self_test() const
	{
		assert(type != VK_OBJECT_TYPE_UNKNOWN);
		assert(size != VK_WHOLE_SIZE);
		trackable::self_test();
	}
};

struct trackedshadermodule : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	bool enables_device_address = false;
	size_t size = 0;
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
		assert(flags != VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(usage != VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM);
		assert(type == VK_OBJECT_TYPE_BUFFER);
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
		assert(buffer != VK_NULL_HANDLE);
		assert(buffer_index != CONTAINER_INVALID_INDEX);
		assert(type != VK_ACCELERATION_STRUCTURE_TYPE_MAX_ENUM_KHR);
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
		assert(tiling != VK_IMAGE_TILING_MAX_ENUM);
		assert(usage != VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(imageType != VK_IMAGE_TYPE_MAX_ENUM);
		assert(flags != VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM);
		assert(format != VK_FORMAT_MAX_ENUM);
		assert(initialLayout != VK_IMAGE_LAYOUT_MAX_ENUM);
		assert(currentLayout != VK_IMAGE_LAYOUT_MAX_ENUM);
		assert(type == VK_OBJECT_TYPE_IMAGE);
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
		assert(info.sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
		trackable::self_test();
	}
};

struct trackedswapchain_trace : trackedswapchain
{
	using trackedswapchain::trackedswapchain; // inherit constructor

	void self_test() const
	{
		trackedswapchain::self_test();
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

struct trackedpipeline : trackable
{
	using trackable::trackable; // inherit constructor
	VkPipelineBindPoint type = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	VkPipelineCreateFlags flags = 0;
	VkPipelineCache cache = VK_NULL_HANDLE;

	void self_test() const
	{
		assert(type != VK_PIPELINE_BIND_POINT_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackedcmdbuffer_trace : trackable
{
	using trackable::trackable; // inherit constructor
	VkCommandPool pool = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
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
		for (const auto& pair : touched) { assert(pair.first->accessible); pair.first->self_test(); pair.second.self_test(); }
		assert(pool != VK_NULL_HANDLE);
		assert(device != VK_NULL_HANDLE);
		assert(physicalDevice != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		assert(level != VK_COMMAND_BUFFER_LEVEL_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackeddescriptorset_trace : trackable
{
	using trackable::trackable; // inherit constructor
	VkDescriptorPool pool = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
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
		assert(pool != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		for (const auto& pair : touched) { pair.first->self_test(); pair.second.self_test(); }
		trackable::self_test();
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
		for (const auto v : imageviews) v->self_test();
		trackable::self_test();
	}
};
