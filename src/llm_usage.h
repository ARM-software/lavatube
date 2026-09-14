#pragma once

#include <stdint.h>

#include <string>

#include "jsoncpp/json/value.h"

struct llm_usage_tracker
{
	uint64_t input_tokens = 0;
	uint64_t cached_tokens = 0;
	uint64_t output_tokens = 0;
	uint64_t total_tokens = 0;
	bool has_input_tokens = false;
	bool has_cached_tokens = false;
	bool has_output_tokens = false;
	bool has_total_tokens = false;

	void add(const Json::Value& root_or_response);
	std::string format() const;
};
