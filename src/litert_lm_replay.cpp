#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "litert_lm_replay.h"
#include "read.h"
#include "util.h"

litert_lm_replay_state::~litert_lm_replay_state()
{
	lava::lock_guard lock(mutex);
	for (auto& pair : responses_map)
	{
		if (pair.second && dispatch.responses_delete)
		{
			dispatch.responses_delete(pair.second);
		}
	}
	responses_map.clear();

	for (auto& pair : input_data_map)
	{
		if (pair.second && dispatch.input_data_delete)
		{
			dispatch.input_data_delete(pair.second);
		}
	}
	input_data_map.clear();

	for (auto& pair : session_map)
	{
		if (pair.second && dispatch.session_delete)
		{
			dispatch.session_delete(pair.second);
		}
	}
	session_map.clear();

	for (auto& pair : sampler_params_map)
	{
		if (pair.second && dispatch.sampler_params_delete)
		{
			dispatch.sampler_params_delete(pair.second);
		}
	}
	sampler_params_map.clear();

	for (auto& pair : session_config_map)
	{
		if (pair.second && dispatch.session_config_delete)
		{
			dispatch.session_config_delete(pair.second);
		}
	}
	session_config_map.clear();

	for (auto& pair : engine_map)
	{
		if (pair.second && dispatch.engine_delete)
		{
			dispatch.engine_delete(pair.second);
		}
	}
	engine_map.clear();

	for (auto& pair : settings_map)
	{
		if (pair.second && dispatch.engine_settings_delete)
		{
			dispatch.engine_settings_delete(pair.second);
		}
	}
	settings_map.clear();

	if (library_handle)
	{
		dlclose(library_handle);
		library_handle = nullptr;
	}
}

