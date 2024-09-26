// Vulkan trace common code

#pragma once

#include <assert.h>
#include <atomic>
#include <thread>
#include <cstdint>
#include <thread>
#include <list>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <stdio.h>
#include <functional>

#include "lavamutex.h"
#include "containers.h"
#include "lavatube.h"
#include "filereader.h"
#include "jsoncpp/json/value.h"

using lava_replay_func = std::function<void(lava_file_reader&)>;
class lava_file_reader;

extern lava::mutex sync_mutex;

// these are implemented differently for trace and replay cases

struct trackedcmdbuffer_replay : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t pool = 0;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};

struct trackeddescriptorset_replay : trackable
{
	using trackable::trackable; // inherit constructor
	uint32_t pool = 0;
};

class lava_reader
{
	friend lava_file_reader;

public:
	lava_reader() {}
	lava_reader(const std::string& path, int heap_size = -1) { init(path, heap_size); }
	void init(const std::string& path, int heap_size = -1);
	~lava_reader();

	lava_file_reader& file_reader(uint16_t thread_id);

	/// end -1 means play until the end
	void parameters(int start, int end, bool preload = false)
	{
		mStart = start;
		mEnd = end;
		if (mEnd != -1) mGlobalFrames = end - start;
		mPreload = preload;
	}

	/// Dump trace information to stdout
	void dump_info();

	void finalize(bool terminate);

	std::vector<std::atomic_uint_fast32_t>* thread_call_numbers; // thread local call numbers

	// This is thread safe since we allocate it all before threading begins.
	std::map<VkDeviceAddress, trackedbuffer*> buffer_device_address_remapping;

	/// Current global frame (only use for logging)
	std::atomic_int global_frame{ 0 };

	std::vector<std::thread> threads;

	// The dictionary is read from a JSON file and then mapped from their to our function ids.
	std::unordered_map<uint16_t, uint16_t> dictionary;

private:
	/// Start time of frame range
	std::atomic_uint64_t mStartTime{ 0 };

	lava::mutex global_mutex;
	std::string mPackedFile;
	std::unordered_map<int, lava_file_reader*> thread_streams GUARDED_BY(global_mutex);
	int mStart = 0;
	int mEnd = -1;
	int mGlobalFrames = 0;
	bool mPreload = false;
};

class lava_file_reader : public file_reader
{
	lava_file_reader(const lava_file_reader&) = delete;
	lava_file_reader& operator=(const lava_file_reader&) = delete;

public:
	/// Initialize one thread of replay. Frame start and end values are global, not local. Frames are either (end-start)
	/// or total number of global frames in the trace.
	lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, int start = 0, int end = -1, bool preload = false);
	~lava_file_reader();

	/// Returns zero if no more instructions in the file, or the instruction type found.
	uint8_t step();

	VkFlags read_VkFlags() { uint32_t t; read_value(&t); return static_cast<VkFlags>(t); }

	inline int thread_index() const { return tid; }

	inline VkDescriptorDataEXT read_VkDescriptorDataEXT() { return VkDescriptorDataEXT{}; } // TBD
	inline VkAccelerationStructureNV read_VkAccelerationStructureNV() { return VK_NULL_HANDLE; }

	inline uint32_t read_handle();
	inline void read_handle_array(uint32_t* dest, uint32_t length) { for (uint32_t i = 0; i < length; i++) dest[i] = read_handle(); }
	inline void read_barrier();

	/// If this one returns true, we are responsible for cleaning up all Vulkan calls and then exiting.
	bool new_frame()
	{
		if (mStart == local_frame)
		{
			parent->mStartTime.store(gettime());
			if (mPreload) initiate_preload(mBytesEndPreload);
			if (mHaveFirstFrame) ILOG("==== starting frame frange ====");
		}
		local_frame++;
		parent->global_frame++; // just use for logging purposes
		if (mEnd != -1 && local_frame == mEnd)
		{
			if (mPreload) reset_preload();
			if (mHaveFinalFrame)
			{
				return true;
			}
		}
		return false;
	}

	lava_reader* parent;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	/// Current local frame
	int local_frame = 0;

	/// Is this reader's thread terminated?
	std::atomic_bool terminated{ false };

private:
	int tid;
	bool mPreload;
	int mStart = 0;	///< Local start frame
	int mEnd = -1; ///< Local end frame
	uint64_t mBytesEndPreload = 0; ///< Local byte boundary to end preloading
	/// Do we own the final global frame?
	bool mHaveFinalFrame = false;
	/// Do we own the first global frame?
	bool mHaveFirstFrame = false;
	/// Total amount of global frames (for final FPS calculation)
	unsigned global_frames = 0;
	unsigned local_frames = 0;
};

inline void lava_file_reader::read_barrier()
{
	const unsigned size = read_uint8_t();
	for (int i = 0; i < (int)size; i++)
	{
		const unsigned call = read_uint32_t();
		DLOG3("Thread barrier on thread %d, waiting for call %u on thread %d / %u", tid, call, i, size - 1);
		while (i != tid && call > parent->thread_call_numbers->at(i).load(std::memory_order_relaxed)) usleep(1);
	}
	DLOG2("Passed thread barrier on thread %d, waited for %u threads", tid, size);
}

inline uint32_t lava_file_reader::read_handle()
{
	const uint32_t index = read_uint32_t();
	const int req_thread = read_int8_t();
	const uint16_t req_call = read_uint16_t();
	DLOG3("%d : read handle idx=%u tid=%d call=%u", tid, (unsigned)index, (int)req_thread, (unsigned)req_call);
	if (req_thread < 0 || req_thread == tid) return index;
	// check for thread dependency, if we need a resource not provided yet, spin until it is
	int currentcall = parent->thread_call_numbers->at(req_thread).load(std::memory_order_relaxed);
	while (req_call > currentcall)
	{
		usleep(1);
		currentcall = parent->thread_call_numbers->at(req_thread).load(std::memory_order_relaxed);
	}
	return index;
}
