// Vulkan trace common code

#pragma once

#include <assert.h>
#include <atomic>
#include <thread>
#include <cstdint>
#include <list>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <stdio.h>
#include <functional>

#include "feature_detect.h"
#include "containers.h"
#include "lavamutex.h"
#include "filewriter.h"
#include "lavatube.h"
#include "write_resource_auto.h"
#include "jsoncpp/json/value.h"

/// A global to make static thread analysis easier. This MUST be the first variable constructed, or
/// we will have trouble on destruction. Also never assume this variable is safe during program
/// termination _except_ inside write.cpp
extern lava::mutex frame_mutex;

struct framedata
{
	/// start position in the uncompressed byte stream for this frame
	uint64_t start_pos;
	int global_frame;
	int local_frame;
};

struct trace_capabilities
{
	void reset()
	{
		delete stored_device_properties; stored_device_properties = nullptr;
		delete stored_VkPhysicalDeviceFeatures2; stored_VkPhysicalDeviceFeatures2 = nullptr;
		delete stored_VkPhysicalDeviceVulkan11Features; stored_VkPhysicalDeviceVulkan11Features = nullptr;
		delete stored_VkPhysicalDeviceVulkan12Features; stored_VkPhysicalDeviceVulkan12Features = nullptr;
		delete stored_VkPhysicalDeviceVulkan13Features; stored_VkPhysicalDeviceVulkan13Features = nullptr;
		delete stored_driver_properties; stored_driver_properties = nullptr;
		stored_api_version = 0;
		instance_extensions.clear();
		device_extensions.clear();
	}

	~trace_capabilities() { reset(); }

	VkPhysicalDeviceProperties* stored_device_properties = nullptr;
	VkPhysicalDeviceFeatures2* stored_VkPhysicalDeviceFeatures2 = nullptr;
	VkPhysicalDeviceVulkan11Features* stored_VkPhysicalDeviceVulkan11Features = nullptr;
	VkPhysicalDeviceVulkan12Features* stored_VkPhysicalDeviceVulkan12Features = nullptr;
	VkPhysicalDeviceVulkan13Features* stored_VkPhysicalDeviceVulkan13Features = nullptr;
	VkPhysicalDeviceDriverProperties* stored_driver_properties = nullptr;
	uint32_t stored_api_version = 0;

	std::unordered_set<std::string> instance_extensions;
	std::unordered_set<std::string> device_extensions;
};

struct trace_metadata
{
	/// What the device told the app it is capable of, modified by us
	trace_capabilities device GUARDED_BY(frame_mutex);
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory = {};

	/// What the app asked for, modified by us
	trace_capabilities app GUARDED_BY(frame_mutex);
};

// trace classes

struct debug_info
{
	std::atomic_uint32_t flushes_queue { 0 }; // total number of memory mapping flushes called from queue submits
	std::atomic_uint32_t flushes_event { 0 }; // total number of memory mapping flushes called from vkSetEvent
	std::atomic_uint32_t flushes_remap { 0 }; // of the above flush types, how many required changing an existing mmap?
	std::atomic_uint32_t flushes_persistent { 0 }; // of the above flush types, how many were persistent?
	std::atomic_uint32_t memory_devices { 0 }; // total number of memory devices scanned
	std::atomic_uint32_t memory_dumps { 0 }; // of the devices, how many times did we simply dump a range?
	std::atomic_uint32_t memory_scans { 0 }; // of the devices, how many times did we scan for chages?
	std::atomic_uint64_t memory_bytes { 0 }; // how many bytes we scanned for changes
	std::atomic_uint64_t memory_changed_bytes { 0 }; // how many bytes were actually changed
	std::atomic_uint32_t memory_scans_unchanged { 0 }; // of the scans, how many did actually change?

	debug_info() {}

	debug_info& operator=(const debug_info& rhs)
	{
		flushes_queue.store(rhs.flushes_queue.load(std::memory_order_relaxed));
		flushes_event.store(rhs.flushes_event.load(std::memory_order_relaxed));
		flushes_remap.store(rhs.flushes_remap.load(std::memory_order_relaxed));
		flushes_persistent.store(rhs.flushes_persistent.load(std::memory_order_relaxed));
		memory_devices.store(rhs.memory_devices.load(std::memory_order_relaxed));
		memory_dumps.store(rhs.memory_dumps.load(std::memory_order_relaxed));
		memory_scans.store(rhs.memory_scans.load(std::memory_order_relaxed));
		memory_bytes.store(rhs.memory_bytes.load(std::memory_order_relaxed));
		memory_changed_bytes.store(rhs.memory_changed_bytes.load(std::memory_order_relaxed));
		memory_scans_unchanged.store(rhs.memory_scans_unchanged.load(std::memory_order_relaxed));
		return *this;
	}

