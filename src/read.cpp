#include <fstream>
#include <errno.h>

#include "lavatube.h"
#include "read.h"
#include "packfile.h"
#include "jsoncpp/json/reader.h"
#include "read_auto.h"

/// Mutex to enforce additional external synchronization
lava::mutex sync_mutex;

// --- misc

Json::Value readJson(const std::string& filename, const std::string packedfile)
{
	return packed_json(filename, packedfile);
}

// --- file reader

lava_file_reader::lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, int start, int end, bool preload)
	: file_reader(packed_open("thread_" + std::to_string(mytid) + ".bin", path), mytid)
	, tid(mytid)
	, mPreload(preload)
{
	parent = _parent;
	global_frames = frames;
	Json::Value frameinfo = readJson("frames_" + _to_string(tid) + ".json", path);

	if (frameinfo.isMember("thread_name"))
	{
		set_thread_name(frameinfo["thread_name"].asString().c_str());
	}

	// Translate global frames to local frames and set our preload window
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
				mBytesEndPreload = i["position"].asUInt64();
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
	if (mPreload && start == 0) // initiate preload right away
	{
		initiate_preload(mBytesEndPreload);
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
	FILE* fptr = fopen("lavaresults.txt", "w");
	fprintf(fptr, "%.2f", fps);
	fclose(fptr);
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
	global_mutex.lock();
	if (thread_streams.count(thread_id) == 0)
	{
		lava_file_reader* f = new lava_file_reader(this, mPackedFile, thread_id, mGlobalFrames, mStart, mEnd, mPreload);
		thread_streams.emplace(thread_id, std::move(f));
	}
	lava_file_reader* ret = thread_streams.at(thread_id);
	global_mutex.unlock();
	return *ret;
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
	retrace_init(readJson("limits.json", mPackedFile), heap_size);
	Json::Value trackable = readJson("tracking.json", mPackedFile);
	trackable_read(trackable);

	// Set up buffer device address tracking
	if (trackable.isMember("VkBuffer"))
	{
		for (uint32_t i = 0; i < VkBuffer_index.size(); i++)
		{
			Json::Value& buf = trackable["VkBuffer"][i];
			if (buf.isMember("buffer_device_address"))
			{
				VkDeviceAddress address = buf["buffer_device_address"].asUInt64();
				buffer_device_address_remapping[address] = &VkBuffer_index.at(i);
			}
		}
	}

	Json::Value meta = readJson("metadata.json", mPackedFile);
	mGlobalFrames = meta["global_frames"].asInt();
	const int num_threads = meta["threads"].asInt();
	thread_call_numbers = new std::vector<std::atomic_uint_fast32_t>(num_threads);
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
