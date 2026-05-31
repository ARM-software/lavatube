#include <fstream>
#include <errno.h>
#include <algorithm>

#include "lavatube.h"
#include "read.h"
#include "packfile.h"
#include "jsoncpp/json/reader.h"
#include "jsoncpp/json/writer.h"
#include "json_helpers.h"
#include "read_auto.h"
#include "util_auto.h"
#include "suballocator.h"
#include "markings.h"

/// Mutex to enforce additional external synchronization
lava::mutex sync_mutex;

bool same_change_source(const change_source& a, const change_source& b)
{
	return a.call == b.call && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
}

static bool same_rewrite_target(const address_rewrite& entry, VkObjectType object_type, uint32_t object_index, uint32_t stage_index)
{
	return entry.object_type == object_type && entry.object_index == object_index && entry.stage_index == stage_index;
}

void merge_rewrite_markings(std::list<address_rewrite>& queue, const change_source& source, const VkMarkedOffsetsARM* markings)
{
	merge_rewrite_markings(queue, source, markings, VK_OBJECT_TYPE_UNKNOWN, CONTAINER_NULL_VALUE, CONTAINER_NULL_VALUE);
}

void merge_rewrite_markings(std::list<address_rewrite>& queue, const change_source& source, const VkMarkedOffsetsARM* markings, VkObjectType object_type, uint32_t object_index)
{
	merge_rewrite_markings(queue, source, markings, object_type, object_index, CONTAINER_NULL_VALUE);
}

void merge_rewrite_markings(std::list<address_rewrite>& queue, const change_source& source, const VkMarkedOffsetsARM* markings, VkObjectType object_type, uint32_t object_index, uint32_t stage_index)
{
	assert(markings);
	auto it = std::find_if(queue.begin(), queue.end(), [&](const address_rewrite& entry)
	{
		return same_change_source(entry.source, source) && same_rewrite_target(entry, object_type, object_index, stage_index);
	});
	if (it == queue.end())
	{
		address_rewrite entry;
		entry.markings = clone_marked_offsets(markings);
		normalize_marked_offsets(entry.markings);
		entry.source = source;
		entry.object_type = object_type;
		entry.object_index = object_index;
		entry.stage_index = stage_index;
		queue.push_back(entry);
		return;
	}

	VkMarkedOffsetsARM* merged = merge_marked_offsets(it->markings, markings);
	free_marked_offsets(it->markings);
	it->markings = merged;
}

static void load_requested_extensions(const Json::Value& root, bool& has_extensions, std::vector<std::string>& extensions)
{
	extensions.clear();
	has_extensions = root.isArray();
	if (!has_extensions) return;

	extensions.reserve(root.size());
	for (const auto& ext : root)
	{
		extensions.push_back(ext.asString());
	}
}

// --- file reader

lava_file_reader::lava_file_reader(lava_reader* _parent, const std::string& path, int mytid, int frames, const Json::Value& frameinfo, size_t uncompressed_size, size_t uncompressed_target, int start, int end)
	: file_reader(packed_open("thread_" + std::to_string(mytid) + ".bin", path), mytid, uncompressed_size, uncompressed_target, start == 0)
{
	parent = _parent;
	run = parent->run;
	write_output = parent->write_output;
	global_frames = frames;
	current.thread = mytid;
	current.call = 0;
	current.frame = 0;
	current.thread = mytid;
	memset(trace_thread_name, 0, sizeof(trace_thread_name));

	if (frameinfo.isMember("thread_name"))
	{
		strncpy(trace_thread_name, frameinfo["thread_name"].asString().c_str(), sizeof(trace_thread_name) - 1);
		set_thread_name(trace_thread_name);
	}

	// Translate global frames to local frames and set our measurement window
	mUseFrameRange = end != -1;
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
	if (parent->stop_requested())
	{
		terminated.store(true);
		return 0;
	}
	if (file_reader::done())
	{
		terminated.store(true);
		return 0; // done
	}
	release_checkpoint();
	const uint8_t r = read_uint8_t();
	assert(r != 0); // invalid value for instrtype
	current.packet_type = r;
	if (r != PACKET_VULKAN_API_CALL) current.call_id = UINT16_MAX;
	printed_current_packet = false;
	print_packet_frame = current.frame;
	return r;
}

void lava_file_reader::note_markings(const VkMarkedOffsetsARM* markings)
{
	if (!parent->markings_observer || !markings) return;
	parent->markings_observer(current, markings, parent->markings_observer_data);
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
	current.packet_type = PACKET_VULKAN_API_CALL;
	// replay_stop_requested may unwind out of this call before the normal per-call epilogue runs.
	func(*this);
	if (parent->print_packets && !printed_current_packet)
	{
		callback_context cb_context{ *this };
		print_params_unavailable(cb_context);
	}
	current.call++;
	parent->thread_call_numbers->at(current.thread).fetch_add(1, std::memory_order_relaxed);
	pool.reset();
	return apicall;
}

Json::Value cli_params_base_json(const callback_context& cb)
{
	cb.params_attachment_index = 0;
	Json::Value v = from_change_source(cb.reader.current);
	return v;
}

Json::Value cli_params_attachment(const callback_context& cb)
{
	return Json::Value("[attachment " + std::to_string(cb.params_attachment_index++) + "]");
}

