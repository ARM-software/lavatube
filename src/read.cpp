#include <fstream>
#include <errno.h>

#include "lavatube.h"
#include "read.h"
#include "packfile.h"
#include "jsoncpp/json/reader.h"
#include "json_helpers.h"
#include "read_auto.h"
#include "util_auto.h"
#include "suballocator.h"

/// Mutex to enforce additional external synchronization
lava::mutex sync_mutex;

// --- file reader

lava_file_reader::lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, const Json::Value& frameinfo, size_t uncompressed_size, size_t uncompressed_target, int start, int end)
	: file_reader(packed_open("thread_" + std::to_string(mytid) + ".bin", path), mytid, uncompressed_size, uncompressed_target)
{
	parent = _parent;
	run = parent->run;
	global_frames = frames;
	current.thread = mytid;
	current.call = 0;
	current.frame = 0;
	current.thread = mytid;

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
	release_checkpoint();
	const uint8_t r = read_uint8_t();
	assert(r != 0); // invalid value for instrtype
	return r;
}

lava_file_reader::~lava_file_reader()
{
}

uint16_t lava_file_reader::read_apicall()
{
	set_checkpoint();
	const uint16_t apicall = parent->dictionary.at(read_uint16_t());
	(void)read_uint32_t(); // reserved for future use
	DLOG2("[t%02u f%u %06d] %s", current.thread, current.frame, (int)parent->thread_call_numbers->at(current.thread).load(std::memory_order_relaxed) + 1, get_function_name(apicall));
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
	delete thread_call_numbers;
	for (auto& t : thread_streams)
	{
		delete t;
	}
	thread_streams.clear();
}

void lava_reader::finalize(bool terminate)
{
	const double total_time_ms = ((gettime() - mStartTime.load()) / 1000000UL);
	const double fps = (double)mGlobalFrames / (total_time_ms / 1000.0);
	ILOG("==== %.2f ms, %u frames (%.2f fps) ====", total_time_ms, mGlobalFrames, fps);
	Json::Value out;
	out["fps"] = fps;
	out["frames"] = mGlobalFrames;
	out["time"] = total_time_ms;
	uint64_t runner = 0;
	uint64_t worker = 0;
	for (unsigned i = 0; i < threads.size(); i++)
	{
		uint64_t runner_local = 0;
		uint64_t worker_local = 0;
		thread_streams[i]->stop_measurement(worker_local, runner_local);
		DLOG("CPU time thread %u - readahead worker %lu, API runner %lu", i, (long unsigned)worker_local, (long unsigned)runner_local);
		runner += runner_local;
		worker += worker_local;
	}
	struct timespec stop_process_cpu_usage;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stop_process_cpu_usage) != 0)
	{
		ELOG("Failed to get process CPU usage at stop time: %s", strerror(errno));
	}
	assert(stop_process_cpu_usage.tv_sec >= process_cpu_usage.tv_sec);
	const uint64_t process_time = diff_timespec(&stop_process_cpu_usage, &process_cpu_usage);
	ILOG("CPU time spent in ms - readhead workers %lu, API runners %lu, full process %lu", (long unsigned)worker, (long unsigned)runner, (long unsigned)process_time);
	out["readahead_workers_time"] = worker;
	out["api_runners_time"] = runner;
	out["process_time"] = process_time;
	if (terminate)
	{
		if (p__debug_destination) fflush(p__debug_destination);
		for (auto& v : *thread_call_numbers) v = 0; // stop waiting threads from progressing
		for (unsigned i = 0; i < threads.size(); i++)
		{
			if (!thread_streams[i]->terminated.load()) pthread_cancel(threads[i].native_handle());
		}
	}
	write_json(out_fptr, out);
	fclose(out_fptr);
}

lava_file_reader& lava_reader::file_reader(uint16_t thread_id)
{
	return *thread_streams.at(thread_id);
}

