#pragma once

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "util.h"
#include "vulkan_ext.h"

struct suballoc_location
{
	VkDeviceMemory memory;
	VkDeviceSize offset;
	VkDeviceSize size;
	bool needs_init;
	bool needs_flush;
};

struct suballocator_private;
struct memory_requirements;
struct trackedobject;
struct trackedimage;
struct trackedtensor;
struct trackedbuffer;

struct suballoc_metrics
{
	uint64_t used = 0;
	uint64_t allocated = 0;
	uint32_t heaps = 0;
	uint32_t objects = 0;
	double efficiency = 0.0;
};

struct suballocator
{
	/// Call this when our parent device object is created, and before calling any other suballoc function. Set `run` to false if we are not actually running the API, but instead
	/// doing some post-processing.
	void create(VkPhysicalDevice physicaldevice, VkDevice device, const std::vector<trackedimage>& images, const std::vector<trackedbuffer>& buffers, const std::vector<trackedtensor>& tensors, bool run);

	/// Call when our parent device is destroyed.
	void destroy();

	/// Get performance metrics for the suballocator. This is not thread safe.
	suballoc_metrics performance() const;

	suballocator();
	~suballocator();

	/// Add an object to our memory pools. Thread safe because each thread gets its own set of memory pools that only they
	/// can modify. Other threads may access the objects stored inside subject to Vulkan external synchronization rules.
	suballoc_location add_trackedobject(uint16_t tid, const memory_requirements& reqs, uint64_t native, const trackedobject& data);

	/// Delete an image from our memory pools. Thread safe because the internal data structure is preallocated and never resized,
	/// and deleted entries are never reused.
	void free_image(uint32_t image_index);

	/// Delete a buffer from our memory pools. See above.
	void free_buffer(uint32_t buffer_index);

	/// Delete a tensor from our memory pools. See above.
	void free_tensor(uint32_t buffer_index);

	/// Find an image based its index, and return its memory pool, offset and size. Thread safe as long as the usual Vulkan
	/// external synchronization rules are followed in regards to object creation. Returns if an explicit flush is needed.
	/// Note that the size returned is the (possibly padded) allocated size, not the size of the image inside the allocation.
	suballoc_location find_image_memory(uint32_t buffer_index) const;

	/// Find a buffer based its index, and return its memory pool, offset and size. See above. Returns if an explicit flush
	/// is needed.
	/// Note that the size returned is the (possibly padded) allocated size, not the size of the buffer inside the allocation.
	suballoc_location find_buffer_memory(uint32_t buffer_index) const;

	suballoc_location find_tensor_memory(uint32_t tensor_index) const;

	/// Check that our internal structures are internally consistent and abort if not. (This is NOT thread-safe!)
	int self_test() const;

	/// Special handling of virtual swapchain images
	void virtualswap_images(const std::vector<VkImage>& images, VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

private:
	suballocator_private* priv = nullptr;
};