static Json::Value params_unavailable_json(const callback_context& cb)
{
	Json::Value v = cli_params_base_json(cb);
	v["parameters"]["TODO"] = "parameter serialization is not implemented for this hardcoded replay path";
	return v;
}

static Json::Value params_packet_json(const callback_context& cb)
{
	Json::Value v = cli_params_base_json(cb);
	Json::Value params;
	const output_update_packet& update = cb.reader.current_update_packet;
	if (update.valid)
	{
		params["packet_type"] = update.instrtype;
		params["device_index"] = update.device_index;
		params["object_index"] = update.object_index;
		params["size"] = (Json::UInt64)update.size;
		params["pNext"] = json_extension(cb, update.sptr);
	}
	else if (cb.reader.current.packet_type == PACKET_THREAD_BARRIER)
	{
		params["TODO"] = "thread barrier packet parameters are not retained after replaying the packet";
	}
	else
	{
		params["TODO"] = "packet parameter serialization not implemented";
	}
	v["parameters"] = params;
	return v;
}

void cli_params_publish(callback_context& cb, Json::Value v)
{
	lava_reader* parent = cb.reader.parent;
	parent->cli_response = v.toStyledString();
	if (parent->cli_response.empty() || parent->cli_response.back() != '\n') parent->cli_response += "\n";
	parent->cli_params_ready.store(true, std::memory_order_release);
	parent->cli_params_ready.notify_all();
}

void cli_params_unavailable(callback_context& cb)
{
	lava_reader* parent = cb.reader.parent;
	if (!parent->cli_params_requested.exchange(false, std::memory_order_acq_rel)) return;
	cli_params_publish(cb, params_unavailable_json(cb));
}

void cli_params_packet(callback_context& cb)
{
	lava_reader* parent = cb.reader.parent;
	if (!parent->cli_params_requested.exchange(false, std::memory_order_acq_rel)) return;
	cli_params_publish(cb, params_packet_json(cb));
}

void print_params_publish(callback_context& cb, Json::Value v)
{
	if (!cb.reader.parent->is_frame_selected(cb.reader.print_packet_frame))
	{
		cb.reader.printed_current_packet = true;
		return;
	}
	v["frame"] = cb.reader.print_packet_frame;
	Json::FastWriter writer;
	const std::string out = writer.write(v);
	lava::lock_guard lock(cb.reader.parent->print_mutex);
	printf("%s", out.c_str());
	cb.reader.printed_current_packet = true;
}

void print_params_unavailable(callback_context& cb)
{
	print_params_publish(cb, params_unavailable_json(cb));
}

void print_params_packet(callback_context& cb)
{
	print_params_publish(cb, params_packet_json(cb));
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

void lava_reader::finalize()
{
	const double total_time_ms = ((gettime() - mStartTime.load()) / 1000000UL);
	const double fps = (total_time_ms > 0.0) ? ((double)global_frame_count / (total_time_ms / 1000.0)) : 0.0;
	ILOG("==== %.2f ms, %u frames (%.2f fps) ====", total_time_ms, global_frame_count, fps);
	Json::Value out;
	out["fps"] = fps;
	out["frames"] = global_frame_count;
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
	uint64_t process_time = 0;
	if (timespec_less(&stop_process_cpu_usage, &process_cpu_usage))
	{
		ELOG("Failed to measure process CPU usage: stop time precedes start time");
	}
	else
	{
		process_time = diff_timespec(&stop_process_cpu_usage, &process_cpu_usage);
	}
	ILOG("CPU time spent in ms - readahead workers %lu, API runners %lu, full process %lu", (long unsigned)worker, (long unsigned)runner, (long unsigned)process_time);
	out["readahead_workers_time"] = worker;
	out["api_runners_time"] = runner;
	out["process_time"] = process_time;
	if (out_fptr)
	{
		write_json(out_fptr, out);
		fclose(out_fptr);
		out_fptr = nullptr;
	}
}

bool lava_reader::cleanup_after_stop()
{
	if (!stop_requested() || !run || thread_streams.empty()) return false;
	const VkDevice cleanup_device = reinterpret_cast<VkDevice>(mCleanupDevice.load(std::memory_order_acquire));
	if (cleanup_device == VK_NULL_HANDLE) return false;
	wrap_vkDeviceWaitIdle(cleanup_device);
	terminate_all(*thread_streams.front(), cleanup_device);
	return true;
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
	global_frame_count = meta["global_frames"].asInt();
	stored_version_major = meta["lavatube_version_major"].asInt();
	stored_version_minor = meta["lavatube_version_minor"].asInt();
	stored_version_patch = meta["lavatube_version_patch"].asInt();
	load_requested_extensions(meta["instanceRequested"]["enabledExtensions"], has_stored_instance_requested_extensions, stored_instance_requested_extensions);
	load_requested_extensions(meta["deviceRequested"]["enabledExtensions"], has_stored_device_requested_extensions, stored_device_requested_extensions);

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
		thread_streams[thread_id] = new lava_file_reader(this, mPackedFile, thread_id, global_frame_count, frameinfo, uncompressed_size, uncompressed_target, mStart, mEnd);
	}

	if (create_results_file)
	{
		out_fptr = fopen("lavaresults.json", "w");
		if (!out_fptr) ABORT("Failed to open results file: %s", strerror(errno));
	}
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
