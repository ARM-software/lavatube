#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstring>
#include <errno.h>
#include <unistd.h>

#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include <sys/system_properties.h>
#endif

#include "write.h"
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

static void append_sorted_strings(Json::Value& array, const std::unordered_set<std::string>& values)
{
	std::vector<std::string> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	for (const std::string& value : sorted) array.append(value);
}

static std::string trim_extension_name(const std::string& value)
{
	size_t first = 0;
	while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) first++;
	size_t last = value.size();
	while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) last--;
	return value.substr(first, last - first);
}

static bool valid_extension_name(const std::string& name)
{
	if (name.size() < 6 || name.compare(0, 3, "VK_") != 0) return false;
	const size_t vendor_end = name.find('_', 3);
	if (vendor_end == std::string::npos || vendor_end == 3 || vendor_end + 1 == name.size()) return false;
	for (size_t i = 3; i < vendor_end; i++)
	{
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if (!std::isupper(c) && !std::isdigit(c)) return false;
	}
	for (size_t i = vendor_end + 1; i < name.size(); i++)
	{
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if (!std::isalnum(c) && c != '_') return false;
	}
	return true;
}

static void parse_blacklisted_extensions(trace_metadata& meta) REQUIRES(frame_mutex)
{
	const char* value = getenv("LAVATUBE_BLACKLIST_EXTENSIONS");
	if (!value) return;

	const std::string list(value);
	size_t start = 0;
	while (start <= list.size())
	{
		const size_t comma = list.find(',', start);
		const std::string name = trim_extension_name(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (!valid_extension_name(name)) DIE("Invalid Vulkan extension name in LAVATUBE_BLACKLIST_EXTENSIONS: '%s'", name.c_str());
		meta.blacklisted_extensions.insert(name);
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
}

static void write_removed_strings(Json::Value& parent, const char* key, const std::unordered_set<std::string>& values)
{
	parent[key] = Json::arrayValue;
	append_sorted_strings(parent[key], values);
}

static void rewrite_enabled_extensions(Json::Value& target, const Json::Value& original, const std::unordered_set<std::string>& enabled)
{
	target = Json::arrayValue;

	std::unordered_set<std::string> seen;
	if (original.isArray())
	{
		for (const Json::Value& value : original)
		{
			const std::string name = value.asString();
			if (enabled.count(name) == 0 || seen.count(name)) continue;
			target.append(name);
			seen.insert(name);
		}
	}

	std::unordered_set<std::string> remainder = enabled;
	for (const std::string& name : seen) remainder.erase(name);
	append_sorted_strings(target, remainder);
}

static void log_removed_strings(const char* heading, const std::unordered_set<std::string>& values)
{
	if (values.empty()) return;
	ILOG("%s", heading);
	std::vector<std::string> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	for (const std::string& value : sorted) ILOG("\t%s", value.c_str());
}

static void merge_tracking_field(Json::Value& dst, const Json::Value& src, const char* field)
{
	if (!dst.isObject() || !src.isObject()) return;
	for (const std::string& type_name : dst.getMemberNames())
	{
		if (!src.isMember(type_name)) continue;
		Json::Value& dst_array = dst[type_name];
		const Json::Value& src_array = src[type_name];
		if (!dst_array.isArray() || !src_array.isArray()) continue;
		for (Json::ArrayIndex i = 0; i < dst_array.size(); i++)
		{
			Json::Value& dst_value = dst_array[i];
			if (!dst_value.isObject() || !dst_value.isMember("index")) continue;
			const Json::ArrayIndex index = dst_value["index"].asUInt();
			if (index >= src_array.size()) continue;
			const Json::Value& src_value = src_array[index];
			if (!src_value.isObject() || !src_value.isMember("index") || src_value["index"].asUInt() != index) continue;
			if (!src_value.isMember(field)) continue;
			dst_value[field] = src_value[field];
		}
	}
}

static bool has_suffix(const std::string& value, const char* suffix)
{
	const size_t suffix_len = strlen(suffix);
	return value.size() >= suffix_len && value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

static const char* trace_pack_suffix()
{
	return ".api";
}

static std::string trace_pack_path(const std::string& path, bool explicit_output)
{
	if (has_suffix(path, ".api")) return path;
	if (explicit_output) return path + trace_pack_suffix();
	return path + trace_pack_suffix();
}

// --- trace file writer

lava_file_writer::lava_file_writer(uint16_t _tid, lava_writer* _parent) : file_writer(_tid), parent(_parent)
{
	current.thread = _tid;
	current.packet = 0;
	run = parent->run;
	write_output = parent->write_output;
	get_thread_name(thread_name);
	if (p__disable_multithread_compress) disable_multithreaded_compress();
	if (p__disable_multithread_writeout) disable_multithreaded_writeout();
}

void lava_file_writer::set(const std::string& path)
{
	assert(mPath.empty());
	std::string fname = path + "/thread_" + _to_string((unsigned)current.thread) + ".bin";
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
	begin_packet(PACKET_THREAD_BARRIER);
	int size = parent->thread_streams.size();
	write_uint8_t(size); // threads to sync
	for (int i = 0; i < size; i++)
	{
		const uint32_t packet_index = parent->thread_streams.at(i)->current.packet;
		assert(packet_index != UINT32_MAX);
		write_uint32_t(packet_index);
	}
	DLOG2("Injected thread barrier on thread %d with %d targets", thread_index(), size);
	end_packet();
}

lava_file_writer::~lava_file_writer()
{
	end_packet();
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
		k["packet"] = frame.packet_index;
		v["frames"].append(k);
		highest = std::max(highest, frame.global_frame);
	}
	v["uncompressed_sizes"] = Json::arrayValue;
	v["compressed_sizes"] = Json::arrayValue;
	for (const auto i : uncompressed_sizes) v["uncompressed_sizes"].append((Json::Value::UInt64)i);
	for (const auto i : compressed_sizes) v["compressed_sizes"].append((Json::Value::UInt64)i);
	DLOG("Wrapping up thread %u with %d frames", current.thread, highest);
	v["highest_global_frame"] = highest;
	const std::string path = mPath + "/frames_" + _to_string((unsigned)current.thread) + ".json";
	write_json(path, v);
}

void lava_file_writer::new_frame(int global_frame)
{
	framedata data;
	data.start_pos = uncompressed_bytes;
	data.packet_index = current.packet;
	data.global_frame = global_frame;
	data.local_frame = current.frame;
	frames.push_back(data);
	assert(global_frame >= (int)current.frame);
	current.frame++;
	self_test();
}

void lava_file_writer::begin_packet(uint8_t type)
{
	end_packet();
	current.packet_type = type;
	if (type != PACKET_VULKAN_API_CALL) current.call_id = UINT16_MAX;
	packet_start = uncompressed_bytes;
	write_uint8_t(type);
	packet_size = write_later_uint32_t(0);
	packet_open = true;
}

void lava_file_writer::end_packet()
{
	if (!packet_open) return;
	assert(packet_size);
	const uint64_t size = uncompressed_bytes - packet_start;
	if (size > UINT32_MAX)
	{
		ABORT("Packet on thread %u is too large: %lu bytes", (unsigned)current.thread, (unsigned long)size);
	}
	*packet_size = (uint32_t)size;
	packet_size = nullptr;
	packet_open = false;
	thaw();
	current.packet++;
}

void lava_file_writer::write_raw_packet(const char* data, uint32_t size)
{
	assert(data);
	assert(size >= sizeof(uint8_t) + sizeof(uint32_t));
	end_packet();
	write_array(data, size);
	current.packet++;
}

// --- trace writer

static lava_writer _instance;

lava_writer& lava_writer::instance()
{
	return _instance;
}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
void lava_writer::start_android_finish_monitor()
{
	if (android_finish_monitor_running.load(std::memory_order_acquire))
	{
		return;
	}
	if (android_finish_monitor_thread.joinable())
	{
		if (android_finish_monitor_thread.get_id() == std::this_thread::get_id())
		{
			android_finish_monitor_thread.detach();
		}
		else
		{
			android_finish_monitor_thread.join();
		}
	}
	android_finish_monitor_running.store(true, std::memory_order_release);

	android_finish_monitor_thread = std::thread([this]()
	{
		while (android_finish_monitor_running.load(std::memory_order_acquire))
		{
			char value[PROP_VALUE_MAX] = {};
			if (__system_property_get("debug.vulkan.lavatube.finish", value) > 0 && strcmp(value, "1") == 0)
			{
				bool do_finish = false;
				{
					lava::lock_guard lock(frame_mutex);
					do_finish = should_serialize;
				}
				if (do_finish)
				{
					ILOG("Android finish property set, serializing trace");
					serialize();
					finish();
				}
				android_finish_monitor_running.store(false, std::memory_order_release);
				return;
			}
			usleep(200000);
		}
	});
}
#endif

void lava_writer::set(const std::string& path)
{
	mPath = path + "_tmp";
	mPack = trace_pack_path(path, false);
	write_output = false;
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
		thread_streams.at(i)->write_output = false;
		thread_streams.at(i)->set(mPath);
	}
	frame_mutex.unlock();

	should_serialize = true;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	start_android_finish_monitor();
#endif
}

void lava_writer::set_output(const std::string& packed_path)
{
	mPack = trace_pack_path(packed_path, true);
	mPath = mPack + "_tmp";
	write_output = true;
	ILOG("Output path is set to %s", mPack.c_str());

	if (access(mPath.c_str(), F_OK) == 0)
	{
		erase_directory(mPath);
	}

	int result = mkdir(mPath.c_str(), 0755);
	if (result != 0)
	{
		ELOG("Failed to create \"%s\": %s", mPath.c_str(), strerror(errno));
	}

	frame_mutex.lock();
	for (unsigned i = 0; i < thread_streams.size(); i++)
	{
		thread_streams.at(i)->write_output = true;
		thread_streams.at(i)->set(mPath);
	}
	frame_mutex.unlock();

	should_serialize = true;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	start_android_finish_monitor();
#endif
}

lava_writer::lava_writer() : global_frame(0)
{
	frame_mutex.lock();
	parse_blacklisted_extensions(meta);

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
	mJson["blacklistedExtensions"] = Json::arrayValue;
	append_sorted_strings(mJson["blacklistedExtensions"], meta.blacklisted_extensions);

	frame_mutex.unlock();
}

lava_writer::~lava_writer()
{
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	android_finish_monitor_running.store(false, std::memory_order_release);
	if (android_finish_monitor_thread.joinable())
	{
		if (android_finish_monitor_thread.get_id() == std::this_thread::get_id())
		{
			android_finish_monitor_thread.detach();
		}
		else
		{
			android_finish_monitor_thread.join();
		}
	}
#endif
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
	lava::lock_guard lock(frame_mutex);
	assert(!mPath.empty());

	// write dictionary to JSON file
	std::string dict_path = mPath + "/dictionary.json";
	Json::Value jd;
	for (const auto& pair : records.function_table)
	{
		jd[pair.first] = (unsigned)pair.second;
	}
	write_json(dict_path, jd);

	// over-write these in case something was not used
	feature_detection* f = vulkan_feature_detection_get();
	std::unordered_set<std::string> removed_features10;
	std::unordered_set<std::string> removed_features11;
	std::unordered_set<std::string> removed_features12;
	std::unordered_set<std::string> removed_features13;
	std::unordered_set<std::string> removed_features14;
	if (meta.app.stored_VkPhysicalDeviceFeatures2) removed_features10 = f->adjust_VkPhysicalDeviceFeatures(meta.app.stored_VkPhysicalDeviceFeatures2->features);
	if (meta.app.stored_VkPhysicalDeviceVulkan11Features) removed_features11 = f->adjust_VkPhysicalDeviceVulkan11Features(*meta.app.stored_VkPhysicalDeviceVulkan11Features);
	if (meta.app.stored_VkPhysicalDeviceVulkan12Features) removed_features12 = f->adjust_VkPhysicalDeviceVulkan12Features(*meta.app.stored_VkPhysicalDeviceVulkan12Features);
	if (meta.app.stored_VkPhysicalDeviceVulkan13Features) removed_features13 = f->adjust_VkPhysicalDeviceVulkan13Features(*meta.app.stored_VkPhysicalDeviceVulkan13Features);
	if (meta.app.stored_VkPhysicalDeviceVulkan14Features) removed_features14 = f->adjust_VkPhysicalDeviceVulkan14Features(*meta.app.stored_VkPhysicalDeviceVulkan14Features);
	auto removed_device_exts = f->adjust_device_extensions(meta.app.device_extensions);
	auto removed_instance_exts = f->adjust_instance_extensions(meta.app.instance_extensions);
	Json::Value& r = mJson;
	Json::Value instance_requested_extensions = r["instanceRequested"]["enabledExtensions"];
	Json::Value device_requested_extensions = r["deviceRequested"]["enabledExtensions"];
	rewrite_enabled_extensions(r["instanceRequested"]["enabledExtensions"], instance_requested_extensions, meta.app.instance_extensions);
	rewrite_enabled_extensions(r["deviceRequested"]["enabledExtensions"], device_requested_extensions, meta.app.device_extensions);
	write_removed_strings(r["instanceRequested"], "removedExtensions", removed_instance_exts);
	write_removed_strings(r["deviceRequested"], "removedExtensions", removed_device_exts);
	r["deviceRequested"]["removedFeatures"] = Json::objectValue;
	if (meta.app.stored_VkPhysicalDeviceFeatures2)
	{
		r["deviceRequested"]["VkPhysicalDeviceFeatures"] = writeVkPhysicalDeviceFeatures2(*meta.app.stored_VkPhysicalDeviceFeatures2);
		write_removed_strings(r["deviceRequested"]["removedFeatures"], "VkPhysicalDeviceFeatures", removed_features10);
	}
	if (meta.app.stored_VkPhysicalDeviceVulkan11Features)
	{
		r["deviceRequested"]["VkPhysicalDeviceVulkan11Features"] = writeVkPhysicalDeviceVulkan11Features(*meta.app.stored_VkPhysicalDeviceVulkan11Features);
		write_removed_strings(r["deviceRequested"]["removedFeatures"], "VkPhysicalDeviceVulkan11Features", removed_features11);
	}
	if (meta.app.stored_VkPhysicalDeviceVulkan12Features)
	{
		r["deviceRequested"]["VkPhysicalDeviceVulkan12Features"] = writeVkPhysicalDeviceVulkan12Features(*meta.app.stored_VkPhysicalDeviceVulkan12Features);
		write_removed_strings(r["deviceRequested"]["removedFeatures"], "VkPhysicalDeviceVulkan12Features", removed_features12);
	}
	if (meta.app.stored_VkPhysicalDeviceVulkan13Features)
	{
		r["deviceRequested"]["VkPhysicalDeviceVulkan13Features"] = writeVkPhysicalDeviceVulkan13Features(*meta.app.stored_VkPhysicalDeviceVulkan13Features);
		write_removed_strings(r["deviceRequested"]["removedFeatures"], "VkPhysicalDeviceVulkan13Features", removed_features13);
	}
	if (meta.app.stored_VkPhysicalDeviceVulkan14Features)
	{
		r["deviceRequested"]["VkPhysicalDeviceVulkan14Features"] = writeVkPhysicalDeviceVulkan14Features(*meta.app.stored_VkPhysicalDeviceVulkan14Features);
		write_removed_strings(r["deviceRequested"]["removedFeatures"], "VkPhysicalDeviceVulkan14Features", removed_features14);
	}
	log_removed_strings("Feature detection removed unused instance extensions:", removed_instance_exts);
	log_removed_strings("Feature detection removed unused device extensions:", removed_device_exts);
	log_removed_strings("Feature detection removed unused device features from VkPhysicalDeviceFeatures:", removed_features10);
	log_removed_strings("Feature detection removed unused device features from VkPhysicalDeviceVulkan11Features:", removed_features11);
	log_removed_strings("Feature detection removed unused device features from VkPhysicalDeviceVulkan12Features:", removed_features12);
	log_removed_strings("Feature detection removed unused device features from VkPhysicalDeviceVulkan13Features:", removed_features13);
	log_removed_strings("Feature detection removed unused device features from VkPhysicalDeviceVulkan14Features:", removed_features14);

	// write metadata to JSON file
	mJson["vulkan_header_version"] = version_to_string(VK_HEADER_VERSION);
	mJson["global_frames"] = global_frame + 1; // +1 since zero-indexed
	mJson["threads"] = (unsigned)thread_streams.size();
	write_json(mPath + "/metadata.json", mJson);

	// write limits
	write_json(mPath + "/limits.json", trace_limits(this));

	// write out tracking info for each object
	Json::Value tracking = trackable_json(this);
	if (write_output)
	{
		merge_tracking_field(tracking, mInputTracking, "updates");
		merge_tracking_field(tracking, mInputTracking, "written");
	}
	write_json(mPath + "/tracking.json", tracking);

}

void lava_writer::finish()
{
	lava::lock_guard lock(frame_mutex);
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	android_finish_monitor_running.store(false, std::memory_order_release);
#endif

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
		if (p__delete_empty_trace && records.VkDevice_index.size() == 0)
		{
			ILOG("No device was created; deleting empty trace %s", mPack.c_str());
			erase_directory(mPath);
		}
		else if (!pack_directory(mPack, mPath, true))
		{
			ELOG("Failed to pack result files!");
		}
	}
	mPath = "";
	mJson = Json::Value();
	mInputTracking = Json::Value();
	global_frame.exchange(0);
	tid = -1;
	vulkan_feature_detection_reset();
}

