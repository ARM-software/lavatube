#include "suballocator.h"

#include <memory>
#include <vector>
#include <functional>
#include "tbb/concurrent_vector.h"
#include "containers.h"

// --* Vulkan memory suballocator *--
// Each thread has its own set of heaps.
//
// Calling the delete functions is safe because the callee is responsible for making sure
// that other acccesses to the same entry do not happen when we delete it.

// TBD Turn this into a class and instanciate one suballocator for each device. This way we can safely
// handle multi-device cases. Right now they would share device memory pools, which is not legal.

#define SUBALLOC_ABORT(_priv, _format, ...) do { _priv->suballoc_print(p__debug_destination); fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); fflush(p__debug_destination); abort(); } while(0)

struct suballocation
{
	VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
	union
	{
		uint64_t native;
		VkImage image;
		VkBuffer buffer;
		VkTensorARM tensor;
	} handle;
	VkDeviceSize size = 0;
	VkDeviceSize offset = 0;
	VkDeviceSize alignment = 0;
	uint32_t index = 0;
};

struct heap
{
	uint_fast16_t tid;
	uint_fast16_t memoryTypeIndex;
	VkDeviceMemory mem;
	VkDeviceSize free;
	VkDeviceSize total;
	/// This one does not need to be concurrent safe, since each thread owns its own heap
	/// and only it may iterate over and modify the allocations list.
	std::list<suballocation> subs;
	/// We cannot allow other threads to delete anything in our list, so they can queue
	/// up deletes in this concurrency safe vector instead.
	tbb::concurrent_vector<uint32_t> deletes;
	lava_tiling tiling = TILING_LINEAR; // default assumed to be linear

	void self_test() const
	{
		assert(free <= total);
	}
};

struct lookup
{
	heap* home = nullptr;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	bool initialized = false;
	lookup(heap* h, VkDeviceSize o, VkDeviceSize s) : home(h), offset(o), size(s) {}
	lookup() {}
};

struct suballocator_private
{
	uint64_t min_heap_size = 1024 * 1024 * 32; // default 32mb size heaps
	std::vector<VkDeviceMemory> virtualswapmemory;
	bool run = true; // whether we actually run things or just fake it
	tbb::concurrent_vector<heap> heaps;
	VkPhysicalDeviceMemoryProperties memory_properties;
	std::vector<lookup> image_lookup;
	std::vector<lookup> buffer_lookup;
	std::vector<lookup> tensor_lookup;
	/// Does this device have the an annoying optimal-to-linear padding requirement? If so, put optimal and linear objects in different memory heaps
	bool allow_mixed_tiling = true;

	void print_memory_usage();
	uint32_t get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
	suballoc_location add_object_new(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
		lava_tiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags);
	bool fill_image_memreq(VkDevice device, VkImage image, VkMemoryRequirements2& req, VkDeviceSize size);
	void suballoc_print(FILE* fp);
	suballoc_location add_object(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
		lava_tiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags);
	void self_test();
	void bind(heap& h, const suballocation& s);

