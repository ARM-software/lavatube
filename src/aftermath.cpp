#include "aftermath.h"

#include "util.h"

#ifdef LAVATUBE_USE_NSIGHT_AFTERMATH

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

struct aftermath_context
{
	std::mutex mutex;
	std::string trace_filename;
	std::atomic_uint dump_count{ 0 };
	std::atomic_uint shader_debug_count{ 0 };
	std::unordered_map<const void*, std::string> markers;
};

static bool aftermath_result_ok(GFSDK_Aftermath_Result result, const char* operation)
{
	if (GFSDK_Aftermath_SUCCEED(result)) return true;
	ELOG("Nsight Aftermath %s failed: 0x%x", operation, result);
	return false;
}

static bool aftermath_write_file(const std::string& filename, const void* data, size_t size)
{
	FILE* file = fopen(filename.c_str(), "wb");
	if (!file)
	{
		ELOG("Failed to open Nsight Aftermath output %s: %s", filename.c_str(), strerror(errno));
		return false;
	}
	const size_t written = fwrite(data, 1, size, file);
	const int close_result = fclose(file);
	if (written != size || close_result != 0)
	{
		ELOG("Failed to write Nsight Aftermath output %s", filename.c_str());
		return false;
	}
	return true;
}

static std::string aftermath_output_name(unsigned index, const char* suffix)
{
	return "lava-replay-aftermath-" + std::to_string(getpid()) + "-" + std::to_string(index) + suffix;
}

static void aftermath_decode_dump(aftermath_context* context, const void* dump, uint32_t dump_size,
	const std::string& dump_filename)
{
	GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
	GFSDK_Aftermath_Result result = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
		GFSDK_Aftermath_Version_API, dump, dump_size, &decoder);
	if (!aftermath_result_ok(result, "decoder creation")) return;

	uint32_t json_size = 0;
	result = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(decoder,
		GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
		GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
		nullptr, nullptr, nullptr, context, &json_size);
	if (aftermath_result_ok(result, "JSON generation") && json_size > 0)
	{
		std::vector<char> json(json_size);
		result = GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, json_size, json.data());
		if (aftermath_result_ok(result, "JSON retrieval"))
		{
			const size_t output_size = json.back() == '\0' ? json.size() - 1 : json.size();
			const std::string json_filename = dump_filename + ".json";
			if (aftermath_write_file(json_filename, json.data(), output_size))
			{
				ELOG("Wrote decoded Nsight Aftermath GPU crash dump to %s", json_filename.c_str());
			}
		}
	}
	aftermath_result_ok(GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder), "decoder destruction");
}

static void GFSDK_AFTERMATH_CALL aftermath_crash_dump_callback(const void* dump, uint32_t dump_size, void* user_data)
{
	aftermath_context* context = static_cast<aftermath_context*>(user_data);
	if (!context || !dump || dump_size == 0) return;
	const unsigned index = context->dump_count.fetch_add(1, std::memory_order_relaxed) + 1;
	const std::string filename = aftermath_output_name(index, ".nv-gpudmp");
	if (aftermath_write_file(filename, dump, dump_size))
	{
		ELOG("Wrote Nsight Aftermath GPU crash dump to %s", filename.c_str());
	}
	aftermath_decode_dump(context, dump, dump_size, filename);
}

static void GFSDK_AFTERMATH_CALL aftermath_shader_debug_callback(const void* data, uint32_t size, void* user_data)
{
	aftermath_context* context = static_cast<aftermath_context*>(user_data);
	if (!context || !data || size == 0) return;
	std::lock_guard<std::mutex> lock(context->mutex);
	const unsigned index = context->shader_debug_count.fetch_add(1, std::memory_order_relaxed) + 1;
	const std::string filename = aftermath_output_name(index, ".nvdbg");
	if (aftermath_write_file(filename, data, size))
	{
		DLOG("Wrote Nsight Aftermath shader debug information to %s", filename.c_str());
	}
}

static void GFSDK_AFTERMATH_CALL aftermath_description_callback(
	PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add_description, void* user_data)
{
	aftermath_context* context = static_cast<aftermath_context*>(user_data);
	add_description(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "lava-replay");
	if (context)
	{
		add_description(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_UserDefined, context->trace_filename.c_str());
	}
}

static void GFSDK_AFTERMATH_CALL aftermath_resolve_marker_callback(const void* marker_data, uint32_t marker_data_size,
	void* user_data, PFN_GFSDK_Aftermath_ResolveMarker resolve_marker)
{
	(void)marker_data_size;
	aftermath_context* context = static_cast<aftermath_context*>(user_data);
	if (!context || !marker_data || !resolve_marker) return;
	std::lock_guard<std::mutex> lock(context->mutex);
	const auto marker = context->markers.find(marker_data);
	if (marker == context->markers.end()) return;
	resolve_marker(marker->second.data(), (uint32_t)marker->second.size());
}

void* aftermath_initialize(const char* trace_filename)
{
	aftermath_context* context = new aftermath_context;
	context->trace_filename = trace_filename ? trace_filename : "";
	const GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
		GFSDK_Aftermath_Version_API,
		GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
		GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
		aftermath_crash_dump_callback,
		aftermath_shader_debug_callback,
		aftermath_description_callback,
		aftermath_resolve_marker_callback,
		context);
	if (!aftermath_result_ok(result, "initialization"))
	{
		delete context;
		return nullptr;
	}
	ILOG("Enabled NVIDIA Nsight Aftermath GPU crash dump collection");
	return context;
}

void aftermath_register_marker(void* opaque_context, const void* marker, const char* label)
{
	aftermath_context* context = static_cast<aftermath_context*>(opaque_context);
	if (!context || !marker || !label) return;
	std::lock_guard<std::mutex> lock(context->mutex);
	context->markers[marker] = label;
}

void aftermath_handle_device_lost(void* opaque_context)
{
	if (!opaque_context) return;
	GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
	if (!aftermath_result_ok(GFSDK_Aftermath_GetCrashDumpStatus(&status), "status query")) return;
	unsigned elapsed_ms = 0;
	while (status != GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed &&
		status != GFSDK_Aftermath_CrashDump_Status_Finished && elapsed_ms < 5000)
	{
		usleep(50000);
		elapsed_ms += 50;
		if (!aftermath_result_ok(GFSDK_Aftermath_GetCrashDumpStatus(&status), "status query")) return;
	}
	if (status == GFSDK_Aftermath_CrashDump_Status_Finished)
	{
		ILOG("Nsight Aftermath finished processing the GPU crash dump");
	}
	else
	{
		ELOG("Nsight Aftermath GPU crash dump processing did not finish: status=%u", (unsigned)status);
	}
}

void aftermath_shutdown(void* opaque_context)
{
	aftermath_context* context = static_cast<aftermath_context*>(opaque_context);
	if (!context) return;
	aftermath_result_ok(GFSDK_Aftermath_DisableGpuCrashDumps(), "shutdown");
	delete context;
}

#else

void* aftermath_initialize(const char* trace_filename)
{
	(void)trace_filename;
	ILOG("Nsight Aftermath is unavailable; set NSIGHT_AFTERMATH_SDK_ROOT when configuring to enable it");
	return nullptr;
}

void aftermath_handle_device_lost(void* context)
{
	(void)context;
}

void aftermath_register_marker(void* context, const void* marker, const char* label)
{
	(void)context;
	(void)marker;
	(void)label;
}

void aftermath_shutdown(void* context)
{
	(void)context;
}

#endif
