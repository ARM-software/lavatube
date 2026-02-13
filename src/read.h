// Vulkan trace common code

#pragma once

#include <assert.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <list>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <stdio.h>
#include <functional>
#include <deque>

#include "lavamutex.h"
#include "containers.h"
#include "lavatube.h"
#include "filereader.h"
#include "jsoncpp/json/value.h"

using lava_replay_func = std::function<void(lava_file_reader&)>;
class lava_file_reader;

extern lava::mutex sync_mutex;

struct address_rewrite
{
	VkDeviceSize offset;
	VkDeviceSize size;
	change_source source;
};

class lava_reader
{
	friend lava_file_reader;

public:
	lava_reader() {}
	lava_reader(const std::string& path) { init(path); }
	void init(const std::string& path);
	~lava_reader();

	lava_file_reader& file_reader(uint16_t thread_id);

	/// end -1 means play until the end
	void set_frames(int start, int end)
	{
		mStart = start;
		mEnd = end;
		if (mEnd != -1) mGlobalFrames = end - start;
	}

	/// Dump trace information to stdout
	void dump_info();

	void finalize(bool terminate);

	std::vector<std::atomic_uint_fast32_t>* thread_call_numbers; // thread local call numbers

	// Use the remapping lists below to find possible candidates for remapping in a buffer.
	// Return the number of candidates found. Will search from 'ptr' over a 'size' sized window.
	// base_offset is the byte offset from the start of the buffer to 'ptr'.
	// Caller must make sure access is thread safe.
	uint32_t find_address_candidates(trackedbuffer& buffer_data, VkDeviceSize size, const void* ptr, VkDeviceSize base_offset, change_source source) const;

	// This is thread safe since we allocate it all before threading begins.
	address_remapper<trackedobject> device_address_remapping;
	address_remapper<trackedaccelerationstructure> acceleration_structure_address_remapping;

	// Our rewrite queue. Only used during post-processing. During first pass entries are ordered by entry time. During second
	// pass they must be ordered by change time.
	std::list<address_rewrite> rewrite_queue;

	/// Are we currently looking for remap and rewrite candidates?
	bool remap_scan = false;
	bool raytracing_callbacks_registered = false;

	/// Current global frame (only use for logging)
	std::atomic_int global_frame{ 0 };

	std::vector<std::thread> threads;

	// The dictionary is read from a JSON file and then mapped from their to our function ids.
	std::unordered_map<uint16_t, uint16_t> dictionary;

	/// Whether we should actually call into Vulkan or if we are just processing the data.
	/// Duplicated into the file reader.
	bool run = true;

	// Version numbers are never reset but just keep increasing
	int stored_version_major = 0;
	int stored_version_minor = 0;
	int stored_version_patch = 0;

	/// Set our exit status here if we want to signal something special (eg a skip test value)
	std::atomic_int exit_status { 0 };

private:
	/// Start time of frame range
	std::atomic_uint64_t mStartTime{ 0 };
	/// Start CPU usage for whole process
	struct timespec process_cpu_usage;
	std::string mPackedFile;
	std::vector<lava_file_reader*> thread_streams;
	int mStart = 0;
	int mEnd = -1;
	int mGlobalFrames = 0;
	FILE* out_fptr = nullptr;
};

class lava_file_reader : public file_reader
{
	lava_file_reader(const lava_file_reader&) = delete;
	lava_file_reader& operator=(const lava_file_reader&) = delete;

public:
	/// Initialize one thread of replay. Frame start and end values are global, not local. Frames are either (end-start)
	/// or total number of global frames in the trace.
	lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, const Json::Value& frameinfo, size_t uncompressed_size, size_t uncompressed_wanted, int start = 0, int end = -1);
	~lava_file_reader();

	/// Returns zero if no more instructions in the file, or the instruction type found.
	uint8_t step();

	VkFlags read_VkFlags() { uint32_t t; read_value(&t); return static_cast<VkFlags>(t); }

	inline int thread_index() const { return current.thread; }

	inline VkDescriptorDataEXT read_VkDescriptorDataEXT() { return VkDescriptorDataEXT{}; } // TBD
	inline VkAccelerationStructureNV read_VkAccelerationStructureNV() { return VK_NULL_HANDLE; }

	inline uint32_t read_handle(DEBUGPARAM(const char* name));
#ifdef DEBUG
	inline void read_handle_array(const char* name, uint32_t* dest, uint32_t length) { for (uint32_t i = 0; i < length; i++) dest[i] = read_handle(name); }
