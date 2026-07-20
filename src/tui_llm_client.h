#pragma once

#include <string>
#include <vector>

#include "tui_trace_tools.h"

struct tui_chat_message
{
	std::string role;
	std::string content;
};

struct tui_tool_notice
{
	std::string name;
	std::string call_id;
	std::string arguments;
	std::string output;
	bool ok = false;
};

struct tui_assistant_result
{
	bool ok = false;
	std::string text;
	std::string error;
	std::string usage;
	std::vector<tui_tool_notice> tools;
};

class tui_llm_client
{
public:
	tui_llm_client(const std::string& api_key, const std::string& model, const std::string& base_url, const std::string& reasoning_effort);

	bool configured() const { return !mApiKey.empty(); }
	const std::string& model() const { return mModel; }
	const std::string& base_url() const { return mBaseUrl; }
	const std::string& reasoning_effort() const { return mReasoningEffort; }

	tui_assistant_result ask(const std::vector<tui_chat_message>& history, const tui_trace_tools& tools) const;

private:
	struct response_data
	{
		bool ok = false;
		long http_code = 0;
		std::string body;
		std::string error;
	};

	response_data post_json(const Json::Value& request) const;
	Json::Value build_initial_request(const std::vector<tui_chat_message>& history, const Json::Value& tool_definitions) const;
	Json::Value build_tool_result_request(const Json::Value& input, const Json::Value& tool_definitions) const;
	bool parse_response_json(const response_data& response, Json::Value& root, tui_assistant_result& result) const;
	std::string collect_output_text(const Json::Value& root) const;
	std::string collect_usage(const Json::Value& root) const;
	bool collect_tool_calls(const Json::Value& root, std::vector<tui_tool_notice>& calls) const;
	void append_response_output(Json::Value& input, const Json::Value& root) const;
	void append_tool_outputs(Json::Value& input, const std::vector<tui_tool_notice>& notices) const;

	std::string mApiKey;
	std::string mModel;
	std::string mBaseUrl;
	std::string mReasoningEffort;
};
