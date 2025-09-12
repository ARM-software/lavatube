#include <fstream>
#include <errno.h>

#include "lavatube.h"
#include "read.h"
#include "packfile.h"
#include "jsoncpp/json/reader.h"
#include "read_auto.h"
#include "util_auto.h"

/// Mutex to enforce additional external synchronization
lava::mutex sync_mutex;
thread_local lava_file_reader* local_reader_ptr;

// --- misc

Json::Value readJson(const std::string& filename, const std::string packedfile)
{
	return packed_json(filename, packedfile);
}

// --- file reader

lava_file_reader::lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, int start, int end)
	: file_reader(packed_open("thread_" + std::to_string(mytid) + ".bin", path), mytid)
{
	parent = _parent;
	run = parent->run;
	global_frames = frames;
	Json::Value frameinfo = readJson("frames_" + _to_string(mytid) + ".json", path);
	current.thread = mytid;
	current.call = 0;
	current.frame = 0;
	current.thread = mytid;

	local_reader_ptr = this;

	if (frameinfo.isMember("thread_name"))
	{
		set_thread_name(frameinfo["thread_name"].asString().c_str());
	}

	// Translate global frames to local frames and set our measurement window
	if (end != -1)
	{
		for (const auto& i : frameinfo["frames"])
		{
			if (mStart == -1 && i["global_frame"].asInt() >= start)
			{
				mStart = i["local_frame"].asInt();
			}
			if (i["global_frame"].asInt() <= end)
			{
				mEnd = i["local_frame"].asInt();
			}
			if (i["global_frame"].asInt() == i["local_frame"].asInt())
			{
				if (i["global_frame"].asInt() == end) mHaveFinalFrame = true;
				if (i["global_frame"].asInt() == start) mHaveFirstFrame = true;
			}
		}
	}
	else
	{
		local_frames = frameinfo["frames"].size();
		if (frames == frameinfo["highest_global_frame"].asInt()) mHaveFinalFrame = true;
		if (frameinfo["frames"][0]["global_frame"] == 0) mHaveFirstFrame = true;
	}
}

uint8_t lava_file_reader::step()
{
	if (file_reader::done())
	{
		terminated.store(true); // prevent us from calling pthread_cancel on this thread later
		return 0; // done
	}
	const uint8_t r = read_uint8_t();
	assert(r != 0); // invalid value for instrtype
	return r;
}

lava_file_reader::~lava_file_reader()
{
}

uint16_t lava_file_reader::read_apicall()
{
	const uint16_t apicall = parent->dictionary.at(read_uint16_t());
	(void)read_uint32_t(); // reserved for future use
	DLOG("[t%02u %06d] %s", current.thread, (int)parent->thread_call_numbers->at(current.thread).load(std::memory_order_relaxed) + 1, get_function_name(apicall));
	lava_replay_func func = retrace_getcall(apicall);
	current.call_id = apicall;
	func(*this);
	current.call++;
	parent->thread_call_numbers->at(current.thread).fetch_add(1, std::memory_order_relaxed);
	pool.reset();
	return apicall;
}

// --- trace reader

lava_reader::~lava_reader()
{
	global_mutex.lock();
	delete thread_call_numbers;
	for (auto& t : thread_streams)
	{
		delete t.second;
	}
	thread_streams.clear();
	global_mutex.unlock();
}

void lava_reader::finalize(bool terminate)
{
	const double total_time_ms = ((gettime() - mStartTime.load()) / 1000000UL);
	const double fps = (double)mGlobalFrames / (total_time_ms / 1000.0);
	ILOG("==== %.2f ms, %u frames (%.2f fps) ====", total_time_ms, mGlobalFrames, fps);
	fprintf(out_fptr, "%.2f", fps);
	fclose(out_fptr);
	if (terminate)
	{
		global_mutex.lock();
		for (auto& v : *thread_call_numbers) v = 0; // stop waiting threads from progressing
		for (unsigned i = 0; i < threads.size(); i++)
		{
			if (!thread_streams[i]->terminated.load()) pthread_cancel(threads[i].native_handle());
		}
		global_mutex.unlock();
	}
}

lava_file_reader& lava_reader::file_reader(uint16_t thread_id)
{
	lava::lock_guard keep(global_mutex);
	return *thread_streams.at(thread_id);
}

