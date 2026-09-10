#pragma once

#include <stdint.h>

#include <chrono>
#include <string>

#include "jsoncpp/json/value.h"

class lava_reader;

struct agent_tools_options
{
	std::string trace_file;
	std::string hostname;
	int port = 0;
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

struct agent_tool_result
{
	bool ok = false;
	Json::Value result;
	std::string error;
};

class agent_tools
{
public:
	explicit agent_tools(const agent_tools_options& options);
	~agent_tools();

	bool validate(std::string& paused_position, std::string& error) const;
	bool stable_at(const std::string& paused_position, std::string& error) const;
	Json::Value definitions() const;
	agent_tool_result execute(const std::string& name, const Json::Value& arguments) const;

private:
	agent_tool_result trace_get_metadata(const Json::Value& arguments) const;
	agent_tool_result trace_list_threads(const Json::Value& arguments) const;
	agent_tool_result trace_get_thread(const Json::Value& arguments) const;
	agent_tool_result trace_get_frame(const Json::Value& arguments) const;
	agent_tool_result trace_list_objects(const Json::Value& arguments) const;
	agent_tool_result trace_get_object(const Json::Value& arguments) const;
	agent_tool_result trace_get_packets(const Json::Value& arguments) const;
	agent_tool_result trace_find_calls(const Json::Value& arguments) const;
	agent_tool_result replay_command(const Json::Value& arguments, const std::string& command) const;
	agent_tool_result replay_get_object_state(const Json::Value& arguments) const;
	agent_tool_result replay_get_indexed_state(const Json::Value& arguments, const std::string& command) const;
	agent_tool_result service_command(const std::string& command) const;
	agent_tool_result error_result(const std::string& error) const;
	lava_reader& trace_reader() const;

	std::string mTraceFile;
	std::string mHostname;
	int mPort = 0;
	std::chrono::steady_clock::time_point mDeadline;
	mutable lava_reader* mTrace = nullptr;
};

std::string agent_json_compact(const Json::Value& value);