	inline bool needs_flush(unsigned memoryTypeIndex) { return !(memory_properties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
};

// helpers

static VkMemoryPropertyFlags prune_memory_flags(VkMemoryPropertyFlags flags)
{
	if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) || (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
	{
		flags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; // do not require this bit in these cases
	}
	return flags;
}

void suballocator_private::print_memory_usage()
{
	printf("Suballocator memory usage:\n");
	int i = 0;
	uint64_t total = 0;
	uint64_t waste = 0;
	for (const heap& h : heaps)
	{
		printf("Heap %d : tid=%u memorytype=%u tiling=%u free=%lu total=%lu allocs=%u pending deletes=%u\n",
		       i, (unsigned)h.tid, (unsigned)h.memoryTypeIndex, (unsigned)h.tiling, (unsigned long)h.free, (unsigned long)h.total,
		       (unsigned)h.subs.size(), (unsigned)h.deletes.size());
		total += h.total;
		waste += h.free;
		i++;
	}
	printf("Total memory used:   %10lu\n", (unsigned long)total);
	printf("Total memory wasted: %10lu\n", (unsigned long)waste);
}

uint32_t suballocator_private::get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
	{
		if (type_filter & (1 << i) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	// Oops, try to simplify our request!
	properties &= ~(VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
	{
		if (type_filter & (1 << i) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	ILOG("Memory flags requested:");
	if (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ILOG("\tDEVICE_LOCAL");
	if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ILOG("\tHOST_VISIBLE");
	if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ILOG("\tHOST_COHERENT");
	if (properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ILOG("\tHOST_CACHED");
	if (properties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ILOG("\tLAZILY_ALLOCATED");
	if (properties & VK_MEMORY_PROPERTY_PROTECTED_BIT) ILOG("\tPROTECTED");
	SUBALLOC_ABORT(this, "Failed to find required memory type (filter=%u, props=%u)", type_filter, (unsigned)properties);
	return 0xffff; // satisfy compiler
}

// API

suballocator::suballocator()
{
	priv = new suballocator_private;
}

void suballocator::init(int num_images, int num_buffers, int num_tensors, int heap_size, bool fake)
{
	priv->run = !fake;
	priv->image_lookup.resize(num_images);
	priv->buffer_lookup.resize(num_buffers);
	priv->tensor_lookup.resize(num_tensors);
	memset(&priv->memory_properties, 0, sizeof(priv->memory_properties));
	if (heap_size != -1) priv->min_heap_size = heap_size;
}

suballocator::~suballocator()
{
	delete priv;
}

void suballocator::setup(VkPhysicalDevice physicaldevice)
{
	assert(priv->memory_properties.memoryTypeCount == 0);
	if (priv->run)
	{
		wrap_vkGetPhysicalDeviceMemoryProperties(physicaldevice, &priv->memory_properties);
		VkPhysicalDeviceProperties pdprops = {};
		wrap_vkGetPhysicalDeviceProperties(physicaldevice, &pdprops);
		priv->allow_mixed_tiling = (pdprops.limits.bufferImageGranularity == 1);
	}
	else
	{
		priv->memory_properties.memoryTypeCount = 1;
		priv->memory_properties.memoryTypes[0].heapIndex = 0;
		priv->memory_properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_FLAG_BITS_MAX_ENUM;
		priv->memory_properties.memoryHeapCount = 1;
		priv->memory_properties.memoryHeaps[0].size = UINT64_MAX;
		priv->memory_properties.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
		priv->allow_mixed_tiling = true;
	}
	assert(priv->memory_properties.memoryTypeCount > 0);
}

suballoc_location suballocator_private::add_object_new(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
        lava_tiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags)
{
	heap h;
	h.tid = tid;
	VkMemoryDedicatedAllocateInfoTensorARM tensorded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM, nullptr };
	VkMemoryDedicatedAllocateInfoKHR dedinfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr };
	VkMemoryAllocateFlagsInfo flaginfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr };
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flaginfo };
	flaginfo.flags = allocflags;
	flaginfo.deviceMask = 0; // TBD
	if (dedicated)
	{
		flaginfo.pNext = &dedinfo;
		if (s.type == VK_OBJECT_TYPE_BUFFER)
		{
			dedinfo.buffer = s.handle.buffer;
		}
		else if (s.type == VK_OBJECT_TYPE_TENSOR_ARM)
		{
			tensorded.tensor = s.handle.tensor;
			flaginfo.pNext = &tensorded;
		}
		else
		{
			assert(s.type == VK_OBJECT_TYPE_IMAGE);
			dedinfo.image = s.handle.image;
		}
		info.allocationSize = s.size;
	}
	else
	{
		info.allocationSize = std::max<VkDeviceSize>(min_heap_size, s.size);
	}
	assert(info.allocationSize < 1024 * 1024 * 1024); // 1 gig max for sanity's sake
	info.memoryTypeIndex = memoryTypeIndex;
	if (run)
	{
		VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &h.mem);
		if (result != VK_SUCCESS)
		{
			print_memory_usage();
			SUBALLOC_ABORT(this, "Failed to allocate %lu bytes of memory for memory type %u and tiling %u", (unsigned long)info.allocationSize, (unsigned)memoryTypeIndex, (unsigned)tiling);
		}
	}
	else
	{
		h.mem = (VkDeviceMemory)malloc(info.allocationSize);
	}
	h.free = info.allocationSize - s.size;
	h.total = info.allocationSize;
	h.memoryTypeIndex = memoryTypeIndex;
	h.tiling = tiling;
	h.subs.push_back(s);
	DLOG2("allocating new memory pool with size = %lu, free = %lu (memoryTypeIndex=%u, tiling=%u)", (unsigned long)info.allocationSize,
	      (unsigned long)h.free, (unsigned)memoryTypeIndex, (unsigned)tiling);
	auto it = heaps.push_back(h);
	s.offset = 0;
	bind(*it, s);
	return { h.mem, 0, s.size, true, needs_flush(memoryTypeIndex) };
}

suballoc_location suballocator_private::add_object(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
	lava_tiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags)
{
	if (dedicated)
	{
		return add_object_new(device, tid, memoryTypeIndex, s, flags, tiling, dedicated, allocflags);
	}
	for (heap& h : heaps)
	{
		VkMemoryPropertyFlags f = memory_properties.memoryTypes[h.memoryTypeIndex].propertyFlags;
		// this is a safe time to actually delete things
		if (!h.deletes.empty())
		{
			for (auto it = h.subs.begin(); it != h.subs.end(); ++it)
			{
				for (auto d = h.deletes.cbegin(); d != h.deletes.cend(); ++d)
				{
					if (it->offset == *d)
					{
						h.free += it->size;
						DLOG3("finalized delete in heap=%p off=%lu size=%lu, total free is %lu", &h, (unsigned long)*d,
						      (unsigned long)it->size, (unsigned long)h.free);
						it = h.subs.erase(it);
						break;
					}
				}
			}
			h.deletes.clear();
		}
		// find suballocation
		if (h.tid == tid && (flags & f) == flags && h.free >= s.size && h.memoryTypeIndex == memoryTypeIndex && (h.tiling == tiling || allow_mixed_tiling))
		{
			// First case: nothing allocated in heap. In this case, we do not care about alignment, because according to the spec:
			// "Allocations returned by vkAllocateMemory are guaranteed to meet any alignment requirement of the implementation."
			// Also second case: We place our allocation first. Like above, we are guaranteed not to have to care about alignment.
			if (h.subs.empty() || (h.subs.front().offset >= s.size))
			{
				s.offset = 0;
				bind(h, s); // call to vkBind{Buffer|Image}Memory
				h.subs.push_front(s);
				h.free -= s.size;
				DLOG3("inserting object into memory at the front size=%lu, alignment=%u, free is %lu", (unsigned long)s.size, (unsigned)s.alignment, (unsigned long)h.free);
				return { h.mem, 0, s.size, true, needs_flush(h.memoryTypeIndex) };
			}
			// Third case: scan for unused memory segment of the correct size. We need to make sure it is aligned correctly.
			for (auto it = h.subs.begin(); it != h.subs.end(); ++it)
			{
				void* start = (void *)(it->offset + it->size); // make into fake pointer for use with std::align
				auto next = std::next(it);
				if (next == h.subs.end()) // we are at the end
				{
					std::size_t left = h.total - (it->offset + it->size);
					if (std::align(s.alignment, s.size, start, left))
					{
						h.free -= s.size;
						DLOG3("inserting object into memory at the end size=%lu, alignment=%u, free is %lu", (unsigned long)s.size, (unsigned)s.alignment, (unsigned long)h.free);
						s.offset = (VkDeviceSize)start;
						bind(h, s); // call to vkBind{Buffer|Image}Memory
						h.subs.push_back(s);
						return { h.mem, s.offset, s.size, true, needs_flush(h.memoryTypeIndex) };
					}
					break; // no space found
				}
				std::size_t hole_size = next->offset - (it->offset + it->size);
				if (std::align(s.alignment, s.size, start, hole_size))
				{
					s.offset = (VkDeviceSize)start;
					bind(h, s); // call to vkBind{Buffer|Image}Memory
					h.subs.insert(next, s);
					h.free -= s.size;
					DLOG3("inserting object into memory in existing hole offset=%lu size=%lu, alignment=%u, free is %lu", (unsigned long)s.offset,
					      (unsigned long)s.size, (unsigned)s.alignment, (unsigned long)h.free);
					return { h.mem, s.offset, s.size, true, needs_flush(h.memoryTypeIndex) };
				}
			}
		}
	}
	// if we get here, we need to create another heap
	return add_object_new(device, tid, memoryTypeIndex, s, flags, tiling, dedicated, allocflags);
}

bool suballocator_private::fill_image_memreq(VkDevice device, VkImage image, VkMemoryRequirements2& req, VkDeviceSize size)
{
	if (!run) { req.memoryRequirements.size = size; req.memoryRequirements.alignment = 1; req.memoryRequirements.memoryTypeBits = 1; return false; }
	assert(run);
	req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	VkMemoryDedicatedRequirements dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, nullptr, VK_FALSE, VK_FALSE };
	if (use_dedicated_allocation())
	{
		VkImageMemoryRequirementsInfo2 info = {};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
		info.image = image;
		if (use_dedicated_allocation()) req.pNext = &dedicated;
		wrap_vkGetImageMemoryRequirements2(device, &info, &req);
	}
	else
	{
		wrap_vkGetImageMemoryRequirements(device, image, &req.memoryRequirements);
	}
	return dedicated.prefersDedicatedAllocation || dedicated.requiresDedicatedAllocation;
}

void suballocator_private::bind(heap& h, const suballocation& s)
{
	assert(h.mem != VK_NULL_HANDLE);
	assert(s.alignment != 0);
	if (s.type == VK_OBJECT_TYPE_IMAGE)
	{
		image_lookup.at(s.index) = lookup(&h, s.offset, s.size);
		DLOG3("adding image=%p|%u heap=%p size=%lu off=%lu mem=%p alignment=%lu", (void*)s.handle.image, s.index, &h, (unsigned long)s.size,
		      (unsigned long)s.offset, (void*)h.mem, (unsigned long)s.alignment);
	}
	else
	{
		buffer_lookup[s.index] = lookup(&h, s.offset, s.size);
		DLOG3("adding buffer=%p|%u heap=%p size=%lu off=%lu mem=%p alignment=%lu", (void*)s.handle.buffer, s.index, &h, (unsigned long)s.size,
		      (unsigned long)s.offset, (void*)h.mem, (unsigned long)s.alignment);
	}
	assert(s.offset + s.size <= h.total);
}

suballoc_location suballocator::add_image(uint16_t tid, VkDevice device, VkImage image, const trackedimage& image_data)
{
	VkMemoryRequirements2 req = {};
	VkMemoryPropertyFlags memory_flags = prune_memory_flags(image_data.memory_flags);
	const bool dedicated = priv->fill_image_memreq(device, image, req, image_data.size);
	const uint32_t memoryTypeIndex = priv->get_device_memory_type(req.memoryRequirements.memoryTypeBits, memory_flags);
	suballocation s;
	s.type = VK_OBJECT_TYPE_IMAGE;
	s.handle.image = image;
	s.size = std::max(req.memoryRequirements.size, image_data.size);
	s.offset = 0;
	s.index = image_data.index;
	s.alignment = req.memoryRequirements.alignment;
	auto r = priv->add_object(device, tid, memoryTypeIndex, s, memory_flags, image_data.tiling, dedicated, 0);
	assert(r.offset == s.offset);
	return r;
}

suballoc_location suballocator::add_trackedobject(uint16_t tid, VkDevice device, const memory_requirements& reqs, uint64_t native, const trackedobject& data)
{
	const VkMemoryPropertyFlags memory_flags = prune_memory_flags(data.memory_flags);
	const uint32_t memoryTypeIndex = priv->get_device_memory_type(reqs.requirements.memoryTypeBits, memory_flags);
	suballocation s;
	s.type = data.object_type;
	s.handle.native = native;
	s.size = reqs.requirements.size;
	s.offset = 0;
	s.index = data.index;
	s.alignment = reqs.requirements.alignment;
	const bool dedicated = reqs.dedicated.prefersDedicatedAllocation || reqs.dedicated.requiresDedicatedAllocation;
	auto r = priv->add_object(device, tid, memoryTypeIndex, s, memory_flags, data.tiling, dedicated, reqs.allocate_flags);
	assert(r.offset == s.offset);
	return r;
}

void suballocator::virtualswap_images(VkDevice device, const std::vector<VkImage>& images, VkMemoryPropertyFlags memory_flags)
{
	assert(priv->run);
	VkMemoryPropertyFlags flags = prune_memory_flags(memory_flags);
	VkMemoryRequirements2 req = {};
	const bool dedicated = priv->fill_image_memreq(device, images.at(0), req, 0);
	const uint32_t memoryTypeIndex = priv->get_device_memory_type(req.memoryRequirements.memoryTypeBits, flags);
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	VkDeviceSize image_size = aligned_size(req.memoryRequirements.size, req.memoryRequirements.alignment);
	info.memoryTypeIndex = memoryTypeIndex;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	if (dedicated)
	{
		for (unsigned i = 0; i < images.size(); i++)
		{
			info.allocationSize = VkDeviceSize(image_size);
			VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &mem);
			if (result != VK_SUCCESS) SUBALLOC_ABORT(priv, "Failed to allocate dedicated memory for virtual swapchain!");
			priv->virtualswapmemory.push_back(mem);
			wrap_vkBindImageMemory(device, images.at(i), mem, 0);
		}
	}
	else
	{
		info.allocationSize = VkDeviceSize(image_size * images.size());
		VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &mem);
		if (result != VK_SUCCESS) SUBALLOC_ABORT(priv, "Failed to allocate memory for virtual swapchain!");
		priv->virtualswapmemory.push_back(mem);
		uint32_t offset = 0;
		for (unsigned i = 0; i < images.size(); i++) { wrap_vkBindImageMemory(device, images.at(i), mem, offset); offset += image_size; }
	}
}

suballoc_location suballocator::add_buffer(uint16_t tid, VkDevice device, VkBuffer buffer, const trackedbuffer& buffer_data)
{
	VkMemoryPropertyFlags memory_flags = prune_memory_flags(buffer_data.memory_flags);
	const VkBufferUsageFlags buffer_flags = buffer_data.usage;
	VkMemoryRequirements2 req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, nullptr };
	VkMemoryDedicatedRequirements dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, nullptr };
	if (use_dedicated_allocation() && priv->run)
	{
		VkBufferMemoryRequirementsInfo2 info = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, nullptr };
		info.buffer = buffer;
		req.pNext = &dedicated;
		wrap_vkGetBufferMemoryRequirements2(device, &info, &req);
	}
	else if (priv->run)
	{
		wrap_vkGetBufferMemoryRequirements(device, buffer, &req.memoryRequirements);
	}
	else // fake mem system
	{
		req.memoryRequirements.size = buffer_data.size;
		req.memoryRequirements.alignment = 1;
		req.memoryRequirements.memoryTypeBits = 1;
	}
	uint32_t memoryTypeIndex = priv->get_device_memory_type(req.memoryRequirements.memoryTypeBits, memory_flags);
	suballocation s;
	s.type = VK_OBJECT_TYPE_BUFFER;
	s.handle.buffer = buffer;
	s.size = req.memoryRequirements.size;
	s.offset = 0;
	s.index = buffer_data.index;
	s.alignment = req.memoryRequirements.alignment;
	VkMemoryAllocateFlags allocflags = 0;
	if (buffer_flags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) { dedicated.prefersDedicatedAllocation = VK_TRUE; allocflags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR; }
	auto r = priv->add_object(device, tid, memoryTypeIndex, s, memory_flags, TILING_LINEAR, dedicated.prefersDedicatedAllocation, allocflags);
	assert(r.offset == s.offset);
	return r;
}

