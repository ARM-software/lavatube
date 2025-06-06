#include <fstream>
#include <errno.h>
#include <unistd.h>

#include "write.h"
#include "jsoncpp/json/writer.h"
#include "write_auto.h"
#include "packfile.h"
#include "density/src/density_api.h"

// Important: Keep this first.
lava::mutex frame_mutex;

// Keep track of which thread we are currently running in, and how many there are
static thread_local int tid = -1;

// --- helpers

static inline void sntimef(char *str, size_t bufSize, const char *format)
{
	time_t utcTime = time(nullptr);
	tm *tmv = localtime(&utcTime);
	strftime(str, bufSize, format, tmv);
}

static void writeJson(const std::string& path, const Json::Value& v)
{
	FILE* fp = fopen(path.c_str(), "w");
	if (!fp)
	{
		ELOG("Failed to open \"%s\": %s", path.c_str(), strerror(errno));
		return;
	}
	Json::StyledWriter writer;
	std::string data = writer.write(v);
	size_t written;
	int err = 0;
	do {
		written = fwrite(data.c_str(), data.size(), 1, fp);
		err = ferror(fp);
	} while (!err && !written);
	if (err)
	{
		ELOG("Failed to write dictionary: %s", strerror(err));
	}
	fclose(fp);
}

// --- trace file writer

lava_file_writer::lava_file_writer(uint16_t _tid, lava_writer* _parent) : file_writer(_tid), parent(_parent)
{
	current.thread = _tid;
	current.call = 0;
	run = parent->run;
	get_thread_name(thread_name);
	if (p__disable_multithread_compress) disable_multithreaded_compress();
	if (p__disable_multithread_writeout) disable_multithreaded_writeout();
}

void lava_file_writer::set(const std::string& path)
{
	assert(mPath.empty());
	std::string fname = path + "/thread_" + _to_string(current.thread) + ".bin";
	mPath = path;
	file_writer::set(fname);
}

void lava_file_writer::push_thread_barriers()
{
	frame_mutex.lock();
	int size = parent->thread_streams.size();
	for (int i = 0; i < size; i++)
	{
		if (i == thread_index()) continue;
		DLOG2("Pushing thread barrier to thread %d", i);
		parent->thread_streams.at(i)->pending_barrier = true;
	}
	frame_mutex.unlock();
}

void lava_file_writer::inject_thread_barrier()
{
	write_uint8_t(PACKET_THREAD_BARRIER); // packet type
	int size = parent->thread_streams.size();
	write_uint8_t(size); // threads to sync
	for (int i = 0; i < size; i++)
	{
		const uint32_t call = parent->thread_streams.at(i)->current.call;
		assert(call != UINT32_MAX);
		write_uint32_t(call);
	}
	DLOG2("Injected thread barrier on thread %d with %d targets", thread_index(), size);
}

lava_file_writer::~lava_file_writer()
{
	self_test();
	file_writer::finalize();

	// Write out per-frame data
	Json::Value v;
	int highest = 0;
	if (thread_name[0] != '\0') v["thread_name"] = thread_name;
	v["frames"] = Json::arrayValue;
	v["uncompressed_size"] = (Json::Value::Int64)uncompressed_bytes;
	for (const framedata& frame : frames)
	{
		Json::Value k;
		k["global_frame"] = frame.global_frame;
		k["local_frame"] = frame.local_frame;
		k["position"] = (Json::Value::Int64)frame.start_pos;
		v["frames"].append(k);
		highest = std::max(highest, frame.global_frame);
	}
	DLOG("Wrapping up thread %u with %d frames", current.thread, highest);
	v["highest_global_frame"] = highest;
	const std::string path = mPath + "/frames_" + _to_string(current.thread) + ".json";
	writeJson(path, v);
}

debug_info lava_file_writer::new_frame(int global_frame)
{
	framedata data;
	data.start_pos = uncompressed_bytes;
	data.global_frame = global_frame;
	data.local_frame = current.frame;
	frames.push_back(data);
	assert(global_frame >= (int)current.frame);
	current.frame++;
	debug_info retval = debug;
	debug = {};
	self_test();
	return retval;
}