#else
	inline void read_handle_array(uint32_t* dest, uint32_t length) { for (uint32_t i = 0; i < length; i++) dest[i] = read_handle(); }
#endif
	inline void read_barrier();
	uint16_t read_apicall();

	/// Read patch update while scanning for remap candidates
	uint32_t read_patch_scanning(char* buf, uint64_t maxsize, trackedbuffer& buffer_data)
	{
		char* ptr = buf;
		uint32_t offset;
		uint32_t size;
		uint64_t changed = 0;
		do {
			offset = read_uint32_t();
			ptr += offset;
			// cppcheck-suppress nullPointerRedundantCheck
			assert(maxsize == 0 || ptr <= buf + maxsize);
			size = read_uint32_t();
			check_space(size);
			const char* uptr = uncompressed_data + read_position;
			if (buf && size)
			{
				memcpy(ptr, uptr, size);
				const VkDeviceSize base_offset = (VkDeviceSize)(ptr - buf);
				parent->find_address_candidates(buffer_data, size, ptr, base_offset, current);
			}
			read_position += size;
			ptr += size;
			changed += size;
			// cppcheck-suppress nullPointerRedundantCheck
			assert(maxsize == 0 || ptr <= buf + maxsize);
		}
		while (!(offset == 0 && size == 0));
		return changed;
	}

	/// If this one returns true, we are responsible for cleaning up all Vulkan calls and then exiting.
	bool new_frame()
	{
		if (mStart == (int)current.frame)
		{
			parent->mStartTime.store(gettime());
			if (mHaveFirstFrame)
			{
				ILOG("==== starting frame frange ====");
				if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &parent->process_cpu_usage) != 0)
				{
					ELOG("Failed to get process CPU usage: %s", strerror(errno));
				}
			}
			start_measurement();
		}
		current.frame++;
		parent->global_frame++; // just use for logging purposes
		if (mEnd != -1 && (int)current.frame == mEnd)
		{
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

	/// Whether we should actually call into Vulkan or if we are just processing the data
	bool run = true;

	// Replay-only: per-thread queue for AS build sizes and internal AS buffers.
	std::deque<VkAccelerationStructureBuildSizesInfoKHR> pending_as_build_sizes;
	std::deque<internal_buffer> pending_as_storage_buffers;

	/// Is this reader's thread terminated?
	std::atomic_bool terminated{ false };

	change_source current;

	void self_test() const
	{
		assert(parent);
		assert(run == parent->run);
		assert(global_frames >= local_frames);
		file_reader::self_test();
	}

private:
	int mStart = 0;	///< Local start frame
	int mEnd = -1; ///< Local end frame
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
		assert(call != UINT32_MAX);
		DLOG3("Thread barrier on thread %d, waiting for call %u on thread %d / %u", current.thread, call, i, size - 1);
		while (i != current.thread && call > parent->thread_call_numbers->at(i).load(std::memory_order_relaxed)) usleep(1);
	}
	DLOG2("[t%02d] Passed thread barrier, waited for %u threads", (int)current.thread, size);
}

inline uint32_t lava_file_reader::read_handle(DEBUGPARAM(const char* name))
{
	const uint32_t index = read_uint32_t();
	const int req_thread = read_int8_t();
	const int req_call = read_uint16_t();
	if (req_thread < 0 || req_thread == (int)current.thread)
	{
		DLOG2("[t%02d %06d] read handle %s index=%u from same thread", (int)current.thread, (int)current.call + 1, name, (unsigned)index);
		return index;
	}
	// check for thread dependency, if we need a resource not provided yet, spin until it is
	int currentcall = parent->thread_call_numbers->at(req_thread).load(std::memory_order_relaxed);
#ifdef DEBUG
	if (req_call <= currentcall) DLOG2("[t%02d %06d] read handle %s index=%u, was for tid=%d call=%d, it is already at call=%d", (int)current.thread, (int)current.call + 1, name, (unsigned)index, (int)req_thread, req_call, currentcall);
	else DLOG2("[t%02d %06d] read handle %s index=%u, MUST WAIT for tid=%d call=%d, it is now at call=%d", (int)current.thread, (int)current.call + 1, name, (unsigned)index, (int)req_thread, req_call, currentcall);
#endif
	while (req_call > currentcall)
	{
		usleep(1);
		currentcall = parent->thread_call_numbers->at(req_thread).load(std::memory_order_relaxed);
	}
	return index;
}