void lava_reader::init(const std::string& path, int heap_size)
{
	// read dictionary
	mPackedFile = path;
	Json::Value dict = readJson("dictionary.json", mPackedFile);
	for (const std::string& funcname : dict.getMemberNames())
	{
		const uint16_t trace_index = dict[funcname].asInt(); // old index
		const uint16_t retrace_index = retrace_getid(funcname.c_str()); // new index
		if (retrace_index != UINT16_MAX) dictionary[trace_index] = retrace_index; // map old index to new
		else DLOG("Function %s from trace dictionary not supported! If used, we will fail!", funcname.c_str());
	}

	// read limits and allocate the global remapping structures
	retrace_init(*this, readJson("limits.json", mPackedFile), heap_size, run);
	Json::Value trackable = readJson("tracking.json", mPackedFile);
	trackable_read(trackable);

	// Set up buffer device address tracking
	if (trackable.isMember("VkBuffer"))
	{
		for (uint32_t i = 0; i < VkBuffer_index.size(); i++)
		{
			Json::Value& buf = trackable["VkBuffer"][i];
			if (buf.isMember("device_address"))
			{
				VkDeviceAddress address = buf["device_address"].asUInt64();
				device_address_remapping.add(address, &VkBuffer_index.at(i));
			}
		}
	}
	if (trackable.isMember("VkAccelerationStructureKHR"))
	{
		for (uint32_t i = 0; i < VkAccelerationStructureKHR_index.size(); i++)
		{
			Json::Value& buf = trackable["VkAccelerationStructureKHR"][i];
			if (buf.isMember("device_address"))
			{
				VkDeviceAddress address = buf["device_address"].asUInt64();
				acceleration_structure_address_remapping.add(address, &VkAccelerationStructureKHR_index.at(i));
			}
		}
	}

	Json::Value meta = readJson("metadata.json", mPackedFile);
	mGlobalFrames = meta["global_frames"].asInt();
	const int num_threads = meta["threads"].asInt();
	global_mutex.lock();
	thread_call_numbers = new std::vector<std::atomic_uint_fast32_t>(num_threads);
	for (int thread_id = 0; thread_id < num_threads; thread_id++)
	{
		lava_file_reader* f = new lava_file_reader(this, mPackedFile, thread_id, mGlobalFrames, mStart, mEnd);
		thread_streams.emplace(thread_id, std::move(f));
	}
	global_mutex.unlock();

	out_fptr = fopen("lavaresults.txt", "w");
	if (!out_fptr) ABORT("Failed to open results file: %s", strerror(errno));
}

void lava_reader::dump_info()
{
	// App info
	Json::Value meta = readJson("metadata.json", mPackedFile);
	printf("App name: %s\n", meta["applicationInfo"]["applicationName"].asCString());
	printf("App version: %s\n", meta["applicationInfo"]["applicationVersion"].asCString());
	printf("App engine: %s\n", meta["applicationInfo"]["engineName"].asCString());
	printf("Traced device name: %s\n", meta["devicePresented"]["deviceName"].asCString());
	printf("Traced device version: %s\n", meta["devicePresented"]["apiVersion"].asCString());
	printf("Frames: %d\n", meta["global_frames"].asInt());
	// Trace info
	Json::Value tracking = readJson("tracking.json", mPackedFile);
	printf("Swapchains:\n");
	for (const auto& item : tracking["VkSwapchainKHR"])
	{
		printf("\t%d: %dx%d\n", item["index"].asInt(), item["width"].asInt(), item["height"].asInt());
	}
	// Limits
	Json::Value limits = readJson("limits.json", mPackedFile);
	printf("Resources:\n");
	for (const std::string& key : limits.getMemberNames())
	{
		if (limits[key].asInt() > 0)
		{
			printf("\t%s : %d\n", key.c_str(), limits[key].asInt());
		}
	}
	// Read each thread
	if (p__debug_level > 0)
	{
		std::vector<std::string> files = packed_files(mPackedFile, "frames_");
		for (unsigned thread = 0; thread < files.size(); thread++)
		{
			Json::Value t = readJson("frames_" + _to_string(thread) + ".json", mPackedFile);
			printf("Thread %u:\n", thread);
			printf("\tFrames: %d\n", (int)t["frames"].size());
			printf("\tUncompressed size: %u\n", t["uncompressed_size"].asUInt());
		}
	}
}

uint32_t lava_reader::find_address_candidates(trackedbuffer& buffer_data, VkDeviceSize size, const void* ptr, change_source source) const
{
	buffer_data.self_test();
	// Search on a 4-byte aligned boundary
	if ((uintptr_t)ptr % sizeof(uint32_t) != 0)
	{
		ptr = (const void*)aligned_size((uintptr_t)ptr, sizeof(uint32_t));
	}
	const uint32_t* start = (const uint32_t*)ptr;
	const uint32_t* end = (const uint32_t*)((char*)ptr + size);
	uint32_t found = 0;
	for (const uint32_t* p = start; p + 2 <= end; p++)
	{
		const VkDeviceSize offset = (VkDeviceSize)p;
		const VkDeviceAddress candidate = *((uintptr_t*)p); // read full 64bit word at current position

		// Do we already have a candidate for this address?
		if (buffer_data.candidate_lookup.count(offset) > 0)
		{
			auto it = buffer_data.candidate_lookup.at(offset);
			if (it->address != candidate) // did it change? if so, update or remove it
			{
				if (device_address_remapping.get_by_address(candidate) || acceleration_structure_address_remapping.get_by_address(candidate))
				{
					it->address = candidate;
					it->source = source;
				}
				else
				{
					buffer_data.remove_candidate(offset);
				}
			}
			continue;
		}

		// First check for whole buffer
		const trackedmemoryobject* data = device_address_remapping.get_by_address(candidate);
		if (data)
		{
			buffer_data.add_candidate(offset, candidate, source);
			found++;
			continue;
		}
		// Then check for the more restricted acceleration structure subset. Need to check both since user may not have
		// taken the device address of the whole buffer.
		data = acceleration_structure_address_remapping.get_by_address(candidate);
		if (data)
		{
			buffer_data.add_candidate(offset, candidate, source);
			found++;
		}
	}
	buffer_data.self_test();
	return found;
}
