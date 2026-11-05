#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "lavamutex.h"
#include "litert_lm_auto.h"

class lava_file_reader;

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

	~litert_lm_replay_state();
};

void retrace_litert_lm_apicall(lava_file_reader& reader);
