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

#include "external/tracetooltests/src/usagetracker/vulkan_feature_detect.h"
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
	uint32_t packet_index;
	int global_frame;
	int local_frame;
};

union result_value
{
	VkResult result;
	VkDeviceAddress device_address;
	VkDeviceSize device_size;
	uint32_t uint_32;
	uint64_t uint_64;
	PFN_vkVoidFunction function;
};

struct surface_create_packet
{
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t stored_sType = 0;
	VkBaseOutStructure* pNext = nullptr;
	uint32_t flags = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
	int32_t border = 0;
	int32_t depth = 0;
	VkResult retval = VK_SUCCESS;
	uint32_t surface_index = CONTAINER_NULL_VALUE;
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
		delete stored_VkPhysicalDeviceVulkan14Features; stored_VkPhysicalDeviceVulkan14Features = nullptr;
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
	VkPhysicalDeviceVulkan14Features* stored_VkPhysicalDeviceVulkan14Features = nullptr;
	VkPhysicalDeviceDriverProperties* stored_driver_properties = nullptr;
	uint32_t stored_api_version = 0;

	std::unordered_set<std::string> instance_extensions;
	std::unordered_set<std::string> device_extensions;
};

struct trace_metadata
{
	/// Capture policy: extension names hidden from the application.
	std::unordered_set<std::string> blacklisted_extensions GUARDED_BY(frame_mutex);

	/// What the device told the app it is capable of, modified by us
	trace_capabilities device GUARDED_BY(frame_mutex);
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory = {};

	/// What the app asked for, modified by us
	trace_capabilities app GUARDED_BY(frame_mutex);
};

class lava_writer;

/// Per-thread controller
class lava_file_writer : public file_writer
{
	lava_file_writer(const lava_file_writer&) = delete;
	lava_file_writer& operator=(const lava_file_writer&)= delete;

public:
	lava_file_writer(uint16_t _tid, lava_writer* _parent, bool _thread_barriers_active = true);
	~lava_file_writer();

	void new_frame(int global_frame) REQUIRES(frame_mutex);
	void set(const std::string& path) REQUIRES(frame_mutex);
	void capture_thread_name() { get_thread_name(thread_name); }
	inline int thread_index() const { return current.thread; }

	int prev_callno = -1; // for validation
	bool run = true;
	bool write_output = false;
	result_value use_result; // for post-processing to set which result to store

	memory_pool pool;
	lava_writer* parent;
	change_source current;

	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFormat host_copy_format = VK_FORMAT_UNDEFINED;

	inline void write_api_command(uint16_t id);
	void begin_packet(uint8_t type);
	void end_packet();
	void write_raw_packet(const char* data, uint32_t size);

	inline void write_VkAccelerationStructureNV(VkAccelerationStructureNV val) {} // TBD

	inline void write_handle(const trackable* t)
	{
		if (t)
		{
			assert(!t->is_state(trackable::states::uninitialized) && !t->is_state(trackable::states::destroyed));
			write_uint32_t(t->index);
			write_int8_t(t->last_modified.thread);
			write_uint32_t(t->last_modified.packet);
			DLOG3("%u : wrote handle idx=%u tid=%d packet=%u", current.thread, (unsigned)t->index, (int)t->last_modified.thread, (unsigned)t->last_modified.packet);
		}
		else
		{
			write_uint32_t(CONTAINER_NULL_VALUE);
			write_int8_t((int8_t)-1);
			write_uint32_t(0);
			DLOG3("%u : wrote a null handle", current.thread);
		}
	}

	void inject_thread_barrier() REQUIRES(frame_mutex);
	void write_thread_barrier(const std::vector<uint32_t>& packet_indices);

	/// Make other threads wait for us
	void push_thread_barriers();
	void activate_thread_barriers() { thread_barriers_active.store(true, std::memory_order_release); }

	std::atomic_bool pending_barrier { false };
	/// Pre-created post-processing writers stay inactive until their input thread emits output.
	std::atomic_bool thread_barriers_active { true };

	void self_test()
	{
		assert(parent != nullptr);
		file_writer::self_test();
	}

private:
	std::string mPath;
	std::vector<framedata> frames;
	char thread_name[16];
	uint64_t packet_start = 0;
	uint32_t* packet_size = nullptr;
	bool packet_open = false;
};

/// Top level singleton instance for the tracer.
class lava_writer
{
public:
	lava_writer();
	~lava_writer();

	static lava_writer& instance();
	void set(const std::string& path);
	void set_output(const std::string& packed_path);
	void bind_thread(unsigned index);
	void prepare_threads(unsigned count);
	Json::Value& json() REQUIRES(frame_mutex) { return mJson; }
	Json::Value& input_tracking() REQUIRES(frame_mutex) { return mInputTracking; }
	lava_file_writer& file_writer();
	lava_file_writer& file_writer(unsigned index); // not thread safe!
	void serialize();
	void finish();
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	void start_android_finish_monitor();
#endif

	int version_major() const { return LAVATUBE_VERSION_MAJOR; }
	int version_minor() const { return LAVATUBE_VERSION_MINOR; }
	int version_patch() const { return LAVATUBE_VERSION_PATCH; }

	void new_frame();

	std::atomic_int global_frame;
	trace_records records;
	bool run = true;
	bool write_output = false;

	trace_metadata meta GUARDED_BY(frame_mutex);

	// statistics
	uint64_t mem_allocated = 0;
	uint64_t mem_wasted = 0;

	char fakeUUID[VK_UUID_SIZE + 1];

	trace_data<lava_file_writer*> thread_streams;

	/// We cannot allow the app to map or unmap memory while we are scanning it
	lava::mutex memory_mutex;

	void self_test() const
	{
	}

private:
	void make_writer(unsigned index = UINT32_MAX, bool thread_barriers_active = true);

	std::string mPath;
	std::string mPack;
	VkuVulkanLibrary library = nullptr;
	Json::Value mJson GUARDED_BY(frame_mutex);
	Json::Value mInputTracking GUARDED_BY(frame_mutex);
	bool should_serialize = false;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	std::atomic<bool> android_finish_monitor_running = false;
	std::thread android_finish_monitor_thread;
#endif
};

void tool_write_vkCreateSurfaceKHR_packet(const surface_create_packet& packet, const char* name, lava_function_id id);

inline void lava_file_writer::write_api_command(uint16_t id)
{
	device = VK_NULL_HANDLE;
	physicalDevice = VK_NULL_HANDLE;
	commandBuffer = VK_NULL_HANDLE;
	if (!write_output && pending_barrier.load(std::memory_order_relaxed))
	{
		frame_mutex.lock();
		inject_thread_barrier();
		pending_barrier.store(false, std::memory_order_relaxed);
		frame_mutex.unlock();
	}
	begin_packet(PACKET_VULKAN_API_CALL); // API call
	write_uint16_t(id); // API call name by id
	write_uint32_t(0); // reserved for future use
}

inline lava_file_writer& write_header(const char* funcname, lava_function_id id, bool thread_barrier = false)
{
	lava_writer& instance = lava_writer::instance();
	lava_file_writer& writer = instance.file_writer();
	if (thread_barrier && !writer.write_output) { frame_mutex.lock(); writer.inject_thread_barrier(); frame_mutex.unlock(); }
	writer.write_api_command(id);
	writer.current.call_id = id;
	writer.current.frame = instance.global_frame;
	DLOG("[t%02u %06u] Seq %s%s", writer.current.thread, writer.current.packet, funcname, thread_barrier ? " (prefaced by thread barrier)" : "");
	return writer;
}
