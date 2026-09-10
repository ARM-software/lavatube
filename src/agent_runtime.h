#pragma once

#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "agent_tools.h"

struct agent_runtime_options
{
	std::string api_key;
	std::string model;
	std::string base_url;
	std::string reasoning_effort;
	uint32_t timeout_seconds = 300;
	size_t max_output_bytes = 32768;
	FILE* debug_file = nullptr;
	bool verbose = false;
};

class agent_runtime
{
public:
	agent_runtime(const agent_runtime_options& options, const agent_tools& tools);

	Json::Value ask(const std::string& prompt, const std::chrono::steady_clock::time_point& deadline);

private:
	struct http_response
	{
		bool ok = false;
		bool timed_out = false;
		long code = 0;
		std::string body;
		std::string error;
	};

	struct tool_record
	{
		uint32_t id = 0;
		std::string name;
		Json::Value arguments;
		Json::Value results;
	};

	http_response post(const Json::Value& request, uint64_t remaining_milliseconds) const;
	Json::Value request(const Json::Value& input) const;
	Json::Value finish(const std::string& status, const std::string& conclusion,
		double confidence, const Json::Value& unresolved, uint32_t rounds,
		uint32_t calls, const Json::Value* model_evidence = nullptr) const;
	bool parse_final(const std::string& text, Json::Value& final, std::string& error) const;
	std::string response_text(const Json::Value& response) const;
	void append_response(Json::Value& input, const Json::Value& response) const;
	void debug_event(const std::string& type, const Json::Value& value) const;
	Json::Value bounded_tool_result(const agent_tool_result& result, size_t& total_bytes, bool& exhausted) const;
	Json::Value bound_output(Json::Value output) const;

	agent_runtime_options mOptions;
	const agent_tools& mTools;
	std::vector<tool_record> mRecords;
};
