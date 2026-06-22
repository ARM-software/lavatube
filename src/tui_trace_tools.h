#pragma once

#include <string>
#include <vector>

#include "jsoncpp/json/value.h"

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

	bool validate(std::string& error) const;
	Json::Value tool_definitions() const;
	tui_tool_result execute(const std::string& name, const std::string& arguments) const;

	const std::string& trace_file() const { return mTraceFile; }

private:
	tui_tool_result list_objects_created() const;
	tui_tool_result get_frame_meta(const Json::Value& args) const;
	tui_tool_result get_thread_meta(const Json::Value& args) const;
	tui_tool_result get_object_meta(const Json::Value& args) const;

	bool parse_arguments(const std::string& arguments, Json::Value& args, std::string& error) const;
	bool read_u32_arg(const Json::Value& args, const char* name, uint32_t& value, std::string& error) const;
	tui_tool_result json_error(const std::string& message) const;

	std::string mTraceFile;
};

std::string tui_json_compact(const Json::Value& value);
std::string tui_json_pretty(const Json::Value& value);
std::string tui_trim_tool_output(const std::string& value, size_t max_size);
