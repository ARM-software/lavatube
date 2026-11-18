#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdlib>
#include <cstring>
#include <optional>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "litert_lm_replay.h"
#include "read.h"
#include "util.h"

void litert_lm_replay_state::record_mismatch(lava_file_reader& reader, Json::Value failure)
{
	const litert_lm_api_packet& packet = reader.current_litert_lm_packet;
	failure["call"] = litert_lm_get_function_name(reader.current.call_id);
	failure["thread"] = reader.current.thread;
	failure["packet"] = reader.current.packet;
	if (packet.responses_index != CONTAINER_INVALID_INDEX && packet.responses_index != CONTAINER_NULL_VALUE)
	{
		failure["responses_index"] = packet.responses_index;
	}
	uint32_t session = packet.session_index;
	auto it = response_sessions.find(packet.responses_index);
	if (it != response_sessions.end()) session = it->second;
	if (session != CONTAINER_INVALID_INDEX && session != CONTAINER_NULL_VALUE) failure["session_index"] = session;
	if (reader.current.call_id == LITERT_LM_litert_lm_responses_get_response_text_at)
	{
		failure["candidate_index"] = Json::UInt64(packet.candidate_index);
	}
	ELOG("LiteRT-LM verification mismatch: %s, thread %u packet %u, session %u response %u candidate %llu (%s)",
	     litert_lm_get_function_name(reader.current.call_id), reader.current.thread, reader.current.packet,
	     session, packet.responses_index, (unsigned long long)packet.candidate_index, failure["field"].asCString());
	verification_failures.append(failure);
}

void litert_lm_replay_state::verify_status(lava_file_reader& reader, int32_t status)
{
	verified_statuses++;
	if (reader.current_litert_lm_packet.result == status) return;
	Json::Value failure;
	failure["field"] = "status";
	failure["captured"] = reader.current_litert_lm_packet.result;
	failure["replayed"] = status;
	ELOG("LiteRT-LM status: captured %d, replayed %d", reader.current_litert_lm_packet.result, status);
	record_mismatch(reader, failure);
}

void litert_lm_replay_state::verify_candidates(lava_file_reader& reader, uint64_t count)
{
	verified_candidate_counts++;
	if (reader.current_litert_lm_packet.num_candidates == count) return;
	Json::Value failure;
	failure["field"] = "candidate_count";
	failure["captured"] = Json::UInt64(reader.current_litert_lm_packet.num_candidates);
	failure["replayed"] = Json::UInt64(count);
	record_mismatch(reader, failure);
}

void litert_lm_replay_state::verify_text(lava_file_reader& reader, const char* text)
{
	verified_responses++;
	const std::string& expected = reader.current_litert_lm_packet.response_text;
	const size_t actual_size = text ? strlen(text) : 0;
	size_t offset = 0;
	while (offset < expected.size() && offset < actual_size && expected[offset] == text[offset]) offset++;
	if (offset == expected.size() && offset == actual_size) return;
	Json::Value failure;
	failure["field"] = "response_text";
	failure["first_differing_byte"] = Json::UInt64(offset);
	failure["captured_length"] = Json::UInt64(expected.size());
	failure["replayed_length"] = Json::UInt64(actual_size);
	ELOG("LiteRT-LM response differs at byte %llu (captured length %llu, replayed length %llu)",
	     (unsigned long long)offset, (unsigned long long)expected.size(), (unsigned long long)actual_size);
	record_mismatch(reader, failure);
}

Json::Value litert_lm_replay_state::report_verification()
{
	Json::Value result;
	const bool failed = !verification_failures.empty() || verification_unavailable != 0;
	result["status"] = failed ? "failed" : (verified_responses ? "passed" : "no_output");
	result["statuses_checked"] = Json::UInt64(verified_statuses);
	result["candidate_counts_checked"] = Json::UInt64(verified_candidate_counts);
	result["responses_checked"] = Json::UInt64(verified_responses);
	result["unavailable_checks"] = Json::UInt64(verification_unavailable);
	result["mismatches"] = verification_failures;
	ILOG("LiteRT-LM output verification: %s (%llu responses, %llu candidate counts, %llu statuses, %u mismatches, %llu unavailable)",
	     result["status"].asCString(), (unsigned long long)verified_responses,
	     (unsigned long long)verified_candidate_counts, (unsigned long long)verified_statuses,
	     verification_failures.size(), (unsigned long long)verification_unavailable);
	if (!verified_responses) ELOG("LiteRT-LM output verification: no response text could be verified (none captured or replay output unavailable)");
	return result;
}

