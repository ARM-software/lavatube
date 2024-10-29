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

static void suballoc_print(FILE* fp);

#define SUBALLOC_ABORT(_format, ...) do { suballoc_print(p__debug_destination); fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); fflush(p__debug_destination); abort(); } while(0)

static uint64_t min_heap_size = 1024 * 1024 * 32; // default 32mb size heaps
static std::vector<VkDeviceMemory> virtualswapmemory;

struct suballocation
{
	VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
	union
	{
		VkImage image;
		VkBuffer buffer;
	} handle;
	VkDeviceSize size = 0;
	VkDeviceSize offset = 0;
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
	VkImageTiling tiling = VK_IMAGE_TILING_LINEAR; // buffers are assumed to be linear
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


static tbb::concurrent_vector<heap> heaps;
static VkPhysicalDeviceMemoryProperties memory_properties;
static std::vector<lookup> image_lookup;
static std::vector<lookup> buffer_lookup;

using bind_object_memory = std::function<void(heap& h, VkDeviceSize offset, VkDeviceSize size)>;

// helpers

static inline bool needs_flush(unsigned memoryTypeIndex)
{
	return !(memory_properties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

static void print_memory_usage()
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

static uint32_t get_device_memory_type(uint32_t type_filter, VkMemoryPropertyFlags& properties)
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
	SUBALLOC_ABORT("Failed to find required memory type (filter=%u, props=%u)", type_filter, (unsigned)properties);
	return 0xffff; // satisfy compiler
}

// API

void suballoc_init(int num_images, int num_buffers, int heap_size)
{
	assert(image_lookup.size() == 0);
	assert(buffer_lookup.size() == 0);
	image_lookup.resize(num_images);
	buffer_lookup.resize(num_buffers);
	memset(&memory_properties, 0, sizeof(memory_properties));
	if (heap_size != -1) min_heap_size = heap_size;
}

void suballoc_setup(VkPhysicalDevice physicaldevice)
{
	assert(memory_properties.memoryTypeCount == 0);
	wrap_vkGetPhysicalDeviceMemoryProperties(physicaldevice, &memory_properties);
	assert(memory_properties.memoryTypeCount > 0);
}

static suballoc_location add_object_new(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
        VkDeviceSize alignment, VkImageTiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags, bind_object_memory bind_callback)
{
	heap h;
	h.tid = tid;
	VkMemoryDedicatedAllocateInfoKHR dedinfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr };
	VkMemoryAllocateFlagsInfo flaginfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, allocflags, 0 };
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flaginfo };
	if (dedicated)
	{
		if (s.type == VK_OBJECT_TYPE_BUFFER)
		{
			dedinfo.buffer = s.handle.buffer;
		}
		else
		{
			dedinfo.image = s.handle.image;
		}
		flaginfo.pNext = &dedinfo;
		info.allocationSize = s.size;
	}
	else
	{
		info.allocationSize = std::max<VkDeviceSize>(min_heap_size, s.size);
	}
	assert(info.allocationSize < 1024 * 1024 * 1024); // 1 gig max for sanity's sake
	info.memoryTypeIndex = memoryTypeIndex;
	VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &h.mem);
	if (result != VK_SUCCESS)
	{
		print_memory_usage();
		SUBALLOC_ABORT("Failed to allocate %lu bytes of memory for memory type %u and tiling %u", (unsigned long)info.allocationSize, (unsigned)memoryTypeIndex, (unsigned)tiling);
	}
	(void)result; // TBD - handle failure somehow
	h.free = info.allocationSize - s.size;
	h.total = info.allocationSize;
	h.memoryTypeIndex = memoryTypeIndex;
	h.tiling = tiling;
	h.subs.push_back(s);
	DLOG2("allocating new memory pool with size = %lu, free = %lu (memoryTypeIndex=%u, tiling=%u)", (unsigned long)info.allocationSize,
	      (unsigned long)h.free, (unsigned)memoryTypeIndex, (unsigned)tiling);
	heaps.push_back(h);
	bind_callback(heaps.back(), 0, s.size); // call to vkBind{Buffer|Image}Memory
	return { h.mem, 0, s.size, true, needs_flush(memoryTypeIndex) };
}