static void ensure_litert_lm_replay(litert_lm_replay_state* state)
{
	if (state->initialized)
	{
		return;
	}

	if (!state->library_handle)
	{
		const char* custom_lib = getenv("LAVATUBE_LITERT_LM_LIB");
		if (custom_lib && *custom_lib)
		{
			state->library_handle = dlopen(custom_lib, RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state->library_handle)
		{
			state->library_handle = dlopen("/opt/litert-lm/lib/liblitert-lm.so", RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state->library_handle)
		{
			state->library_handle = dlopen("liblitert-lm_real.so", RTLD_LAZY | RTLD_LOCAL);
		}
		if (!state->library_handle)
		{
			state->library_handle = dlopen("liblitert-lm.so", RTLD_LAZY | RTLD_LOCAL);
		}
	}

	if (!state->library_handle)
	{
		ABORT("Failed to load liblitert-lm.so for replay (checked LAVATUBE_LITERT_LM_LIB, /opt/litert-lm/lib/liblitert-lm.so, liblitert-lm_real.so, liblitert-lm.so): %s", dlerror());
	}

	resolve_all_litert_lm_dispatch(&state->dispatch, state->library_handle);
	state->initialized = true;
}

static std::string resolve_litert_model_path(const std::string& recorded_path)
{
	std::string base = recorded_path;
	const char* slash = strrchr(base.c_str(), '/');
	const char* backslash = strrchr(base.c_str(), '\\');
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

	const char* override_path = p__litert_model_path ? p__litert_model_path : getenv("LITERT_MODEL_PATH");
	if (override_path && *override_path)
	{
		struct stat st;
		if (stat(override_path, &st) == 0)
		{
			if (S_ISDIR(st.st_mode))
			{
				std::string candidate = std::string(override_path) + "/" + base;
				if (access(candidate.c_str(), R_OK) == 0)
				{
					return candidate;
				}
			}
			else
			{
				return std::string(override_path);
			}
		}
	}

	if (access(base.c_str(), R_OK) == 0)
	{
		return base;
	}

	const char* home = getenv("HOME");
	if (home && *home)
	{
		std::string candidate = std::string(home) + "/.litert-lm/models/" + base;
		if (access(candidate.c_str(), R_OK) == 0)
		{
			return candidate;
		}
	}

	{
		std::string candidate = "/opt/litert-lm/models/" + base;
		if (access(candidate.c_str(), R_OK) == 0)
		{
			return candidate;
		}
	}

	{
		std::string candidate = "/home/per/LiteRT-LM/runtime/testdata/" + base;
		if (access(candidate.c_str(), R_OK) == 0)
		{
			return candidate;
		}
	}

	if (access(recorded_path.c_str(), R_OK) == 0)
	{
		return recorded_path;
	}

	ABORT("LiteRT-LM model '%s' not found. Specify location using --litert-model-path or LITERT_MODEL_PATH", base.c_str());
	return std::string();
}

void retrace_litert_lm_apicall(lava_file_reader& reader)
{
	if (is_blackhole_mode())
	{
		return;
	}

	lava_reader* parent = reader.parent;
	if (!parent->litert_lm_replay)
	{
		parent->litert_lm_replay = new litert_lm_replay_state();
		ensure_litert_lm_replay(parent->litert_lm_replay);
	}
	litert_lm_replay_state* state = parent->litert_lm_replay;
	lava::lock_guard lock(state->mutex);
	const litert_lm_api_packet& packet = reader.current_litert_lm_packet;

	switch (reader.current.call_id)
	{
	case LITERT_LM_litert_lm_set_min_log_level:
	{
		if (state->dispatch.set_min_log_level)
		{
			state->dispatch.set_min_log_level((LiteRtLmLogSeverity)packet.log_level);
		}
		break;
	}
	case LITERT_LM_litert_lm_engine_settings_create:
	{
		std::string model_path = resolve_litert_model_path(packet.model_path);
		const char* backend = p__litert_backend ? p__litert_backend : getenv("LITERT_BACKEND");
		if (!backend || !*backend)
		{
			if (p__cpu)
			{
				backend = "cpu";
			}
			else if (p__gpu)
			{
				backend = "gpu";
			}
			else if (!packet.backend_str.empty())
			{
				backend = packet.backend_str.c_str();
			}
			else
			{
				backend = nullptr;
			}
		}
		const char* vision_backend = packet.vision_backend_str.empty() ? nullptr : packet.vision_backend_str.c_str();
		const char* audio_backend = packet.audio_backend_str.empty() ? nullptr : packet.audio_backend_str.c_str();

		ILOG("LiteRT-LM initializing engine settings with model: %s (backend: %s)", model_path.c_str(), backend ? backend : "default");
		LiteRtLmEngineSettings* settings = nullptr;
		LiteRtLmStatusCode status = state->dispatch.engine_settings_create(
			model_path.c_str(),
			backend,
			vision_backend,
			audio_backend,
			&settings);
		if (status != 0 || !settings)
		{
			ABORT("litert_lm_engine_settings_create failed with status %d", (int)status);
		}
		state->settings_map[packet.settings_index] = settings;
		break;
	}
	case LITERT_LM_litert_lm_engine_settings_delete:
	{
		auto it = state->settings_map.find(packet.settings_index);
		if (it != state->settings_map.end())
		{
			if (state->dispatch.engine_settings_delete)
			{
				state->dispatch.engine_settings_delete(it->second);
			}
			state->settings_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_engine_create:
	{
		LiteRtLmEngineSettings* settings = nullptr;
		auto it = state->settings_map.find(packet.settings_index);
		if (it != state->settings_map.end())
		{
			settings = it->second;
		}

		LiteRtLmEngine* engine = nullptr;
		LiteRtLmStatusCode status = state->dispatch.engine_create(settings, &engine);
		if (status != 0 || !engine)
		{
			ABORT("litert_lm_engine_create failed with status %d", (int)status);
		}
		state->engine_map[packet.engine_index] = engine;
		break;
	}
	case LITERT_LM_litert_lm_engine_delete:
	{
		auto it = state->engine_map.find(packet.engine_index);
		if (it != state->engine_map.end())
		{
			if (state->dispatch.engine_delete)
			{
				state->dispatch.engine_delete(it->second);
			}
			state->engine_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_config_create:
	{
		LiteRtLmSessionConfig* config = nullptr;
		LiteRtLmStatusCode status = state->dispatch.session_config_create(&config);
		if (status != 0 || !config)
		{
			ABORT("litert_lm_session_config_create failed with status %d", (int)status);
		}
		state->session_config_map[packet.config_index] = config;
		break;
	}
	case LITERT_LM_litert_lm_session_config_set_max_output_tokens:
	{
		auto it = state->session_config_map.find(packet.config_index);
		if (it != state->session_config_map.end() && state->dispatch.session_config_set_max_output_tokens)
		{
			state->dispatch.session_config_set_max_output_tokens(it->second, packet.max_output_tokens);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_create:
	{
		LiteRtLmSamplerParams* params = nullptr;
		LiteRtLmStatusCode status = state->dispatch.sampler_params_create((LiteRtLmSamplerType)packet.sampler_type, &params);
		if (status != 0 || !params)
		{
			ABORT("litert_lm_sampler_params_create failed with status %d", (int)status);
		}
		state->sampler_params_map[packet.sampler_params_index] = params;
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_top_p:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_top_p)
		{
			state->dispatch.sampler_params_set_top_p(it->second, packet.top_p);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_top_k:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_top_k)
		{
			state->dispatch.sampler_params_set_top_k(it->second, packet.top_k);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_temperature:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_temperature)
		{
			state->dispatch.sampler_params_set_temperature(it->second, packet.temperature);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_seed:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_seed)
		{
			state->dispatch.sampler_params_set_seed(it->second, packet.seed);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_config_set_sampler_params:
	{
		auto it_c = state->session_config_map.find(packet.config_index);
		auto it_s = state->sampler_params_map.find(packet.sampler_params_index);
		if (it_c != state->session_config_map.end() && it_s != state->sampler_params_map.end() && state->dispatch.session_config_set_sampler_params)
		{
			state->dispatch.session_config_set_sampler_params(it_c->second, it_s->second);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_delete:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end())
		{
			if (state->dispatch.sampler_params_delete)
			{
				state->dispatch.sampler_params_delete(it->second);
			}
			state->sampler_params_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_config_delete:
	{
		auto it = state->session_config_map.find(packet.config_index);
		if (it != state->session_config_map.end())
		{
			if (state->dispatch.session_config_delete)
			{
				state->dispatch.session_config_delete(it->second);
			}
			state->session_config_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_engine_create_session:
	{
		LiteRtLmEngine* engine = nullptr;
		LiteRtLmSessionConfig* config = nullptr;
		auto it_e = state->engine_map.find(packet.engine_index);
		if (it_e != state->engine_map.end())
		{
			engine = it_e->second;
		}
		auto it_c = state->session_config_map.find(packet.config_index);
		if (it_c != state->session_config_map.end())
		{
			config = it_c->second;
		}

		LiteRtLmSession* session = nullptr;
		LiteRtLmStatusCode status = state->dispatch.engine_create_session(engine, config, &session);
		if (status != 0 || !session)
		{
			ABORT("litert_lm_engine_create_session failed with status %d", (int)status);
		}
		state->session_map[packet.session_index] = session;
		break;
	}
	case LITERT_LM_litert_lm_session_delete:
	{
		auto it = state->session_map.find(packet.session_index);
		if (it != state->session_map.end())
		{
			if (state->dispatch.session_delete)
			{
				state->dispatch.session_delete(it->second);
			}
			state->session_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_input_data_create:
	{
		LiteRtLmInputData* input_data = nullptr;
		LiteRtLmStatusCode status = state->dispatch.input_data_create(
			(LiteRtLmInputDataType)packet.input_data_type,
			packet.data.data(),
			packet.data_size,
			&input_data);
		if (status != 0 || !input_data)
		{
			ABORT("litert_lm_input_data_create failed with status %d", (int)status);
		}
		state->input_data_map[packet.input_data_index] = input_data;
		break;
	}
	case LITERT_LM_litert_lm_input_data_delete:
	{
		auto it = state->input_data_map.find(packet.input_data_index);
		if (it != state->input_data_map.end())
		{
			if (state->dispatch.input_data_delete)
			{
				state->dispatch.input_data_delete(it->second);
			}
			state->input_data_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_generate_content:
	{
		LiteRtLmSession* session = nullptr;
		auto it = state->session_map.find(packet.session_index);
		if (it != state->session_map.end())
		{
			session = it->second;
		}

		std::vector<const LiteRtLmInputData*> inputs;
		inputs.reserve(packet.input_indices.size());
		for (uint32_t idx : packet.input_indices)
		{
			auto it_i = state->input_data_map.find(idx);
			inputs.push_back(it_i != state->input_data_map.end() ? it_i->second : nullptr);
		}

		LiteRtLmResponses* responses = nullptr;
		LiteRtLmStatusCode status = state->dispatch.session_generate_content(
			session,
			inputs.data(),
			packet.num_inputs,
			&responses);
		if (status != 0 || !responses)
		{
			ABORT("litert_lm_session_generate_content failed with status %d", (int)status);
		}
		state->responses_map[packet.responses_index] = responses;
		break;
	}
	case LITERT_LM_litert_lm_session_run_prefill:
	{
		LiteRtLmSession* session = nullptr;
		auto it = state->session_map.find(packet.session_index);
		if (it != state->session_map.end())
		{
			session = it->second;
		}

		std::vector<const LiteRtLmInputData*> inputs;
		inputs.reserve(packet.input_indices.size());
		for (uint32_t idx : packet.input_indices)
		{
			auto it_i = state->input_data_map.find(idx);
			inputs.push_back(it_i != state->input_data_map.end() ? it_i->second : nullptr);
		}

		LiteRtLmStatusCode status = state->dispatch.session_run_prefill(
			session,
			inputs.data(),
			packet.num_inputs);
		if (status != 0)
		{
			ABORT("litert_lm_session_run_prefill failed with status %d", (int)status);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_run_decode:
	{
		LiteRtLmSession* session = nullptr;
		auto it = state->session_map.find(packet.session_index);
		if (it != state->session_map.end())
		{
			session = it->second;
		}

		LiteRtLmResponses* responses = nullptr;
		LiteRtLmStatusCode status = state->dispatch.session_run_decode(session, &responses);
		if (status != 0 || !responses)
		{
			ABORT("litert_lm_session_run_decode failed with status %d", (int)status);
		}
		state->responses_map[packet.responses_index] = responses;
		break;
	}
	case LITERT_LM_litert_lm_responses_delete:
	{
		auto it = state->responses_map.find(packet.responses_index);
		if (it != state->responses_map.end())
		{
			if (state->dispatch.responses_delete)
			{
				state->dispatch.responses_delete(it->second);
			}
			state->responses_map.erase(it);
		}
		break;
	}
	case LITERT_LM_litert_lm_responses_get_num_candidates:
	{
		auto it = state->responses_map.find(packet.responses_index);
		if (it != state->responses_map.end() && state->dispatch.responses_get_num_candidates)
		{
			size_t num = 0;
			state->dispatch.responses_get_num_candidates(it->second, &num);
		}
		break;
	}
	case LITERT_LM_litert_lm_responses_get_response_text_at:
	{
		auto it = state->responses_map.find(packet.responses_index);
		if (it != state->responses_map.end() && state->dispatch.responses_get_response_text_at)
		{
			const char* text = nullptr;
			state->dispatch.responses_get_response_text_at(it->second, packet.candidate_index, &text);
			if (text)
			{
				ILOG("LiteRT-LM replay generated response: %s", text);
			}
		}
		break;
	}
	default:
		break;
	}
}