// --- trace writer

static lava_writer _instance;

lava_writer& lava_writer::instance()
{
	return _instance;
}

void lava_writer::set(const std::string& path, int as_version)
{
	mAsVersion = as_version;
	mPath = path + "_tmp";
	mPack = path + ".vk";
	ILOG("Base path is set to %s", mPath.c_str());

	// make path
	int result = mkdir(mPath.c_str(), 0755);
	if (result != 0)
	{
		ELOG("Failed to create \"%s\": %s", mPath.c_str(), strerror(errno));
	}

	// inform our workers
	frame_mutex.lock();
	for (unsigned i = 0; i < thread_streams.size(); i++)
	{
		thread_streams.at(i)->set(mPath);
	}
	frame_mutex.unlock();

	should_serialize = true;
}

lava_writer::lava_writer() : global_frame(0)
{
	frame_mutex.lock();

	(void)vulkan_feature_detection_get();

	// assign a fake UUID, so that we get SPIR-V instead of cached pipeline data.
	// the start is "rdoc", and the end is the time that this call was first made
	// 0123456789ABCDEF
	// rdocyymmddHHMMSS
	// we pass size+1 so that there's room for a null terminator (the UUID doesn't
	// need a null terminator as it's a fixed size non-string array)
	sntimef(fakeUUID, VK_UUID_SIZE + 1, "rdoc%y%m%d%H%M%S");

	library = vkuCreateWrapper();
	mJson["lavatube_version_major"] = LAVATUBE_VERSION_MAJOR;
	mJson["lavatube_version_minor"] = LAVATUBE_VERSION_MINOR;
	mJson["lavatube_version_patch"] = LAVATUBE_VERSION_PATCH;
	mJson["vulkan_header_version"] = version_to_string(VK_HEADER_VERSION);
	mJson["density_version_major"] = density_version_major();
	mJson["density_version_minor"] = density_version_minor();
	mJson["density_version_patch"] = density_version_revision();

	frame_mutex.unlock();
}

lava_writer::~lava_writer()
{
	if (should_serialize)
	{
		ELOG("Destructor called, but not yet finished! Trying to wrap things up...");
		serialize();
	}
	else DLOG("Writer wrapping up cleanly...");
	finish();
	vkuDestroyWrapper(library);
}

