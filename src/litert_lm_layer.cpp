#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cstring>
#include <dlfcn.h>

#include "write.h"
#include "litert_lm_auto.h"

static std::string model_basename(const char* model_path)
{
	if (!model_path) return std::string();
	const char* slash = strrchr(model_path, '/');
	const char* backslash = strrchr(model_path, '\\');
	const char* base = model_path;
	if (slash && backslash)
	{
		base = (slash > backslash ? slash : backslash) + 1;
	}
	else if (slash)
	{
		base = slash + 1;
	}
	else if (backslash)
	{
		base = backslash + 1;
	}
	return std::string(base);
}

void ensure_litert_lm_dispatch(litert_lm_layer_runtime_state& state)
{
	if (state.initialized.load(std::memory_order_acquire)) return;

	lava::lock_guard lock(frame_mutex);
	if (state.initialized.load(std::memory_order_relaxed)) return;

	if (!state.library_handle)
	{
		const char* custom_lib = getenv("LAVATUBE_LITERT_LM_LIB");
		if (custom_lib && *custom_lib)
		{
			state.library_handle = dlopen(custom_lib, RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state.library_handle)
		{
			state.library_handle = dlopen("/opt/litert-lm/lib/liblitert-lm.so", RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state.library_handle)
		{
			state.library_handle = dlopen("liblitert-lm_real.so", RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state.library_handle)
		{
			state.library_handle = dlopen("liblitert-lm.so", RTLD_LAZY | RTLD_LOCAL);
		}
	}

	resolve_all_litert_lm_dispatch(&state.target_dispatch, state.library_handle);

	if (!state.target_dispatch.engine_settings_create || !state.target_dispatch.engine_create)
	{
		ABORT("Failed to resolve LiteRT-LM runtime symbols");
	}

	state.initialized.store(true, std::memory_order_release);
}

static lava_file_writer& begin_litert_lm_packet(lava_writer& instance, litert_lm_layer_runtime_state& state,
	litert_lm_function_id function_id)
{
	instance.ensure_started(get_trace_path("litert_lm"));
	lava_file_writer& writer = instance.file_writer();
	writer.begin_packet(PACKET_LITERT_LM_API_CALL);
	writer.write_uint16_t(LITERT_LM_DICTIONARY_BASE + function_id);
	writer.write_uint32_t(0);
	writer.current.call_id = function_id;
	writer.current.frame = instance.global_frame.load(std::memory_order_relaxed);
	state.api_calls.fetch_add(1, std::memory_order_relaxed);
	return writer;
}

template<typename T, typename U>
static uint32_t lookup_litert_lm_index(trace_remap<T, U>& remap, T handle, const change_source& current)
{
	if (!handle) return CONTAINER_NULL_VALUE;
	if (!remap.contains(handle)) return CONTAINER_INVALID_INDEX;
	U* data = remap.at(handle);
	data->last_modified = current;
	return data->index;
}

template<typename T, typename U>
static uint32_t destroy_litert_lm_handle(trace_remap<T, U>& remap, T handle, const change_source& current)
{
	if (!handle) return CONTAINER_NULL_VALUE;
	if (!remap.contains(handle)) return CONTAINER_INVALID_INDEX;
	U* data = remap.unset(handle, current);
	data->last_modified = current;
	data->enter_destroyed();
	return data->index;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_set_min_log_level(LiteRtLmLogSeverity level)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_set_min_log_level);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.set_min_log_level(level);
	}
	writer.write_int32_t(static_cast<int32_t>(level));
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_engine_settings_create(
	const char* model_path, const char* backend_str,
	const char* vision_backend_str, const char* audio_backend_str,
	LiteRtLmEngineSettings** out_settings)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_engine_settings_create);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.engine_settings_create(
			model_path, backend_str, vision_backend_str, audio_backend_str, out_settings);
	}

	const std::string basename = model_basename(model_path);
	uint32_t settings_index = out_settings ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	if (result == 0 && out_settings && *out_settings)
	{
		lava::lock_guard lock(frame_mutex);
		trackedlitertlmsettings* data = state.settings_index.add(*out_settings, writer.current);
		data->model_name = basename;
		if (backend_str) data->backend = backend_str;
		data->enter_created();
		settings_index = data->index;
	}

	uint8_t pointer_flags = 0;
	if (model_path) pointer_flags |= 1;
	if (backend_str) pointer_flags |= 2;
	if (vision_backend_str) pointer_flags |= 4;
	if (audio_backend_str) pointer_flags |= 8;
	if (out_settings) pointer_flags |= 16;

	writer.write_uint8_t(pointer_flags);
	writer.write_string(model_path ? basename.c_str() : nullptr);
	writer.write_string(backend_str);
	writer.write_string(vision_backend_str);
	writer.write_string(audio_backend_str);
	writer.write_uint32_t(settings_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_engine_settings_delete(LiteRtLmEngineSettings* settings)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_engine_settings_delete);
	uint32_t settings_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		settings_index = destroy_litert_lm_handle(state.settings_index, settings, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.engine_settings_delete(settings);
	}
	writer.write_uint32_t(settings_index);
	writer.end_packet();
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_engine_create(
	const LiteRtLmEngineSettings* settings, LiteRtLmEngine** out_engine)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_engine_create);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.engine_create(settings, out_engine);
	}

	uint32_t settings_index = CONTAINER_NULL_VALUE;
	uint32_t engine_index = out_engine ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	{
		lava::lock_guard lock(frame_mutex);
		settings_index = lookup_litert_lm_index(
			state.settings_index, const_cast<LiteRtLmEngineSettings*>(settings), writer.current);
		if (result == kLiteRtLmStatusOk && out_engine && *out_engine)
		{
			trackedlitertlmengine* data = state.engine_index.add(*out_engine, writer.current);
			data->settings_index = settings_index;
			data->enter_created();
			engine_index = data->index;
			state.active_engines.fetch_add(1, std::memory_order_release);
		}
	}

	uint8_t pointer_flags = 0;
	if (settings) pointer_flags |= 1;
	if (out_engine) pointer_flags |= 2;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(settings_index);
	writer.write_uint32_t(engine_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_engine_delete(LiteRtLmEngine* engine)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_engine_delete);
	uint32_t engine_index = CONTAINER_NULL_VALUE;
	bool was_active = false;
	{
		lava::lock_guard lock(frame_mutex);
		was_active = engine && state.engine_index.contains(engine);
		engine_index = destroy_litert_lm_handle(state.engine_index, engine, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.engine_delete(engine);
	}
	writer.write_uint32_t(engine_index);
	writer.end_packet();

	if (was_active)
	{
		const uint_fast32_t previous = state.active_engines.fetch_sub(1, std::memory_order_acq_rel);
		assert(previous > 0);
		if (previous == 1)
		{
			trace_finalize_if_inactive();
		}
	}
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_config_create(LiteRtLmSessionConfig** out_config)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_config_create);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_config_create(out_config);
	}

	uint32_t config_index = out_config ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	if (result == kLiteRtLmStatusOk && out_config && *out_config)
	{
		lava::lock_guard lock(frame_mutex);
		trackable* data = state.session_config_index.add(*out_config, writer.current);
		data->enter_created();
		config_index = data->index;
	}

	uint8_t pointer_flags = 0;
	if (out_config) pointer_flags |= 1;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(config_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_config_set_max_output_tokens(
	LiteRtLmSessionConfig* config, int max_output_tokens)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(
		instance, state, LITERT_LM_litert_lm_session_config_set_max_output_tokens);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_config_set_max_output_tokens(config, max_output_tokens);
	}

	uint32_t config_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		config_index = lookup_litert_lm_index(state.session_config_index, config, writer.current);
	}

	writer.write_uint32_t(config_index);
	writer.write_int32_t(max_output_tokens);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_sampler_params_create(
	LiteRtLmSamplerType type, LiteRtLmSamplerParams** out_params)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_sampler_params_create);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.sampler_params_create(type, out_params);
	}

	uint32_t sampler_params_index = out_params ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	if (result == kLiteRtLmStatusOk && out_params && *out_params)
	{
		lava::lock_guard lock(frame_mutex);
		trackable* data = state.sampler_params_index.add(*out_params, writer.current);
		data->enter_created();
		sampler_params_index = data->index;
	}

	uint8_t pointer_flags = 0;
	if (out_params) pointer_flags |= 1;

	writer.write_uint8_t(pointer_flags);
	writer.write_int32_t(static_cast<int32_t>(type));
	writer.write_uint32_t(sampler_params_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_sampler_params_set_top_p(
	LiteRtLmSamplerParams* params, float top_p)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_sampler_params_set_top_p);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.sampler_params_set_top_p(params, top_p);
	}

	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		sampler_params_index = lookup_litert_lm_index(state.sampler_params_index, params, writer.current);
	}

	writer.write_uint32_t(sampler_params_index);
	writer.write_float(top_p);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_sampler_params_set_top_k(
	LiteRtLmSamplerParams* params, int32_t top_k)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_sampler_params_set_top_k);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.sampler_params_set_top_k(params, top_k);
	}

	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		sampler_params_index = lookup_litert_lm_index(state.sampler_params_index, params, writer.current);
	}

	writer.write_uint32_t(sampler_params_index);
	writer.write_int32_t(top_k);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_sampler_params_set_temperature(
	LiteRtLmSamplerParams* params, float temperature)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(
		instance, state, LITERT_LM_litert_lm_sampler_params_set_temperature);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.sampler_params_set_temperature(params, temperature);
	}

	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		sampler_params_index = lookup_litert_lm_index(state.sampler_params_index, params, writer.current);
	}

	writer.write_uint32_t(sampler_params_index);
	writer.write_float(temperature);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_sampler_params_set_seed(
	LiteRtLmSamplerParams* params, int32_t seed)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_sampler_params_set_seed);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.sampler_params_set_seed(params, seed);
	}

	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		sampler_params_index = lookup_litert_lm_index(state.sampler_params_index, params, writer.current);
	}

	writer.write_uint32_t(sampler_params_index);
	writer.write_int32_t(seed);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_config_set_sampler_params(
	LiteRtLmSessionConfig* config, const LiteRtLmSamplerParams* sampler_params)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(
		instance, state, LITERT_LM_litert_lm_session_config_set_sampler_params);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_config_set_sampler_params(config, sampler_params);
	}

	uint32_t config_index = CONTAINER_NULL_VALUE;
	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		config_index = lookup_litert_lm_index(state.session_config_index, config, writer.current);
		sampler_params_index = lookup_litert_lm_index(
			state.sampler_params_index, const_cast<LiteRtLmSamplerParams*>(sampler_params), writer.current);
	}

	writer.write_uint32_t(config_index);
	writer.write_uint32_t(sampler_params_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_sampler_params_delete(LiteRtLmSamplerParams* params)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_sampler_params_delete);
	uint32_t sampler_params_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		sampler_params_index = destroy_litert_lm_handle(state.sampler_params_index, params, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.sampler_params_delete(params);
	}
	writer.write_uint32_t(sampler_params_index);
	writer.end_packet();
}