void suballocator::free_image(uint32_t image_index)
{
	if (image_index == CONTAINER_NULL_VALUE) return;
	DLOG3("deleting image=%u", image_index);
	lookup& l = priv->image_lookup.at(image_index);
	if (l.home) // it is possible to delete something that has not been bound yet
	{
		l.home->deletes.push_back(l.offset);
		l.home = nullptr;
	}
}

void suballocator::free_buffer(uint32_t buffer_index)
{
	if (buffer_index == CONTAINER_NULL_VALUE) return;
	DLOG3("deleting buffer=%u", buffer_index);
	lookup& l = priv->buffer_lookup.at(buffer_index);
	if (l.home) // it is possible to delete something that has not been bound yet
	{
		l.home->deletes.push_back(l.offset);
		l.home = nullptr;
	}
}

suballoc_location suballocator::find_image_memory(uint32_t image_index)
{
	lookup& l = priv->image_lookup.at(image_index);
	if (!l.home) SUBALLOC_ABORT(priv, "Image %u is missing its memory!", image_index);
	const bool needs_init = !l.initialized;
	l.initialized = true;
	return { l.home->mem, l.offset, l.size, needs_init, priv->needs_flush(l.home->memoryTypeIndex) };
}

suballoc_location suballocator::find_buffer_memory(uint32_t buffer_index)
{
	lookup& l = priv->buffer_lookup.at(buffer_index);
	if (!l.home) SUBALLOC_ABORT(priv, "Buffer %u is missing its memory!", buffer_index);
	const bool needs_init = !l.initialized;
	l.initialized = true;
	return { l.home->mem, l.offset, l.size, needs_init, priv->needs_flush(l.home->memoryTypeIndex) };
}