void litert_lm_replay_state::collect_statistics(uint32_t index, LiteRtLmSession* session)
{
	litert_lm_session_statistics& stats = statistics[index];
	stats = {};
	if (!dispatch.session_get_benchmark_info || !dispatch.benchmark_info_delete
	    || !dispatch.benchmark_info_get_total_init_time_in_second || !dispatch.benchmark_info_get_time_to_first_token
	    || !dispatch.benchmark_info_get_num_prefill_turns || !dispatch.benchmark_info_get_num_decode_turns
	    || !dispatch.benchmark_info_get_prefill_token_count_at || !dispatch.benchmark_info_get_decode_token_count_at
	    || !dispatch.benchmark_info_get_prefill_tokens_per_sec_at || !dispatch.benchmark_info_get_decode_tokens_per_sec_at)
	{
		return;
	}
	LiteRtLmBenchmarkInfo* info = nullptr;
	stats.status = dispatch.session_get_benchmark_info(session, &info);
	if (stats.status != 0 || !info)
	{
		if (stats.status == 0) stats.status = -1;
		return;
	}
	size_t prefill_count = 0;
	size_t decode_count = 0;
	stats.status = dispatch.benchmark_info_get_total_init_time_in_second(info, &stats.init_time);
	if (stats.status == 0) stats.status = dispatch.benchmark_info_get_time_to_first_token(info, &stats.time_to_first_token);
	if (stats.status == 0) stats.status = dispatch.benchmark_info_get_num_prefill_turns(info, &prefill_count);
	if (stats.status == 0) stats.status = dispatch.benchmark_info_get_num_decode_turns(info, &decode_count);
	for (size_t i = 0; stats.status == 0 && i < prefill_count; i++)
	{
		litert_lm_turn_statistics turn;
		stats.status = dispatch.benchmark_info_get_prefill_token_count_at(info, i, &turn.tokens);
		if (stats.status == 0) stats.status = dispatch.benchmark_info_get_prefill_tokens_per_sec_at(info, i, &turn.tokens_per_second);
		if (stats.status == 0) stats.prefill.push_back(turn);
	}
	for (size_t i = 0; stats.status == 0 && i < decode_count; i++)
	{
		litert_lm_turn_statistics turn;
		stats.status = dispatch.benchmark_info_get_decode_token_count_at(info, i, &turn.tokens);
		if (stats.status == 0) stats.status = dispatch.benchmark_info_get_decode_tokens_per_sec_at(info, i, &turn.tokens_per_second);
		if (stats.status == 0) stats.decode.push_back(turn);
	}
	dispatch.benchmark_info_delete(info);
}

