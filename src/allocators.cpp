#include <cstdint>
#include <atomic>
#include <string.h>

#include "allocators.h"

static uint64_t allocator_internal_command_calls = 0;
static uint64_t allocator_internal_command_bytes = 0;
static uint64_t allocator_internal_object_calls = 0;
static uint64_t allocator_internal_object_bytes = 0;
static uint64_t allocator_internal_cache_calls = 0;
static uint64_t allocator_internal_cache_bytes = 0;
static uint64_t allocator_internal_device_calls = 0;
static uint64_t allocator_internal_device_bytes = 0;
static uint64_t allocator_internal_instance_calls = 0;
static uint64_t allocator_internal_instance_bytes = 0;
static uint64_t allocator_alloc_bytes = 0;
static uint64_t allocator_alloc_calls = 0;
static uint64_t allocator_alloc_avg_alignment = 0;
static uint64_t allocator_realloc_bytes = 0;
static uint64_t allocator_realloc_calls = 0;
static uint64_t allocator_realloc_avg_alignment = 0;

void allocators_print(FILE* fp)
{
	fprintf(fp, "Custom allocator info:\n");
	fprintf(fp, "internal command calls         : %18llu\n", (unsigned long long)allocator_internal_command_calls);
	fprintf(fp, "internal command bytes         : %18llu\n", (unsigned long long)allocator_internal_command_bytes);
	fprintf(fp, "internal object calls          : %18llu\n", (unsigned long long)allocator_internal_object_calls);
	fprintf(fp, "internal object bytes          : %18llu\n", (unsigned long long)allocator_internal_object_bytes);
	fprintf(fp, "internal cache calls           : %18llu\n", (unsigned long long)allocator_internal_cache_calls);
	fprintf(fp, "internal cache bytes           : %18llu\n", (unsigned long long)allocator_internal_cache_bytes);
	fprintf(fp, "internal device calls          : %18llu\n", (unsigned long long)allocator_internal_device_calls);
	fprintf(fp, "internal device bytes          : %18llu\n", (unsigned long long)allocator_internal_device_bytes);
	fprintf(fp, "internal instance calls        : %18llu\n", (unsigned long long)allocator_internal_instance_calls);
	fprintf(fp, "internal instance bytes        : %18llu\n", (unsigned long long)allocator_internal_instance_bytes);
	fprintf(fp, "external alloc bytes calls     : %18llu\n", (unsigned long long)allocator_alloc_bytes);
	fprintf(fp, "external alloc calls bytes     : %18llu\n", (unsigned long long)allocator_alloc_calls);
	fprintf(fp, "external alloc avg alignment   : %18llu\n", (unsigned long long)allocator_alloc_avg_alignment / std::max<uint64_t>(allocator_alloc_calls, 1));
	fprintf(fp, "external realloc bytes calls   : %18llu\n", (unsigned long long)allocator_realloc_bytes);
	fprintf(fp, "external realloc calls bytes   : %18llu\n", (unsigned long long)allocator_realloc_calls);
	fprintf(fp, "external realloc avg alignment : %18llu\n", (unsigned long long)allocator_realloc_avg_alignment / std::max<uint64_t>(allocator_realloc_calls, 1));
}

Json::Value allocators_json()
{
	Json::Value v;
	if (use_custom_allocator() > 0)
	{
		v["internal_command_calls"] = (Json::Value::UInt64)allocator_internal_command_calls;
		v["internal_command_bytes"] = (Json::Value::UInt64)allocator_internal_command_bytes;
		v["internal_object_calls"] = (Json::Value::UInt64)allocator_internal_object_calls;
		v["internal_object_bytes"] = (Json::Value::UInt64)allocator_internal_object_bytes;
		v["internal_cache_calls"] = (Json::Value::UInt64)allocator_internal_cache_calls;
		v["internal_cache_bytes"] = (Json::Value::UInt64)allocator_internal_cache_bytes;
		v["internal_device_calls"] = (Json::Value::UInt64)allocator_internal_device_calls;
		v["internal_device_bytes"] = (Json::Value::UInt64)allocator_internal_device_bytes;
		v["internal_instance_calls"] = (Json::Value::UInt64)allocator_internal_instance_calls;
		v["internal_instance_bytes"] = (Json::Value::UInt64)allocator_internal_instance_bytes;
		v["external_alloc_bytes_calls"] = (Json::Value::UInt64)allocator_alloc_bytes;
		v["external_alloc_calls_bytes"] = (Json::Value::UInt64)allocator_alloc_calls;
		v["external_alloc_avg_alignment"] = (Json::Value::UInt64)allocator_alloc_avg_alignment / allocator_alloc_calls;
		v["external_realloc_bytes_calls"] = (Json::Value::UInt64)allocator_realloc_bytes;
		v["external_realloc_calls_bytes"] = (Json::Value::UInt64)allocator_realloc_calls;
		v["external_realloc_avg_alignment"] = (Json::Value::UInt64)allocator_realloc_avg_alignment / allocator_realloc_calls;
	}
	return v;
}

