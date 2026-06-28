#pragma once

#include <string>
#include <stdint.h>
#include <vector>

#include "jsoncpp/json/value.h"

struct tui_trace_tools_options
{
	std::string trace_file;
	std::string hostname = "localhost";
	int port = 11901;
	bool replay_service = false;
};

struct tui_tool_result
{
	bool ok = false;
	std::string output;
	std::string error;
};

class tui_trace_tools
{
public:
	explicit tui_trace_tools(const std::string& trace_file);
	explicit tui_trace_tools(const tui_trace_tools_options& options);

	bool validate(std::string& error) const;
	Json::Value tool_definitions() const;
	tui_tool_result execute(const std::string& name, const std::string& arguments) const;

	const std::string& trace_file() const { return mTraceFile; }
	bool replay_service() const { return mReplayService; }
	std::string source_label() const;

private:
	tui_tool_result list_objects_created() const;
	tui_tool_result get_frame_meta(const Json::Value& args) const;
	tui_tool_result get_thread_meta(const Json::Value& args) const;
	tui_tool_result get_object_meta(const Json::Value& args) const;
	tui_tool_result list_threads() const;
	tui_tool_result get_service_info() const;
	tui_tool_result get_memory_info() const;
	tui_tool_result get_suballocator_info() const;
	tui_tool_result get_service_status() const;
	tui_tool_result get_current_call_parameters() const;
	tui_tool_result continue_replay() const;
	tui_tool_result stop_replay() const;
	tui_tool_result step_replay(const Json::Value& args) const;
	tui_tool_result goto_replay_target(const Json::Value& args) const;
	tui_tool_result service_command(const std::string& command) const;

	bool parse_arguments(const std::string& arguments, Json::Value& args, std::string& error) const;
	bool read_u32_arg(const Json::Value& args, const char* name, uint32_t& value, std::string& error) const;
	bool read_string_arg(const Json::Value& args, const char* name, std::string& value, std::string& error) const;
	tui_tool_result json_error(const std::string& message) const;

	std::string mTraceFile;
	std::string mHostname = "localhost";
	int mPort = 11901;
	bool mReplayService = false;
};

std::string tui_json_compact(const Json::Value& value);
std::string tui_json_pretty(const Json::Value& value);
std::string tui_trim_tool_output(const std::string& value, size_t max_size);