Json::Value litert_lm_replay_state::report_statistics()
{
	lava::lock_guard lock(mutex);
	Json::Value sessions(Json::arrayValue);
	for (const auto& pair : session_map) collect_statistics(pair.first, pair.second);
	if (statistics.empty())
	{
		ILOG("LiteRT-LM replay statistics: no session metrics available");
	}
	for (const auto& pair : statistics)
	{
		const litert_lm_session_statistics& stats = pair.second;
		ILOG("LiteRT-LM replay statistics (session %u):", pair.first);
		if (stats.status != 0)
		{
			ILOG("Benchmark metrics unavailable (status %d)", stats.status);
			continue;
		}
		Json::Value session;
		session["session_index"] = pair.first;
		session["init_time_seconds"] = stats.init_time;
		session["time_to_first_token_seconds"] = stats.time_to_first_token;
		session["prefill"] = Json::arrayValue;
		session["decode"] = Json::arrayValue;
		for (size_t i = 0; i < stats.prefill.size(); i++)
		{
			const litert_lm_turn_statistics& turn = stats.prefill[i];
			Json::Value measurement;
			measurement["tokens"] = Json::UInt64(turn.tokens);
			measurement["tokens_per_second"] = turn.tokens_per_second;
			session["prefill"].append(measurement);
			if (stats.prefill.size() > 1)
			{
				ILOG("Prefill turn %zu:", i + 1);
			}
			ILOG("Prefill speed:        %.2f tokens/s (%zu tokens)", turn.tokens_per_second, turn.tokens);
		}
		for (size_t i = 0; i < stats.decode.size(); i++)
		{
			const litert_lm_turn_statistics& turn = stats.decode[i];
			Json::Value measurement;
			measurement["tokens"] = Json::UInt64(turn.tokens);
			measurement["tokens_per_second"] = turn.tokens_per_second;
			session["decode"].append(measurement);
			if (stats.decode.size() > 1)
			{
				ILOG("Decode turn %zu:", i + 1);
			}
			ILOG("Decode speed:         %.2f tokens/s (%zu tokens)", turn.tokens_per_second, turn.tokens);
		}
		ILOG("Init time:            %.4f s", stats.init_time);
		ILOG("Time to first token:  %.4f s", stats.time_to_first_token);
		sessions.append(session);
	}
	return sessions;
}

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
	std::optional<LiteRtLmStatusCode> replay_status;

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
		std::string model_path;
		std::string model_error;
		const char* override_path = p__litert_model_path ? p__litert_model_path : getenv("LITERT_MODEL_PATH");
		if (!find_litert_lm_model(packet.model_path, packet.model_identity, override_path, model_path, model_error))
		{
			ABORT("%s", model_error.c_str());
		}
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
		if (state->dispatch.engine_settings_enable_benchmark)
		{
			const LiteRtLmStatusCode benchmark_status = state->dispatch.engine_settings_enable_benchmark(settings);
			if (benchmark_status != 0)
			{
				ELOG("Failed to enable LiteRT-LM replay metrics: status %d", (int)benchmark_status);
			}
		}
		LiteRtLmStatusCode status = state->dispatch.engine_create(settings, &engine);
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
			replay_status = state->dispatch.session_config_set_max_output_tokens(it->second, packet.max_output_tokens);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_create:
	{
		LiteRtLmSamplerParams* params = nullptr;
		LiteRtLmStatusCode status = state->dispatch.sampler_params_create((LiteRtLmSamplerType)packet.sampler_type, &params);
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
			replay_status = state->dispatch.sampler_params_set_top_p(it->second, packet.top_p);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_top_k:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_top_k)
		{
			replay_status = state->dispatch.sampler_params_set_top_k(it->second, packet.top_k);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_temperature:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_temperature)
		{
			replay_status = state->dispatch.sampler_params_set_temperature(it->second, packet.temperature);
		}
		break;
	}
	case LITERT_LM_litert_lm_sampler_params_set_seed:
	{
		auto it = state->sampler_params_map.find(packet.sampler_params_index);
		if (it != state->sampler_params_map.end() && state->dispatch.sampler_params_set_seed)
		{
			replay_status = state->dispatch.sampler_params_set_seed(it->second, packet.seed);
		}
		break;
	}
	case LITERT_LM_litert_lm_session_config_set_sampler_params:
	{
		auto it_c = state->session_config_map.find(packet.config_index);
		auto it_s = state->sampler_params_map.find(packet.sampler_params_index);
		if (it_c != state->session_config_map.end() && it_s != state->sampler_params_map.end() && state->dispatch.session_config_set_sampler_params)
		{
			replay_status = state->dispatch.session_config_set_sampler_params(it_c->second, it_s->second);
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
			state->collect_statistics(packet.session_index, it->second);
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
		if (status != 0 || !responses)
		{
			ABORT("litert_lm_session_generate_content failed with status %d", (int)status);
		}
		state->responses_map[packet.responses_index] = responses;
		state->response_sessions[packet.responses_index] = packet.session_index;
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
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
		replay_status = status;
		if (status != 0 && parent->litert_verify_output) break;
		if (status != 0 || !responses)
		{
			ABORT("litert_lm_session_run_decode failed with status %d", (int)status);
		}
		state->responses_map[packet.responses_index] = responses;
		state->response_sessions[packet.responses_index] = packet.session_index;
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
		if (state->dispatch.responses_get_num_candidates && (it != state->responses_map.end() || parent->litert_verify_output))
		{
			size_t num = 0;
			replay_status = state->dispatch.responses_get_num_candidates(
				it != state->responses_map.end() ? it->second : nullptr, (packet.pointer_flags & 2) ? &num : nullptr);
			if (parent->litert_verify_output && packet.result == 0 && (packet.pointer_flags & 2))
			{
				if (*replay_status == 0) state->verify_candidates(reader, num);
				else state->verification_unavailable++;
			}
		}
		break;
	}
	case LITERT_LM_litert_lm_responses_get_response_text_at:
	{
		auto it = state->responses_map.find(packet.responses_index);
		if (state->dispatch.responses_get_response_text_at && (it != state->responses_map.end() || parent->litert_verify_output))
		{
			const char* text = nullptr;
			replay_status = state->dispatch.responses_get_response_text_at(
				it != state->responses_map.end() ? it->second : nullptr,
				packet.candidate_index, (packet.pointer_flags & 2) ? &text : nullptr);
			if (parent->litert_verify_output && packet.result == 0 && (packet.pointer_flags & 2))
			{
				if (*replay_status == 0) state->verify_text(reader, text);
				else state->verification_unavailable++;
			}
			if (text) ILOG("LiteRT-LM replay generated response: %s", text);
		}
		break;
	}
	default:
		break;
	}
	if (parent->litert_verify_output && packet.has_result)
	{
		if (replay_status) state->verify_status(reader, *replay_status);
		else
		{
			state->verification_unavailable++;
			ELOG("LiteRT-LM verification unavailable: %s was not executed", litert_lm_get_function_name(reader.current.call_id));
		}
	}
}