static suballoc_location add_object(VkDevice device, uint16_t tid, uint32_t memoryTypeIndex, suballocation &s, VkMemoryPropertyFlags flags,
	VkDeviceSize alignment, VkImageTiling tiling, bool dedicated, VkMemoryAllocateFlags allocflags, bind_object_memory bind_callback)
{
	if (dedicated)
	{
		return add_object_new(device, tid, memoryTypeIndex, s, flags, alignment, tiling, dedicated, allocflags, bind_callback);
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
		if (h.tid == tid && (flags & f) == flags && h.free >= s.size && h.memoryTypeIndex == memoryTypeIndex && h.tiling == tiling)
		{
			// First case: nothing allocated in heap. In this case, we do not care about alignment, because according to the spec:
			// "Allocations returned by vkAllocateMemory are guaranteed to meet any alignment requirement of the implementation."
			// Also second case: some memory is available before the first allocation. Like above, we are guaranteed not to have
			// to care about alignment in this case.
			if (h.subs.empty() || (h.subs.front().offset >= s.size))
			{
				bind_callback(h, 0, s.size); // call to vkBind{Buffer|Image}Memory
				h.subs.push_front(s);
				h.free -= s.size;
				DLOG3("inserting object into memory at the front size=%lu, alignment=%u, free is %lu", (unsigned long)s.size, (unsigned)alignment, (unsigned long)h.free);
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
					if (std::align(alignment, s.size, start, left))
					{
						h.free -= s.size;
						DLOG3("inserting object into memory at the end size=%lu, alignment=%u, free is %lu", (unsigned long)s.size, (unsigned)alignment, (unsigned long)h.free);
						s.offset = (VkDeviceSize)start;
						bind_callback(h, s.offset, s.size); // call to vkBind{Buffer|Image}Memory
						h.subs.push_back(s);
						return { h.mem, s.offset, s.size, true, needs_flush(h.memoryTypeIndex) };
					}
					break; // no space found
				}
				std::size_t hole_size = next->offset - (it->offset + it->size);
				if (std::align(alignment, s.size, start, hole_size))
				{
					s.offset = (VkDeviceSize)start;
					bind_callback(h, s.offset, s.size); // call to vkBind{Buffer|Image}Memory
					h.subs.insert(next, s);
					h.free -= s.size;
					DLOG3("inserting object into memory in existing hole offset=%lu size=%lu, alignment=%u, free is %lu", (unsigned long)s.offset,
					      (unsigned long)s.size, (unsigned)alignment, (unsigned long)h.free);
					return { h.mem, s.offset, s.size, true, needs_flush(h.memoryTypeIndex) };
				}
			}
		}
	}
	// if we get here, we need to create another heap
	return add_object_new(device, tid, memoryTypeIndex, s, flags, alignment, tiling, dedicated, allocflags, bind_callback);
}

static bool fill_image_memreq(VkDevice device, VkImage image, VkMemoryRequirements2& req)
{
	req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	VkMemoryDedicatedRequirements dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, nullptr, VK_TRUE, VK_FALSE };
	if (use_dedicated_allocation())
	{
		VkImageMemoryRequirementsInfo2 info = {};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
		info.image = image;
		req.pNext = &dedicated;
		wrap_vkGetImageMemoryRequirements2(device, &info, &req);
	}
	else
	{
		wrap_vkGetImageMemoryRequirements(device, image, &req.memoryRequirements);
	}
	return dedicated.prefersDedicatedAllocation;
}

suballoc_location suballoc_add_image(uint16_t tid, VkDevice device, VkImage image, uint32_t image_index, VkMemoryPropertyFlags flags, VkImageTiling tiling, VkDeviceSize min_size)
{
	if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) || (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
	{
		flags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; // do not require this bit in these cases
	}
	VkMemoryRequirements2 req = {};
	const bool dedicated = fill_image_memreq(device, image, req);
	const uint32_t memoryTypeIndex = get_device_memory_type(req.memoryRequirements.memoryTypeBits, flags);
	suballocation s;
	s.type = VK_OBJECT_TYPE_IMAGE;
	s.handle.image = image;
	s.size = std::max(req.memoryRequirements.size, min_size);
	s.offset = 0;
	s.index = image_index;
	auto r = add_object(device, tid, memoryTypeIndex, s, flags, req.memoryRequirements.alignment, tiling, dedicated, 0,
		[=](heap& h, VkDeviceSize offset, VkDeviceSize size)
		{
			assert(h.mem != VK_NULL_HANDLE);
			image_lookup.at(image_index) = lookup(&h, offset, size);
			DLOG3("adding image=%p|%u heap=%p size=%lu off=%lu mem=%p alignment=%lu", (void*)image, image_index, &h, (unsigned long)size,
			      (unsigned long)offset, (void*)h.mem, (unsigned long)req.memoryRequirements.alignment);
			assert(offset + size <= h.total);
		});
	assert(r.offset == s.offset);
	return r;
}