void lava_writer::serialize()
{
	frame_mutex.lock();
	assert(!mPath.empty());

	// write dictionary to JSON file
	std::string dict_path = mPath + "/dictionary.json";
	Json::Value jd;
	for (const auto& pair : records.function_table)
	{
		jd[pair.first] = (unsigned)pair.second;
	}
	writeJson(dict_path, jd);

	// over-write these in case something was not used
	feature_detection* f = vulkan_feature_detection_get();
	if (meta.app.stored_VkPhysicalDeviceFeatures2) f->adjust_VkPhysicalDeviceFeatures(meta.app.stored_VkPhysicalDeviceFeatures2->features);
	if (meta.app.stored_VkPhysicalDeviceVulkan11Features) f->adjust_VkPhysicalDeviceVulkan11Features(*meta.app.stored_VkPhysicalDeviceVulkan11Features);
	if (meta.app.stored_VkPhysicalDeviceVulkan12Features) f->adjust_VkPhysicalDeviceVulkan12Features(*meta.app.stored_VkPhysicalDeviceVulkan12Features);
	if (meta.app.stored_VkPhysicalDeviceVulkan13Features) f->adjust_VkPhysicalDeviceVulkan13Features(*meta.app.stored_VkPhysicalDeviceVulkan13Features);
	auto removed_device_exts = f->adjust_device_extensions(meta.app.device_extensions);
	auto removed_instance_exts = f->adjust_instance_extensions(meta.app.instance_extensions);
	Json::Value& r = mJson;
	r["instanceRequested"]["removedExtensions"] = Json::arrayValue;
	for (const std::string& name : removed_instance_exts) r["instanceRequested"]["removedExtensions"].append(name);
	r["deviceRequested"]["removedExtensions"] = Json::arrayValue;
	for (const std::string& name : removed_device_exts) r["deviceRequested"]["removedExtensions"].append(name);

	// write metadata to JSON file
	mJson["global_frames"] = global_frame + 1; // +1 since zero-indexed
	mJson["threads"] = (unsigned)thread_streams.size();
	writeJson(mPath + "/metadata.json", mJson);

	// write limits
	writeJson(mPath + "/limits.json", trace_limits(this));

	// write out tracking info for each object
	writeJson(mPath + "/tracking.json", trackable_json(this));

	// write out debug info
	Json::Value dbg;
	Json::Value arr = Json::arrayValue;
	for (unsigned i = 0; i < debug.size(); i++)
	{
		Json::Value v;
		v["frame"] = i;
		v["flushes_queue"] = debug.at(i).flushes_queue.load(std::memory_order_relaxed);
		v["flushes_event"] = debug.at(i).flushes_event.load(std::memory_order_relaxed);
		v["flushes_remap"] = debug.at(i).flushes_remap.load(std::memory_order_relaxed);
		v["flushes_persistent"] = debug.at(i).flushes_persistent.load(std::memory_order_relaxed);
		v["memory_devices"] = debug.at(i).memory_devices.load(std::memory_order_relaxed);
		v["memory_dumps"] = debug.at(i).memory_dumps.load(std::memory_order_relaxed);
		v["memory_scans"] = debug.at(i).memory_scans.load(std::memory_order_relaxed);
		v["memory_bytes"] = (Json::Value::Int64)debug.at(i).memory_bytes.load(std::memory_order_relaxed);
		v["memory_changed_bytes"] = (Json::Value::Int64)debug.at(i).memory_changed_bytes.load(std::memory_order_relaxed);
		v["memory_scans_unchanged"] = debug.at(i).memory_scans_unchanged.load(std::memory_order_relaxed);
		arr.append(v);
	}
	dbg["frames"] = arr;
	dbg["global_frames"] = global_frame + 1;
	writeJson(mPath + "/debug.json", dbg);
	frame_mutex.unlock();
}

void lava_writer::finish()
{
	frame_mutex.lock();

	if ((p__external_memory || p__debug_level >= 1) && mem_allocated > 0)
	{
		ILOG("Memory allocated %lu, wasted %lu", (unsigned long)mem_allocated, (unsigned long)mem_wasted);
	}
	mem_allocated = 0;
	mem_wasted = 0;
	meta.device.reset();
	meta.app.reset();
	should_serialize = false;
	for (unsigned i = 0; i < thread_streams.size(); i++)
	{
		delete thread_streams.at(i);
	}
	thread_streams.clear();
	if (!mPath.empty())
	{
		if (!pack_directory(mPack, mPath, true))
		{
			ELOG("Failed to pack result files!");
		}
	}
	mPath = "";
	mJson = Json::Value();
	global_frame.exchange(0);
	tid = -1;
	vulkan_feature_detection_reset();
	frame_mutex.unlock();
}

lava_file_writer& lava_writer::file_writer()
{
	if (tid == -1)
	{
		frame_mutex.lock();
		tid = thread_streams.size();
		lava_file_writer* f = new lava_file_writer(tid, this);
		if (!mPath.empty())
		{
			f->set(mPath);
		}
		f->inject_thread_barrier();
		thread_streams.emplace_back(std::move(f));
		DLOG("Created thread %d, currently %d threads", (int)tid, (int)thread_streams.size());
		frame_mutex.unlock();
	}
	return *thread_streams.at(tid);
}

void lava_writer::new_frame()
{
	frame_mutex.lock();

	// inform all workers
	debug_info d;
	for (unsigned i = 0; i < thread_streams.size(); i++)
	{
		d += thread_streams.at(i)->new_frame(global_frame);
	}
	debug.push_back(d);

	global_frame++;
	frame_mutex.unlock();
}