	debug_info(const debug_info& rhs) { operator=(rhs); }

	debug_info& operator+=(const debug_info& rhs)
	{
		flushes_queue += rhs.flushes_queue.load(std::memory_order_relaxed);
		flushes_event += rhs.flushes_event.load(std::memory_order_relaxed);
		flushes_remap += rhs.flushes_remap.load(std::memory_order_relaxed);
		flushes_persistent += rhs.flushes_persistent.load(std::memory_order_relaxed);
		memory_devices += rhs.memory_devices.load(std::memory_order_relaxed);
		memory_dumps += rhs.memory_dumps.load(std::memory_order_relaxed);
		memory_scans += rhs.memory_scans.load(std::memory_order_relaxed);
		memory_bytes += rhs.memory_bytes.load(std::memory_order_relaxed);
		memory_changed_bytes += rhs.memory_changed_bytes.load(std::memory_order_relaxed);
		memory_scans_unchanged += rhs.memory_scans_unchanged.load(std::memory_order_relaxed);
		return *this;
	}
};

class lava_writer;

/// Per-thread controller
class lava_file_writer : public file_writer
{
	lava_file_writer(const lava_file_writer&) = delete;
	lava_file_writer& operator=(const lava_file_writer&)= delete;

public:
	lava_file_writer(uint16_t _tid, lava_writer* _parent);
	~lava_file_writer();

	debug_info new_frame(int global_frame) REQUIRES(frame_mutex);
	void set(const std::string& path) REQUIRES(frame_mutex);
	inline int thread_index() const { return mTid; }

	debug_info debug;
	int prev_callno = -1; // for validation

	memory_pool pool;
	int local_call_number = 0;
	lava_writer* parent;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

	inline void write_api_command(uint16_t id);

	inline void write_handle(const trackable* t)
	{
		if (t)
		{
			write_uint32_t(t->index);
			write_int8_t(t->tid);
			write_uint16_t(t->call);
			DLOG3("%d : wrote handle idx=%u tid=%d call=%u", mTid, (unsigned)t->index, (int)t->tid, (unsigned)t->call);
		}
		else
		{
			write_uint32_t(CONTAINER_NULL_VALUE);
			write_int8_t((int8_t)-1);
			write_uint16_t(0);
			DLOG3("%d : wrote a null handle", mTid);
		}
	}

	void inject_thread_barrier(bool do_lock = true);

	/// Make other threads wait for us
	void push_thread_barriers();

	/// Frame count as seen by this thread alone
	int local_frame = 0;

	std::atomic_bool pending_barrier { false };

private:
	std::string mPath;
	std::vector<framedata> frames;
	char thread_name[16];
};

/// Top level singleton instance for the tracer.
class lava_writer
{
public:
	lava_writer();
	~lava_writer();

	static lava_writer& instance();
	void set(const std::string& path, int as_version = LAVATUBE_VERSION_MAJOR);
	Json::Value& json() REQUIRES(frame_mutex) { return mJson; }
	lava_file_writer& file_writer();
	void serialize();
	void finish();

	int version_major() const { return LAVATUBE_VERSION_MAJOR; }
	int version_minor() const { return LAVATUBE_VERSION_MINOR; }
	int version_patch() const { return LAVATUBE_VERSION_PATCH; }

	void new_frame();

	std::atomic_int global_frame;
	std::atomic_int mCallNo;
	trace_records records;

	trace_metadata meta GUARDED_BY(frame_mutex);

	// statistics
	uint64_t mem_allocated = 0;
	uint64_t mem_wasted = 0;

	/// Actually used features. We use this to modify the meta_app.
	feature_detection usage_detection; // reentrant safe

	char fakeUUID[VK_UUID_SIZE + 1];

	trace_data<lava_file_writer*> thread_streams;

	/// We cannot allow the app to map or unmap memory while we are scanning it
	lava::mutex memory_mutex;

private:
	int mAsVersion = -1;
	std::string mPath;
	std::string mPack;
	VkuVulkanLibrary library = nullptr;
	Json::Value mJson GUARDED_BY(frame_mutex);
	std::vector<debug_info> debug GUARDED_BY(frame_mutex);
	bool should_serialize = false;
};

inline void lava_file_writer::write_api_command(uint16_t id)
{
	freeze();
	commandBuffer = VK_NULL_HANDLE;
	if (pending_barrier.load(std::memory_order_relaxed))
	{
		inject_thread_barrier();
		pending_barrier.store(false, std::memory_order_relaxed);
	}
	write_uint8_t(PACKET_API_CALL); // API call
	write_uint16_t(id); // API call name by id
	write_uint32_t(0); // reserved for future use
	local_call_number++;
}
