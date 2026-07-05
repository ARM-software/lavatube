#include "tui_trace_tools.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static Json::Value string_property(const std::string& description)
{
	Json::Value property;
	property["type"] = "string";
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

static Json::Value parameters_with_step()
{
	Json::Value parameters = empty_parameters();
	Json::Value unit;
	unit["type"] = "string";
	unit["description"] = "Step unit. Use packets for raw trace packets or calls for Vulkan API calls.";
	unit["enum"].append("packets");
	unit["enum"].append("calls");
	parameters["properties"]["unit"] = unit;
	parameters["properties"]["count"] = u32_property("Number of packets or calls to step. Use 1 for a single step.");
	parameters["required"].append("unit");
	parameters["required"].append("count");
	return parameters;
}

static Json::Value parameters_with_target()
{
	Json::Value parameters = empty_parameters();
	parameters["properties"]["target"] = string_property("Absolute packet number or Vulkan command name, for example 300 or vkQueueSubmit.");
	parameters["required"].append("target");
	return parameters;
}

static int tui_service_connect(const std::string& hostname, int port, std::string& error)
{
	if (port <= 0 || port > 65535)
	{
		error = "Invalid TCP port " + _to_string(port);
		return -1;
	}

	const std::string service = _to_string(port);
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* result = nullptr;
	const int gai = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &result);
	if (gai != 0)
	{
		error = "Failed to resolve " + hostname + ":" + service + ": " + gai_strerror(gai);
		return -1;
	}

	int last_error = 0;
	int fd = -1;
	for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
	{
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
		{
			last_error = errno;
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
		{
			break;
		}
		last_error = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(result);

	if (fd < 0)
	{
		error = "Failed to connect to " + hostname + ":" + service + ": " + strerror(last_error);
	}
	return fd;
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

tui_trace_tools::tui_trace_tools(const tui_trace_tools_options& options)
	: mTraceFile(options.trace_file)
	, mHostname(options.hostname)
	, mPort(options.port)
	, mReplayService(options.replay_service)
{
}

bool tui_trace_tools::validate(std::string& error) const
{
	if (mReplayService)
	{
		tui_tool_result result = service_command("status");
		if (result.ok) return true;
		error = result.error;
		return false;
	}
	return trace_metadata_validate(mTraceFile, error);
}

Json::Value tui_trace_tools::tool_definitions() const
{
	Json::Value tools(Json::arrayValue);
	if (mReplayService)
	{
		tools.append(function_tool_schema("get_service_status", "Return the current replay service state: RUNNING, DONE, PAUSED, or the current paused packet/call.", empty_parameters()));
		tools.append(function_tool_schema("continue_replay", "Resume replay until completion, stop, or the next target. Use only when the user wants replay to continue.", empty_parameters()));
		tools.append(function_tool_schema("stop_replay", "Stop the replay service and replay. Use only when the user explicitly asks to stop the service.", empty_parameters()));
		tools.append(function_tool_schema("step_replay", "Advance replay by packets or Vulkan API calls from the current pause point.", parameters_with_step()));
		tools.append(function_tool_schema("goto_replay_target", "Continue replay until an absolute packet number or the next named Vulkan command.", parameters_with_target()));
		tools.append(function_tool_schema("get_current_call_parameters", "Print JSON parameters for the currently paused Vulkan call.", empty_parameters()));
		tools.append(function_tool_schema("list_threads", "List traced threads from the replay service.", empty_parameters()));
		tools.append(function_tool_schema("get_memory_info", "Print current Vulkan memory heap usage and budgets from the replay service.", empty_parameters()));
		tools.append(function_tool_schema("get_suballocator_info", "Print current suballocator heap internals from the replay service as a Markdown table.", empty_parameters()));
		tools.append(function_tool_schema("get_service_info", "Print general replay service information.", empty_parameters()));
	}
	tools.append(function_tool_schema("list_objects_created", "List Vulkan object types with non-zero creation counts from limits.json as a table.", empty_parameters()));
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
	if (mReplayService && name == "list_threads") return list_threads();
	if (mReplayService && name == "get_service_info") return get_service_info();
	if (mReplayService && name == "get_memory_info") return get_memory_info();
	if (mReplayService && name == "get_suballocator_info") return get_suballocator_info();
	if (mReplayService && name == "get_service_status") return get_service_status();
	if (mReplayService && name == "get_current_call_parameters") return get_current_call_parameters();
	if (mReplayService && name == "continue_replay") return continue_replay();
	if (mReplayService && name == "stop_replay") return stop_replay();
	if (mReplayService && name == "step_replay") return step_replay(args);
	if (mReplayService && name == "goto_replay_target") return goto_replay_target(args);

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

bool tui_trace_tools::read_string_arg(const Json::Value& args, const char* name, std::string& value, std::string& error) const
{
	if (!args.isMember(name) || !args[name].isString())
	{
		error = "Missing string argument: " + std::string(name);
		return false;
	}
	value = args[name].asString();
	if (value.empty())
	{
		error = "Argument must not be empty: " + std::string(name);
		return false;
	}
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
	if (mReplayService) return service_command("info objects");

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

	if (mReplayService) return service_command("info frame " + _to_string(thread) + " " + _to_string(frame));

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

	if (mReplayService) return service_command("info thread " + _to_string(thread));

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
	if (mReplayService) return service_command("show " + type + " " + _to_string(index));

	Json::Value tracking = packed_json("tracking.json", mTraceFile);
	if (!tracking.isMember(type)) return json_error("tracking.json has no object type " + type);
	if (!tracking[type].isArray()) return json_error("tracking.json entry for " + type + " is not an array");
	if (index >= tracking[type].size()) return json_error("Object index " + _to_string(index) + " is out of range for " + type);

	tui_tool_result result;
	result.ok = true;
	result.output = tui_json_compact(tracking[type][index]);
	return result;
}

tui_tool_result tui_trace_tools::list_threads() const
{
	return service_command("info threads");
}

tui_tool_result tui_trace_tools::get_service_info() const
{
	return service_command("info");
}

tui_tool_result tui_trace_tools::get_memory_info() const
{
	return service_command("info memory");
}

tui_tool_result tui_trace_tools::get_suballocator_info() const
{
	return service_command("info suballocator");
}

tui_tool_result tui_trace_tools::get_service_status() const
{
	return service_command("status");
}

tui_tool_result tui_trace_tools::get_current_call_parameters() const
{
	return service_command("parameters");
}

tui_tool_result tui_trace_tools::continue_replay() const
{
	return service_command("continue");
}

tui_tool_result tui_trace_tools::stop_replay() const
{
	return service_command("stop");
}

tui_tool_result tui_trace_tools::step_replay(const Json::Value& args) const
{
	std::string unit;
	std::string error;
	if (!read_string_arg(args, "unit", unit, error)) return json_error(error);
	if (unit != "packets" && unit != "calls") return json_error("Step unit must be packets or calls");

	uint32_t count = 0;
	if (!read_u32_arg(args, "count", count, error)) return json_error(error);
	if (count == 0) return json_error("Step count must be greater than zero");

	if (count == 1) return service_command("step " + unit + " 1");
	return service_command("step " + unit + " " + _to_string(count));
}

tui_tool_result tui_trace_tools::goto_replay_target(const Json::Value& args) const
{
	std::string target;
	std::string error;
	if (!read_string_arg(args, "target", target, error)) return json_error(error);
	if (target.find(' ') != std::string::npos || target.find('\n') != std::string::npos || target.find('\r') != std::string::npos)
	{
		return json_error("Goto target must not contain whitespace");
	}
	return service_command("goto " + target);
}

tui_tool_result tui_trace_tools::service_command(const std::string& command) const
{
	std::string error;
	const int fd = tui_service_connect(mHostname, mPort, error);
	if (fd < 0) return json_error(error);

	if (!lava_tcp_send_all(fd, command + "\n"))
	{
		error = "Failed to send command to " + mHostname + ":" + _to_string(mPort) + ": " + strerror(errno);
		close(fd);
		return json_error(error);
	}

	const std::string response = lava_tcp_receive_all(fd);
	close(fd);

	if (response.empty())
	{
		return json_error("Replay service returned an empty response for command: " + command);
	}
	if (response == "ERROR\n" || response == "ERROR")
	{
		return json_error("Replay service rejected command: " + command);
	}

	tui_tool_result result;
	result.ok = true;
	result.output = response;
	return result;
}

std::string tui_trace_tools::source_label() const
{
	if (mReplayService) return "service=" + mHostname + ":" + _to_string(mPort);
	return "trace=" + mTraceFile;
}