void lava_writer::make_writer(unsigned index)
{
	lava::lock_guard lock(frame_mutex);
	if (index == UINT32_MAX)
	{
		index = thread_streams.size();
	}
	assert(index == thread_streams.size());
	tid = index;
	lava_file_writer* f = new lava_file_writer(index, this);
	if (!mPath.empty())
	{
		f->set(mPath);
	}
	f->inject_thread_barrier();
	thread_streams.emplace_back(std::move(f));
	DLOG("Created thread %d, currently %d threads", (int)index, (int)thread_streams.size());
}

void lava_writer::bind_thread(unsigned index)
{
	assert(index < thread_streams.size());
	tid = index;
	thread_streams.at(index)->capture_thread_name();
}

void lava_writer::prepare_threads(unsigned count)
{
	while (thread_streams.size() < count)
	{
		make_writer(thread_streams.size());
	}
}

lava_file_writer& lava_writer::file_writer()
{
	if (tid == -1) // this thread does not yet have its own lava_file_writer, so create one
	{
		make_writer();
	}
	return *thread_streams.at(tid);
}

// This function is NOT thread-safe! Only use when threads have been serialized
lava_file_writer& lava_writer::file_writer(unsigned index)
{
	assert(index <= thread_streams.size());
	if (index == thread_streams.size()) // this thread does not yet have its own lava_file_writer, so create one
	{
		make_writer();
	}
	return *thread_streams.at(index);
}

void lava_writer::new_frame()
{
	frame_mutex.lock();

	// inform all workers
	for (unsigned i = 0; i < thread_streams.size(); i++)
	{
		thread_streams.at(i)->new_frame(global_frame);
	}

	global_frame++;
	frame_mutex.unlock();
}