static void* debug_alloc(void* pUserData, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
	__atomic_add_fetch(&allocator_alloc_bytes, size, __ATOMIC_RELAXED);
	__atomic_add_fetch(&allocator_alloc_calls, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&allocator_alloc_avg_alignment, alignment, __ATOMIC_RELAXED);
	if (alignment > 8)
	{
		void* ptr;
		int retval = posix_memalign(&ptr, alignment, size);
		if (retval != 0) ABORT("Failed to allocate %lu bytes of memory aligned at %u bytes: %s\n", (unsigned long)size, (unsigned)alignment, strerror(retval));
		return ptr;
	}
	return malloc(size);
}

static void debug_free(void* pUserData, void* pMemory)
{
	free(pMemory);
}

static void* debug_realloc(void* pUserData, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
	__atomic_add_fetch(&allocator_realloc_bytes, size, __ATOMIC_RELAXED);
	__atomic_add_fetch(&allocator_realloc_calls, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&allocator_realloc_avg_alignment, alignment, __ATOMIC_RELAXED);
	void* ptr;
	int retval = posix_memalign(&ptr, alignment, size);
	if (retval == 0)
	{
		free(pOriginal);
		return ptr;
	}
	else
	{
		assert(false); // oops!
		return nullptr;
	}
}

static void debug_internal_alloc(void* pUserData, size_t size, VkInternalAllocationType allocationType, VkSystemAllocationScope allocationScope)
{
	switch ((int)allocationType)
	{
	case VK_SYSTEM_ALLOCATION_SCOPE_COMMAND:
		__atomic_add_fetch(&allocator_internal_command_calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&allocator_internal_command_bytes, size, __ATOMIC_RELAXED);
		break;
	case VK_SYSTEM_ALLOCATION_SCOPE_OBJECT:
		__atomic_add_fetch(&allocator_internal_object_calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&allocator_internal_object_bytes, size, __ATOMIC_RELAXED);
		break;
	case VK_SYSTEM_ALLOCATION_SCOPE_CACHE:
		__atomic_add_fetch(&allocator_internal_cache_calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&allocator_internal_cache_bytes, size, __ATOMIC_RELAXED);
		break;
	case VK_SYSTEM_ALLOCATION_SCOPE_DEVICE:
		__atomic_add_fetch(&allocator_internal_device_calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&allocator_internal_device_bytes, size, __ATOMIC_RELAXED);
		break;
	case VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE:
		__atomic_add_fetch(&allocator_internal_instance_calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&allocator_internal_instance_bytes, size, __ATOMIC_RELAXED);
		break;
	default:
		assert(false);
		break;
	}
}

static void debug_internal_free(void* pUserData, size_t size, VkInternalAllocationType allocationType, VkSystemAllocationScope allocationScope)
{
}

void allocators_set(VkAllocationCallbacks*& callbacks)
{
#ifndef __ANDROID__
	if (use_custom_allocator() == 1) // debug
	{
		callbacks->pUserData = nullptr;
		callbacks->pfnAllocation = debug_alloc;
		callbacks->pfnReallocation = debug_realloc;
		callbacks->pfnFree = debug_free;
		callbacks->pfnInternalAllocation = debug_internal_alloc;
		callbacks->pfnInternalFree = debug_internal_free;
		return;
	}
#endif
	callbacks = nullptr;
}