suballoc_location suballocator::find_tensor_memory(uint32_t tensor_index)
{
	lookup& l = priv->tensor_lookup.at(tensor_index);
	if (!l.home) SUBALLOC_ABORT(priv, "Tensor %u is missing its memory!", tensor_index);
	const bool needs_init = !l.initialized;
	l.initialized = true;
	return { l.home->mem, l.offset, l.size, needs_init, priv->needs_flush(l.home->memoryTypeIndex) };
}

void suballocator::destroy(VkDevice device)
{
	for (heap& h : priv->heaps)
	{
		if (priv->run) wrap_vkFreeMemory(device, h.mem, nullptr);
		else free(h.mem);
		h.deletes.clear();
	}
	priv->heaps.clear();
	priv->image_lookup.clear();
	priv->buffer_lookup.clear();
	for (auto v : priv->virtualswapmemory)
	{
		wrap_vkFreeMemory(device, v, nullptr);
	}
	priv->virtualswapmemory.clear();
}

// for debugging, print the contents of the suballocator
void suballocator_private::suballoc_print(FILE* fp)
{
	fprintf(fp, "SUBALLOCATOR CONTENTS\n");
	fprintf(fp, "Images:\n");
	int i = 0; for (const lookup& l : image_lookup)
	{
		if (l.home == nullptr && l.size == 0) continue;
		fprintf(fp, "\t%d: home=%p offset=%lu size=%lu\n", i++, l.home, (unsigned long)l.offset, (unsigned long)l.size);
	}
	fprintf(fp, "Buffers:\n");
	i = 0; for (const lookup& l : buffer_lookup)
	{
		if (l.home == nullptr && l.size == 0) continue;
		fprintf(fp, "\t%d: home=%p offset=%lu size=%lu\n", i++, l.home, (unsigned long)l.offset, (unsigned long)l.size);
	}
	fprintf(fp, "Heaps:\n");
	for (const heap& h : heaps)
	{
		fprintf(fp, "\t%p tid=%u type=%u mem=%lu free=%lu total=%lu subs=%u deletes=%u\n", &h, (unsigned)h.tid, (unsigned)h.memoryTypeIndex,
		        (unsigned long)h.mem, (unsigned long)h.free, (unsigned long)h.total, (unsigned)h.subs.size(), (unsigned)h.deletes.size());
	}
}

