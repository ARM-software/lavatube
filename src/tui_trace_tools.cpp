#include "tui_trace_tools.h"

#include "jsoncpp/json/reader.h"
#include "packfile.h"
#include "trace_metadata.h"
#include "util.h"

static Json::Value function_tool_schema(const std::string& name, const std::string& description, const Json::Value& parameters)
{
	Json::Value tool;
	tool["type"] = "function";
	tool["name"] = name;
	tool["description"] = description;
	tool["parameters"] = parameters;
	return tool;
}

static Json::Value empty_parameters()
{
	Json::Value parameters;
	parameters["type"] = "object";
	parameters["properties"] = Json::Value(Json::objectValue);
	parameters["required"] = Json::Value(Json::arrayValue);
	parameters["additionalProperties"] = false;
	return parameters;
}

static Json::Value u32_property(const std::string& description)
{
	Json::Value property;
	property["type"] = "integer";
	property["minimum"] = 0;
	property["description"] = description;
	return property;
}

static Json::Value parameters_with_thread()
{
	Json::Value parameters = empty_parameters();
	parameters["properties"]["thread"] = u32_property("Trace thread index.");
	parameters["required"].append("thread");
	return parameters;
}

static Json::Value parameters_with_thread_frame()
{
	Json::Value parameters = parameters_with_thread();
	parameters["properties"]["frame"] = u32_property("Frame index within the selected thread metadata array.");
	parameters["required"].append("frame");
	return parameters;
}

static Json::Value parameters_with_object()
{
	Json::Value parameters = empty_parameters();
	Json::Value type;
	type["type"] = "string";
	type["description"] = "Vulkan object type key from tracking.json, for example VkBuffer or VkImage.";
	parameters["properties"]["type"] = type;
	parameters["properties"]["index"] = u32_property("Object index inside tracking.json for the selected type.");
	parameters["required"].append("type");
	parameters["required"].append("index");
	return parameters;
}

std::string tui_json_compact(const Json::Value& value)
{
	return trace_metadata_json_compact(value);
}

std::string tui_json_pretty(const Json::Value& value)
{
	return trace_metadata_json_pretty(value);
}

std::string tui_trim_tool_output(const std::string& value, size_t max_size)
{
	if (value.size() <= max_size) return value;
	Json::Value out;
	out["truncated"] = true;
	out["original_bytes"] = (Json::UInt64)value.size();
	out["head"] = value.substr(0, max_size);
	return tui_json_compact(out);
}

tui_trace_tools::tui_trace_tools(const std::string& trace_file)
	: mTraceFile(trace_file)
{
}

bool tui_trace_tools::validate(std::string& error) const
{
	return trace_metadata_validate(mTraceFile, error);
}

Json::Value tui_trace_tools::tool_definitions() const
{
	Json::Value tools(Json::arrayValue);
	tools.append(function_tool_schema("list_objects_created", "List Vulkan object types with non-zero creation counts from limits.json as TSV.", empty_parameters()));
	tools.append(function_tool_schema("get_frame_meta", "Return metadata for one frame from frames_<thread>.json.", parameters_with_thread_frame()));
	tools.append(function_tool_schema("get_thread_meta", "Return per-thread metadata from frames_<thread>.json, excluding the large frames array.", parameters_with_thread()));
	tools.append(function_tool_schema("get_object_meta", "Return metadata for one object from tracking.json.", parameters_with_object()));
	return tools;
}

tui_tool_result tui_trace_tools::execute(const std::string& name, const std::string& arguments) const
{
	Json::Value args;
	std::string error;
	if (!parse_arguments(arguments, args, error))
	{
		return json_error(error);
	}

	if (name == "list_objects_created") return list_objects_created();
	if (name == "get_frame_meta") return get_frame_meta(args);
	if (name == "get_thread_meta") return get_thread_meta(args);
	if (name == "get_object_meta") return get_object_meta(args);

	return json_error("Unknown tool: " + name);
}

bool tui_trace_tools::parse_arguments(const std::string& arguments, Json::Value& args, std::string& error) const
{
	if (arguments.empty())
	{
		args = Json::Value(Json::objectValue);
		return true;
	}

	Json::Reader reader;
	if (!reader.parse(arguments, args, false))
	{
		error = "Tool arguments are not valid JSON: " + reader.getFormattedErrorMessages();
		return false;
	}
	if (!args.isObject())
	{
		error = "Tool arguments must be a JSON object";
		return false;
	}
	return true;
}

bool tui_trace_tools::read_u32_arg(const Json::Value& args, const char* name, uint32_t& value, std::string& error) const
{
	if (!args.isMember(name) || !args[name].isUInt())
	{
		error = "Missing unsigned integer argument: " + std::string(name);
		return false;
	}
	value = args[name].asUInt();
	return true;
}

tui_tool_result tui_trace_tools::json_error(const std::string& message) const
{
	Json::Value out;
	out["ok"] = false;
	out["error"] = message;

	tui_tool_result result;
	result.ok = false;
	result.error = message;
	result.output = tui_json_compact(out);
	return result;
}

tui_tool_result tui_trace_tools::list_objects_created() const
{
	tui_tool_result result;
	result.ok = true;
	result.output = trace_metadata_objects_tsv(mTraceFile);
	return result;
}

tui_tool_result tui_trace_tools::get_frame_meta(const Json::Value& args) const
{
	uint32_t thread = 0;
	uint32_t frame = 0;
	std::string error;
	if (!read_u32_arg(args, "thread", thread, error)) return json_error(error);
	if (!read_u32_arg(args, "frame", frame, error)) return json_error(error);

	Json::Value frameinfo;
	if (!trace_metadata_frame_json(mTraceFile, thread, frame, frameinfo, error)) return json_error(error);

	tui_tool_result result;
	result.ok = true;
	result.output = tui_json_compact(frameinfo);
	return result;
}

tui_tool_result tui_trace_tools::get_thread_meta(const Json::Value& args) const
{
	uint32_t thread = 0;
	std::string error;
	if (!read_u32_arg(args, "thread", thread, error)) return json_error(error);

	Json::Value frameinfo;
	if (!trace_metadata_thread_json(mTraceFile, thread, frameinfo, error)) return json_error(error);

	tui_tool_result result;
	result.ok = true;
	result.output = tui_json_compact(frameinfo);
	return result;
}

tui_tool_result tui_trace_tools::get_object_meta(const Json::Value& args) const
{
	if (!args.isMember("type") || !args["type"].isString()) return json_error("Missing string argument: type");

	uint32_t index = 0;
	std::string error;
	if (!read_u32_arg(args, "index", index, error)) return json_error(error);

	const std::string type = args["type"].asString();
	Json::Value tracking = packed_json("tracking.json", mTraceFile);
	if (!tracking.isMember(type)) return json_error("tracking.json has no object type " + type);
	if (!tracking[type].isArray()) return json_error("tracking.json entry for " + type + " is not an array");
	if (index >= tracking[type].size()) return json_error("Object index " + _to_string(index) + " is out of range for " + type);

	tui_tool_result result;
	result.ok = true;
	result.output = tui_json_compact(tracking[type][index]);
	return result;
}