extern "C" EXPORT void litert_lm_session_config_delete(LiteRtLmSessionConfig* config)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_config_delete);
	uint32_t config_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		config_index = destroy_litert_lm_handle(state.session_config_index, config, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.session_config_delete(config);
	}
	writer.write_uint32_t(config_index);
	writer.end_packet();
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_engine_create_session(
	LiteRtLmEngine* engine, LiteRtLmSessionConfig* config,
	LiteRtLmSession** out_session)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_engine_create_session);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.engine_create_session(engine, config, out_session);
	}

	uint32_t engine_index = CONTAINER_NULL_VALUE;
	uint32_t config_index = CONTAINER_NULL_VALUE;
	uint32_t session_index = out_session ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	{
		lava::lock_guard lock(frame_mutex);
		engine_index = lookup_litert_lm_index(state.engine_index, engine, writer.current);
		config_index = lookup_litert_lm_index(state.session_config_index, config, writer.current);
		if (result == kLiteRtLmStatusOk && out_session && *out_session)
		{
			trackedlitertlmsession* data = state.session_index.add(*out_session, writer.current);
			data->engine_index = engine_index;
			data->config_index = config_index;
			data->enter_created();
			session_index = data->index;
		}
	}

	uint8_t pointer_flags = 0;
	if (engine) pointer_flags |= 1;
	if (config) pointer_flags |= 2;
	if (out_session) pointer_flags |= 4;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(engine_index);
	writer.write_uint32_t(config_index);
	writer.write_uint32_t(session_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_session_delete(LiteRtLmSession* session)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_delete);
	uint32_t session_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		session_index = destroy_litert_lm_handle(state.session_index, session, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.session_delete(session);
	}
	writer.write_uint32_t(session_index);
	writer.end_packet();
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_input_data_create(
	LiteRtLmInputDataType type, const void* data, size_t size,
	LiteRtLmInputData** out_input_data)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_input_data_create);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.input_data_create(type, data, size, out_input_data);
	}

	uint32_t input_data_index = out_input_data ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	if (result == kLiteRtLmStatusOk && out_input_data && *out_input_data)
	{
		lava::lock_guard lock(frame_mutex);
		trackable* tracked = state.input_data_index.add(*out_input_data, writer.current);
		tracked->enter_created();
		input_data_index = tracked->index;
	}

	uint8_t pointer_flags = 0;
	if (data) pointer_flags |= 1;
	if (out_input_data) pointer_flags |= 2;

	const uint32_t stored_data_size = (data && size > 0) ? (uint32_t)size : 0;
	writer.write_uint8_t(pointer_flags);
	writer.write_int32_t(static_cast<int32_t>(type));
	writer.write_uint64_t((uint64_t)size);
	writer.write_uint32_t(stored_data_size);
	if (stored_data_size > 0)
	{
		writer.write_array(reinterpret_cast<const uint8_t*>(data), stored_data_size);
	}
	writer.write_uint32_t(input_data_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_input_data_delete(LiteRtLmInputData* input_data)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_input_data_delete);
	uint32_t input_data_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		input_data_index = destroy_litert_lm_handle(state.input_data_index, input_data, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.input_data_delete(input_data);
	}
	writer.write_uint32_t(input_data_index);
	writer.end_packet();
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_generate_content(
	LiteRtLmSession* session, const LiteRtLmInputData* const* inputs,
	size_t num_inputs, LiteRtLmResponses** out_responses)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_generate_content);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_generate_content(session, inputs, num_inputs, out_responses);
	}

	uint32_t session_index = CONTAINER_NULL_VALUE;
	uint32_t responses_index = out_responses ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	const uint32_t input_count = (inputs && num_inputs > 0) ? (uint32_t)num_inputs : 0;
	std::vector<uint32_t> input_indices(input_count, CONTAINER_NULL_VALUE);
	{
		lava::lock_guard lock(frame_mutex);
		session_index = lookup_litert_lm_index(state.session_index, session, writer.current);
		for (uint32_t i = 0; i < input_count; i++)
		{
			input_indices[i] = lookup_litert_lm_index(
				state.input_data_index, const_cast<LiteRtLmInputData*>(inputs[i]), writer.current);
		}
		if (result == kLiteRtLmStatusOk && out_responses && *out_responses)
		{
			trackable* data = state.responses_index.add(*out_responses, writer.current);
			data->enter_created();
			responses_index = data->index;
		}
	}

	uint8_t pointer_flags = 0;
	if (session) pointer_flags |= 1;
	if (inputs) pointer_flags |= 2;
	if (out_responses) pointer_flags |= 4;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(session_index);
	writer.write_uint64_t((uint64_t)num_inputs);
	writer.write_uint32_t((uint32_t)input_indices.size());
	writer.write_array(input_indices.data(), input_indices.size());
	writer.write_uint32_t(responses_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_run_prefill(
	LiteRtLmSession* session, const LiteRtLmInputData* const* inputs,
	size_t num_inputs)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_run_prefill);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_run_prefill(session, inputs, num_inputs);
	}

	uint32_t session_index = CONTAINER_NULL_VALUE;
	const uint32_t input_count = (inputs && num_inputs > 0) ? (uint32_t)num_inputs : 0;
	std::vector<uint32_t> input_indices(input_count, CONTAINER_NULL_VALUE);
	{
		lava::lock_guard lock(frame_mutex);
		session_index = lookup_litert_lm_index(state.session_index, session, writer.current);
		for (uint32_t i = 0; i < input_count; i++)
		{
			input_indices[i] = lookup_litert_lm_index(
				state.input_data_index, const_cast<LiteRtLmInputData*>(inputs[i]), writer.current);
		}
	}

	uint8_t pointer_flags = 0;
	if (session) pointer_flags |= 1;
	if (inputs) pointer_flags |= 2;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(session_index);
	writer.write_uint64_t((uint64_t)num_inputs);
	writer.write_uint32_t((uint32_t)input_indices.size());
	writer.write_array(input_indices.data(), input_indices.size());
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_session_run_decode(
	LiteRtLmSession* session, LiteRtLmResponses** out_responses)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_session_run_decode);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.session_run_decode(session, out_responses);
	}

	uint32_t session_index = CONTAINER_NULL_VALUE;
	uint32_t responses_index = out_responses ? CONTAINER_NULL_VALUE : CONTAINER_INVALID_INDEX;
	{
		lava::lock_guard lock(frame_mutex);
		session_index = lookup_litert_lm_index(state.session_index, session, writer.current);
		if (result == kLiteRtLmStatusOk && out_responses && *out_responses)
		{
			trackable* data = state.responses_index.add(*out_responses, writer.current);
			data->enter_created();
			responses_index = data->index;
		}
	}

	uint8_t pointer_flags = 0;
	if (session) pointer_flags |= 1;
	if (out_responses) pointer_flags |= 2;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(session_index);
	writer.write_uint32_t(responses_index);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT void litert_lm_responses_delete(LiteRtLmResponses* responses)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(instance, state, LITERT_LM_litert_lm_responses_delete);
	uint32_t responses_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		responses_index = destroy_litert_lm_handle(state.responses_index, responses, writer.current);
	}
	{
		litert_lm_call_guard guard(state);
		state.target_dispatch.responses_delete(responses);
	}
	writer.write_uint32_t(responses_index);
	writer.end_packet();
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_responses_get_num_candidates(
	const LiteRtLmResponses* responses, size_t* out_num_candidates)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(
		instance, state, LITERT_LM_litert_lm_responses_get_num_candidates);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.responses_get_num_candidates(responses, out_num_candidates);
	}

	uint32_t responses_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		responses_index = lookup_litert_lm_index(
			state.responses_index, const_cast<LiteRtLmResponses*>(responses), writer.current);
	}

	uint8_t pointer_flags = 0;
	if (responses) pointer_flags |= 1;
	if (out_num_candidates) pointer_flags |= 2;

	const uint64_t num_candidates = (result == kLiteRtLmStatusOk && out_num_candidates) ? (uint64_t)*out_num_candidates : 0;
	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(responses_index);
	writer.write_uint64_t(num_candidates);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}