int suballocator::self_test()
{
	int retval = 0;

	priv->self_test();

	// walk the heaps to check consistency
	for (const heap& h : priv->heaps)
	{
		h.self_test();
		uint64_t freed = 0;
		if (h.subs.size() > 0) freed = h.subs.front().offset;
		uint64_t used = 0;
		int64_t prev_end = -1; // end of previous allocation
		for (auto it = h.subs.cbegin(); it != h.subs.cend(); ++it)
		{
			bool deleted = false;
			for (uint32_t deleted_offset : h.deletes)
			{
				if (deleted_offset == it->offset) deleted = true;
			}
			assert((int64_t)it->offset >= prev_end);
			if (prev_end >= 0) freed += it->offset - prev_end;
			used += it->size;
			prev_end = it->offset + it->size;
			assert(it->size > 0);
			assert(it->type == VK_OBJECT_TYPE_IMAGE || it->type == VK_OBJECT_TYPE_BUFFER);
			if (deleted) continue; // looking this up in the lookup table is not valid in this case
			// check that there isn't anything in the heaps that isn't also in the lookup tables
			if (it->type == VK_OBJECT_TYPE_IMAGE)
			{
				suballoc_location loc = find_image_memory(it->index);
				assert(loc.memory == h.mem);
				assert(loc.offset == it->offset);
				assert(loc.size == it->size);
				(void)loc;
			}
			else
			{
				suballoc_location loc = find_buffer_memory(it->index);
				assert(loc.memory == h.mem);
				assert(loc.offset == it->offset);
				assert(loc.size == it->size);
				(void)loc;
			}
			if (!deleted) retval++;
		}
		if (prev_end >= 0) freed += h.total - prev_end;
		else freed = h.total;
		assert(h.free == freed);
		assert(freed + used == h.total);
		(void)freed;
		(void)used;
	}

	return retval;
}

void suballocator_private::self_test()
{
	// check that there isn't anything in the lookup tables that isn't also in the heaps
	for (const lookup& l : image_lookup)
	{
		if (!l.home) continue;
		assert(l.size != 0);
		bool found = false;
		for (auto it = l.home->subs.cbegin(); it != l.home->subs.cend(); ++it)
		{
			if (it->offset == l.offset && it->size == l.size) found = true;
		}
		assert(found);
		(void)found;
	}
	for (const lookup& l : buffer_lookup)
	{
		if (!l.home) continue;
		assert(l.size != 0);
		bool found = false;
		for (auto it = l.home->subs.cbegin(); it != l.home->subs.cend(); ++it)
		{
			if (it->offset == l.offset && it->size == l.size) found = true;
		}
		assert(found);
		(void)found;
	}
}