void suballoc_virtualswap_images(VkDevice device, const std::vector<VkImage>& images)
{
	VkMemoryRequirements2 req = {};
	const bool dedicated = fill_image_memreq(device, images.at(0), req);
	VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	const uint32_t memoryTypeIndex = get_device_memory_type(req.memoryRequirements.memoryTypeBits, flags);
	VkMemoryAllocateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	VkDeviceSize image_size = aligned_size(req.memoryRequirements.size, req.memoryRequirements.alignment);
	info.memoryTypeIndex = memoryTypeIndex;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	if (dedicated)
	{
		for (unsigned i = 0; i < images.size(); i++)
		{
			info.allocationSize = VkDeviceSize(image_size);
			VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &mem);
			if (result != VK_SUCCESS) SUBALLOC_ABORT("Failed to allocate dedicated memory for virtual swapchain!");
			virtualswapmemory.push_back(mem);
			wrap_vkBindImageMemory(device, images.at(i), mem, 0);
		}
	}
	else
	{
		info.allocationSize = VkDeviceSize(image_size * images.size());
		VkResult result = wrap_vkAllocateMemory(device, &info, nullptr, &mem);
		if (result != VK_SUCCESS) SUBALLOC_ABORT("Failed to allocate memory for virtual swapchain!");
		virtualswapmemory.push_back(mem);
		uint32_t offset = 0;
		for (unsigned i = 0; i < images.size(); i++) { wrap_vkBindImageMemory(device, images.at(i), mem, offset); offset += image_size; }
	}
}