extern "C" EXPORT LiteRtLmStatusCode litert_lm_responses_get_response_text_at(
	const LiteRtLmResponses* responses, size_t index, const char** out_text)
{
	lava_writer& instance = lava_writer::instance();
	litert_lm_layer_runtime_state& state = instance.litert_lm_layer;
	ensure_litert_lm_dispatch(state);

	lava_file_writer& writer = begin_litert_lm_packet(
		instance, state, LITERT_LM_litert_lm_responses_get_response_text_at);
	LiteRtLmStatusCode result = kLiteRtLmStatusOk;
	{
		litert_lm_call_guard guard(state);
		result = state.target_dispatch.responses_get_response_text_at(responses, index, out_text);
	}

	uint32_t responses_index = CONTAINER_NULL_VALUE;
	{
		lava::lock_guard lock(frame_mutex);
		responses_index = lookup_litert_lm_index(
			state.responses_index, const_cast<LiteRtLmResponses*>(responses), writer.current);
	}

	uint8_t pointer_flags = 0;
	if (responses) pointer_flags |= 1;
	if (out_text) pointer_flags |= 2;

	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(responses_index);
	writer.write_uint64_t((uint64_t)index);
	writer.write_string((result == kLiteRtLmStatusOk && out_text) ? *out_text : nullptr);
	writer.write_int32_t(static_cast<int32_t>(result));
	writer.end_packet();
	return result;
}
