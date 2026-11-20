#pragma once

#include <string>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lavamutex.h"
#include "litert_lm_auto.h"
#include "jsoncpp/json/value.h"

class lava_file_reader;

struct litert_lm_turn_statistics
{
	size_t tokens = 0;
	double tokens_per_second = 0;
};

struct litert_lm_session_statistics
{
	int32_t status = -1;
	double init_time = 0;
	double time_to_first_token = 0;
	std::vector<litert_lm_turn_statistics> prefill;
	std::vector<litert_lm_turn_statistics> decode;
};

struct litert_lm_replay_state
{
	bool initialized = false;
	void* library_handle = nullptr;
	litert_lm_dispatch_table dispatch;
	lava::mutex mutex;

	std::unordered_map<uint32_t, LiteRtLmEngineSettings*> settings_map;
	std::unordered_map<uint32_t, LiteRtLmEngine*> engine_map;
	std::unordered_map<uint32_t, LiteRtLmSessionConfig*> session_config_map;
	std::unordered_map<uint32_t, LiteRtLmSamplerParams*> sampler_params_map;
	std::unordered_map<uint32_t, LiteRtLmSession*> session_map;
	std::unordered_map<uint32_t, LiteRtLmInputData*> input_data_map;
	std::unordered_map<uint32_t, LiteRtLmResponses*> responses_map;
	std::unordered_map<uint32_t, LiteRtLmConversationConfig*> conversation_config_map;
	std::unordered_map<uint32_t, LiteRtLmConversationOptionalArgs*> conversation_optional_args_map;
	std::unordered_map<uint32_t, LiteRtLmConversation*> conversation_map;
	std::unordered_map<uint32_t, LiteRtLmJsonResponse*> json_response_map;
	std::unordered_map<uint32_t, uint32_t> json_response_conversations;
	std::map<uint32_t, litert_lm_session_statistics> conversation_statistics;
	std::map<uint32_t, litert_lm_session_statistics> statistics;
	std::unordered_map<uint32_t, uint32_t> response_sessions;
	Json::Value outputs = Json::Value(Json::arrayValue);
	uint64_t verified_statuses = 0;
	uint64_t verified_candidate_counts = 0;
	uint64_t verified_responses = 0;
	uint64_t verification_unavailable = 0;
	Json::Value verification_failures = Json::Value(Json::arrayValue);

	// Called with mutex held, or after all replay threads have finished.
	void record_output(lava_file_reader& reader, std::optional<LiteRtLmStatusCode> status, const char* text);
	void verify_status(lava_file_reader& reader, int32_t status);
	void verify_candidates(lava_file_reader& reader, uint64_t count);
	void verify_text(lava_file_reader& reader, const char* text);
	void record_mismatch(lava_file_reader& reader, Json::Value failure);
	Json::Value report_verification();

	// Called with mutex held, while the runtime session is still alive.
	void collect_statistics(uint32_t index, LiteRtLmSession* session, LiteRtLmConversation* conversation = nullptr);
	Json::Value report_statistics(bool conversations = false);

	~litert_lm_replay_state();
};

void retrace_litert_lm_apicall(lava_file_reader& reader);