suballoc_location suballoc_add_buffer(uint16_t tid, VkDevice device, VkBuffer buffer, uint32_t buffer_index, VkMemoryPropertyFlags mempropflags, const trackedbuffer& buffer_data)
{
	const VkBufferUsageFlags buffer_flags = buffer_data.usage;
	if ((mempropflags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) || (mempropflags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
	{
		mempropflags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; // do not require this bit in these cases
	}
	VkMemoryRequirements2 req = {};
	req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	VkMemoryDedicatedRequirements dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
	if (use_dedicated_allocation())
	{
		VkBufferMemoryRequirementsInfo2 info = {};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
		info.buffer = buffer;
		req.pNext = &dedicated;
		wrap_vkGetBufferMemoryRequirements2(device, &info, &req);
	}
	else
	{
		wrap_vkGetBufferMemoryRequirements(device, buffer, &req.memoryRequirements);
	}
	uint32_t memoryTypeIndex = get_device_memory_type(req.memoryRequirements.memoryTypeBits, mempropflags);
	suballocation s;
	s.type = VK_OBJECT_TYPE_BUFFER;
	s.handle.buffer = buffer;
	s.size = req.memoryRequirements.size;
	s.offset = 0;
	s.index = buffer_index;
	VkMemoryAllocateFlags allocflags = 0;
	if (buffer_flags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) { dedicated.prefersDedicatedAllocation = VK_TRUE; allocflags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR; }
	auto r = add_object(device, tid, memoryTypeIndex, s, mempropflags, req.memoryRequirements.alignment, VK_IMAGE_TILING_LINEAR, dedicated.prefersDedicatedAllocation, allocflags,
		[=](heap& h, VkDeviceSize offset, VkDeviceSize size)
		{
			assert(h.mem != VK_NULL_HANDLE);
			buffer_lookup[buffer_index] = lookup(&h, offset, size);
			DLOG3("adding buffer=%p|%u heap=%p size=%lu off=%lu mem=%p alignment=%lu", (void*)buffer, buffer_index, &h, (unsigned long)s.size,
			      (unsigned long)s.offset, (void*)h.mem, (unsigned long)req.memoryRequirements.alignment);
			assert(offset + size <= h.total);
		});
	assert(r.offset == s.offset);
	return r;
}

void suballoc_del_image(uint32_t image_index)
{
	if (image_index == CONTAINER_NULL_VALUE) return;
	DLOG3("deleting image=%u", image_index);
	lookup& l = image_lookup.at(image_index);
	if (l.home) // it is possible to delete something that has not been bound yet
	{
		l.home->deletes.push_back(l.offset);
		l.home = nullptr;
	}
}

void suballoc_del_buffer(uint32_t buffer_index)
{
	if (buffer_index == CONTAINER_NULL_VALUE) return;
	DLOG3("deleting buffer=%u", buffer_index);
	lookup& l = buffer_lookup.at(buffer_index);
	if (l.home) // it is possible to delete something that has not been bound yet
	{
		l.home->deletes.push_back(l.offset);
		l.home = nullptr;
	}
}

suballoc_location suballoc_find_image_memory(uint32_t image_index)
{
	lookup& l = image_lookup.at(image_index);
	if (!l.home) SUBALLOC_ABORT("Image %u is missing its memory!", image_index);
	const bool needs_init = !l.initialized;
	l.initialized = true;
	return { l.home->mem, l.offset, l.size, needs_init, needs_flush(l.home->memoryTypeIndex) };
}

suballoc_location suballoc_find_buffer_memory(uint32_t buffer_index)
{
	lookup& l = buffer_lookup.at(buffer_index);
	if (!l.home) SUBALLOC_ABORT("Buffer %u is missing its memory!", buffer_index);
	const bool needs_init = !l.initialized;
	l.initialized = true;
	return { l.home->mem, l.offset, l.size, needs_init, needs_flush(l.home->memoryTypeIndex) };
}

void suballoc_destroy(VkDevice device)
{
	for (heap& h : heaps)
	{
		wrap_vkFreeMemory(device, h.mem, nullptr);
		h.deletes.clear();
	}
	heaps.clear();
	image_lookup.clear();
	buffer_lookup.clear();
	for (auto v : virtualswapmemory)
	{
		wrap_vkFreeMemory(device, v, nullptr);
	}
	virtualswapmemory.clear();
}

// for debugging, print the contents of the suballocator
static void suballoc_print(FILE* fp)
{
	fprintf(fp, "SUBALLOCATOR CONTENTS\n");
	fprintf(fp, "Images:\n");
	int i = 0; for (const lookup& l : image_lookup) fprintf(fp, "\t%d: home=%p offset=%lu size=%lu\n", i++, l.home, (unsigned long)l.offset, (unsigned long)l.size);
	fprintf(fp, "Buffers:\n");
	i = 0; for (const lookup& l : buffer_lookup) fprintf(fp, "\t%d: home=%p offset=%lu size=%lu\n", i++, l.home, (unsigned long)l.offset, (unsigned long)l.size);
	fprintf(fp, "Heaps:\n");
	for (const heap& h : heaps)
	{
		fprintf(fp, "\t%p tid=%u type=%u mem=%lu free=%lu total=%lu subs=%u deletes=%u\n", &h, (unsigned)h.tid, (unsigned)h.memoryTypeIndex,
		        (unsigned long)h.mem, (unsigned long)h.free, (unsigned long)h.total, (unsigned)h.subs.size(), (unsigned)h.deletes.size());
	}
}

int suballoc_internal_test()
{
	int retval = 0;
#ifndef NDEBUG
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
	}
	// walk the heaps to check consistency
	for (const heap& h : heaps)
	{
		uint64_t freed = 0;
		if (h.subs.size() > 0) freed = h.subs.front().offset;
		uint64_t used = 0;
		assert(h.free <= h.total);
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
				suballoc_location loc = suballoc_find_image_memory(it->index);
				assert(loc.memory == h.mem);
				assert(loc.offset == it->offset);
				assert(loc.size == it->size);
			}
			else
			{
				suballoc_location loc = suballoc_find_buffer_memory(it->index);
				assert(loc.memory == h.mem);
				assert(loc.offset == it->offset);
				assert(loc.size == it->size);
			}
			if (!deleted) retval++;
		}
		if (prev_end >= 0) freed += h.total - prev_end;
		else freed = h.total;
		assert(h.free == freed);
		assert(freed + used == h.total);
	}
#endif
	return retval;
}
