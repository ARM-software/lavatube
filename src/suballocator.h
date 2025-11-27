#pragma once

#include "lavatube.h"

struct suballoc_location
{
	VkDeviceMemory memory;
	VkDeviceSize offset;
	VkDeviceSize size;
	bool needs_init;
	bool needs_flush;
};

struct suballocator_private;

struct suballocator
{
	/// Call as early as possible to set up internal data structures. Must be called before any other suballoc function.
	void init(int num_images, int num_buffers, int tensors, int heap_size = -1, bool fake = false);

	suballocator();
	~suballocator();

	void destroy(VkDevice device);

	/// Call after Vulkan instance has been initialized, and before calling any other suballoc function except suballoc_init().
	/// Queries information and caches it for later.
	void setup(VkPhysicalDevice physicaldevice);

	/// Add an image to our memory pools. Thread safe because each thread gets its own set of memory pools that only they
	/// can modify. Other threads may access the objects stored inside subject to Vulkan external synchronization rules.
	suballoc_location add_image(uint16_t tid, VkDevice device, VkImage image, uint32_t image_index, VkMemoryPropertyFlags flags, VkImageTiling tiling, VkDeviceSize min_size);

	/// Add a buffer to our memory pools. See above.
	suballoc_location add_buffer(uint16_t tid, VkDevice device, VkBuffer buffer, VkMemoryPropertyFlags memory_flags, const trackedbuffer& buffer_data);

	/// Delete an image from our memory pools. Thread safe because the internal data structure is preallocated and never resized,
	/// and deleted entries are never reused.
	void free_image(uint32_t image_index);

	/// Delete a buffer from our memory pools. See above.
	void free_buffer(uint32_t buffer_index);

	/// Find an image based its index, and return its memory pool, offset and size. Thread safe as long as the usual Vulkan
	/// external synchronization rules are followed in regards to object creation. Returns if an explicit flush is needed.
	/// Note that the size returned is the (possibly padded) allocated size, not the size of the buffer inside the allocation.
	suballoc_location find_image_memory(uint32_t buffer_index);

	/// Find a buffer based its index, and return its memory pool, offset and size. See above. Returns if an explicit flush
	/// is needed.
	/// Note that the size returned is the (possibly padded) allocated size, not the size of the image inside the allocation.
	suballoc_location find_buffer_memory(uint32_t buffer_index);

	suballoc_location find_tensor_memory(uint32_t tensor_index);

	/// Check that our internal structures are internally consistent and abort if not. (This is NOT thread-safe!)
	int self_test();

	/// Special handling of virtual swapchain images
	void virtualswap_images(VkDevice device, const std::vector<VkImage>& images, VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

private:
	suballocator_private* priv = nullptr;
};