void lava_reader::init(const std::string& path)
{
	// read dictionary
	mPackedFile = path;
	Json::Value dict = packed_json("dictionary.json", mPackedFile);
	for (const std::string& funcname : dict.getMemberNames())
	{
		const uint16_t trace_index = dict[funcname].asInt(); // old index
		const uint16_t retrace_index = retrace_getid(funcname.c_str()); // new index
		if (retrace_index != UINT16_MAX) dictionary[trace_index] = retrace_index; // map old index to new
		else DLOG("Function %s from trace dictionary not supported! If used, we will fail!", funcname.c_str());
	}

	// read limits and allocate the global remapping structures
	retrace_init(*this, packed_json("limits.json", mPackedFile));
	Json::Value trackable = packed_json("tracking.json", mPackedFile);
	trackable_read(trackable);

	// Set initial value, in case no start frame reached
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process_cpu_usage) != 0)
	{
		ELOG("Failed to initialize process CPU usage: %s", strerror(errno));
	}

	Json::Value meta = packed_json("metadata.json", mPackedFile);
	const int num_threads = meta["threads"].asInt();
	mGlobalFrames = meta["global_frames"].asInt();
	stored_version_major = meta["lavatube_version_major"].asInt();
	stored_version_minor = meta["lavatube_version_minor"].asInt();
	stored_version_patch = meta["lavatube_version_patch"].asInt();

	// initialize threads -- note that this happens before threading begins, so thread safe
	threads.resize(num_threads);
	thread_streams.resize(num_threads);
	thread_call_numbers = new std::vector<std::atomic_uint_fast32_t>(num_threads);

	for (int thread_id = 0; thread_id < num_threads; thread_id++)
	{
		Json::Value frameinfo = packed_json("frames_" + _to_string(thread_id) + ".json", path);
		const size_t uncompressed_size = frameinfo["uncompressed_size"].asUInt64();
		size_t uncompressed_target = uncompressed_size;
		if (frameinfo.isMember("frames") && mEnd > 0)
		{
			for (const auto& v : frameinfo["frames"])
			{
				if (v["global_frame"] == mEnd + 1) { uncompressed_target = v["position"].asUInt(); break; }
			}
		}
		thread_streams[thread_id] = new lava_file_reader(this, mPackedFile, thread_id, mGlobalFrames, frameinfo, uncompressed_size, uncompressed_target, mStart, mEnd);
	}

	out_fptr = fopen("lavaresults.json", "w");
	if (!out_fptr) ABORT("Failed to open results file: %s", strerror(errno));
	mStartTime.store(gettime());
}

void lava_reader::dump_info()
{
	// App info
	Json::Value meta = packed_json("metadata.json", mPackedFile);
	printf("App name: %s\n", meta["applicationInfo"]["applicationName"].asCString());
	printf("App version: %s\n", meta["applicationInfo"]["applicationVersion"].asCString());
	printf("App engine: %s\n", meta["applicationInfo"]["engineName"].asCString());
	printf("Traced device name: %s\n", meta["devicePresented"]["deviceName"].asCString());
	printf("Traced device version: %s\n", meta["devicePresented"]["apiVersion"].asCString());
	printf("Frames: %d\n", meta["global_frames"].asInt());
	// Trace info
	Json::Value tracking = packed_json("tracking.json", mPackedFile);
	printf("Swapchains:\n");
	for (const auto& item : tracking["VkSwapchainKHR"])
	{
		printf("\t%d: %dx%d\n", item["index"].asInt(), item["width"].asInt(), item["height"].asInt());
	}
	// Limits
	Json::Value limits = packed_json("limits.json", mPackedFile);
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
			Json::Value t = packed_json("frames_" + _to_string(thread) + ".json", mPackedFile);
			printf("Thread %u:\n", thread);
			printf("\tFrames: %d\n", (int)t["frames"].size());
			printf("\tUncompressed size: %u\n", t["uncompressed_size"].asUInt());
		}
	}
}

uint32_t lava_reader::find_address_candidates(trackedbuffer& buffer_data, VkDeviceSize size, const void* ptr, VkDeviceSize base_offset, change_source source) const
{
	buffer_data.self_test();
	const char* base_ptr = (const char*)ptr;
	// Search on a 4-byte aligned boundary
	if ((uintptr_t)ptr % sizeof(uint32_t) != 0)
	{
		ptr = (const void*)aligned_size((uintptr_t)ptr, sizeof(uint32_t));
	}
	const uint32_t* start = (const uint32_t*)ptr;
	const uint32_t* end = (const uint32_t*)(base_ptr + size);
	uint32_t found = 0;
	for (const uint32_t* p = start; p + 2 <= end; p++)
	{
		const VkDeviceSize offset = base_offset + (VkDeviceSize)((const char*)p - base_ptr);
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
		const trackedobject* data = device_address_remapping.get_by_address(candidate);
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
