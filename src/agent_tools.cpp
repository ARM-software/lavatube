#include "agent_tools.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "jsoncpp/json/reader.h"
#include "jsoncpp/json/writer.h"
#include "packfile.h"
#include "random_access_file_reader.h"
#include "read.h"
#include "read_auto.h"
#include "trace_metadata.h"
#include "util.h"
#include "util_auto.h"

struct agent_frame_boundary
{
	uint64_t position = 0;
	uint32_t frame = 0;
};

static Json::Value agent_empty_parameters()
{
	Json::Value parameters;
	parameters["type"] = "object";
	parameters["properties"] = Json::Value(Json::objectValue);
	parameters["required"] = Json::Value(Json::arrayValue);
	parameters["additionalProperties"] = false;
	return parameters;
}

static Json::Value agent_u32_property(const std::string& description)
{
	Json::Value property;
	property["type"] = "integer";
	property["minimum"] = 0;
	property["maximum"] = (Json::UInt64)UINT32_MAX;
	property["description"] = description;
	return property;
}

static Json::Value agent_string_property(const std::string& description)
{
	Json::Value property;
	property["type"] = "string";
	property["description"] = description;
	return property;
}

static Json::Value agent_function_schema(const std::string& name, const std::string& description, const Json::Value& parameters)
{
	Json::Value tool;
	tool["type"] = "function";
	tool["name"] = name;
	tool["description"] = description;
	tool["parameters"] = parameters;
	return tool;
}

static Json::Value agent_thread_parameters()
{
	Json::Value parameters = agent_empty_parameters();
	parameters["properties"]["thread"] = agent_u32_property("Trace thread index.");
	parameters["required"].append("thread");
	return parameters;
}

static Json::Value agent_frame_parameters()
{
	Json::Value parameters = agent_thread_parameters();
	parameters["properties"]["frame"] = agent_u32_property("Frame index in this trace thread.");
	parameters["required"].append("frame");
	return parameters;
}

static Json::Value agent_object_parameters()
{
	Json::Value parameters = agent_empty_parameters();
	parameters["properties"]["type"] = agent_string_property("Vulkan object type, for example VkBuffer.");
	parameters["properties"]["index"] = agent_u32_property("Object index.");
	parameters["required"].append("type");
	parameters["required"].append("index");
	return parameters;
}

static Json::Value agent_index_parameters(const std::string& description)
{
	Json::Value parameters = agent_empty_parameters();
	parameters["properties"]["command_buffer"] = agent_u32_property(description);
	parameters["required"].append("command_buffer");
	return parameters;
}

static Json::Value agent_packet_parameters()
{
	Json::Value parameters = agent_thread_parameters();
	parameters["properties"]["start"] = agent_u32_property("First packet index, inclusive.");
	parameters["properties"]["end"] = agent_u32_property("Last packet index, inclusive. Defaults to start and may span at most 64 packets.");
	parameters["required"].append("start");
	return parameters;
}

static Json::Value agent_find_parameters()
{
	Json::Value parameters = agent_empty_parameters();
	parameters["properties"]["name"] = agent_string_property("Exact Vulkan command name, for example vkQueueSubmit.");
	parameters["properties"]["thread"] = agent_u32_property("Optional trace thread index.");
	parameters["properties"]["start"] = agent_u32_property("Optional first packet index, inclusive.");
	parameters["properties"]["end"] = agent_u32_property("Optional last packet index, inclusive.");
	parameters["properties"]["limit"] = agent_u32_property("Maximum matches to return, from 1 through 128. Defaults to 20.");
	parameters["required"].append("name");
	return parameters;
}

static bool agent_arguments_only(const Json::Value& arguments, const char* first = nullptr,
	const char* second = nullptr, const char* third = nullptr, const char* fourth = nullptr,
	const char* fifth = nullptr)
{
	if (!arguments.isObject()) return false;
	for (const std::string& name : arguments.getMemberNames())
	{
		if ((!first || name != first) && (!second || name != second) && (!third || name != third)
		    && (!fourth || name != fourth) && (!fifth || name != fifth)) return false;
	}
	return true;
}

static bool agent_read_u32(const Json::Value& arguments, const char* name, uint32_t& value, std::string& error)
{
	if (!arguments.isMember(name) || !arguments[name].isUInt64() || arguments[name].asUInt64() > UINT32_MAX)
	{
		error = "Missing unsigned integer argument: " + std::string(name);
		return false;
	}
	value = (uint32_t)arguments[name].asUInt64();
	return true;
}

static bool agent_read_string(const Json::Value& arguments, const char* name, std::string& value, std::string& error)
{
	if (!arguments.isMember(name) || !arguments[name].isString() || arguments[name].asString().empty())
	{
		error = "Missing non-empty string argument: " + std::string(name);
		return false;
	}
	value = arguments[name].asString();
	return true;
}

static std::string agent_trim_response(const std::string& response)
{
	size_t end = response.size();
	while (end > 0 && (response[end - 1] == '\n' || response[end - 1] == '\r')) end--;
	return response.substr(0, end);
}

static Json::Value agent_response_value(const std::string& response)
{
	Json::Value value;
	Json::Reader reader;
	if (reader.parse(response, value, false)) return value;
	value = Json::Value(Json::objectValue);
	value["output"] = agent_trim_response(response);
	return value;
}

static bool agent_set_socket_timeout(int fd, const std::chrono::steady_clock::time_point& deadline, std::string& error)
{
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (now >= deadline)
	{
		error = "Replay service deadline expired";
		return false;
	}
	std::chrono::steady_clock::duration remaining = deadline - now;
	const std::chrono::seconds maximum(10);
	if (remaining > maximum) remaining = maximum;
	int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
	if (microseconds < 1) microseconds = 1;
	struct timeval timeout = {};
	timeout.tv_sec = microseconds / 1000000;
	timeout.tv_usec = microseconds % 1000000;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0
	    || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
	{
		error = "Failed to set replay service timeout: " + std::string(strerror(errno));
		return false;
	}
	return true;
}

static int agent_service_connect(const std::string& hostname, int port,
	const std::chrono::steady_clock::time_point& deadline, std::string& error)
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
	struct addrinfo* addresses = nullptr;
	const int gai = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &addresses);
	if (gai != 0)
	{
		error = "Failed to resolve " + hostname + ":" + service + ": " + gai_strerror(gai);
		return -1;
	}

	int fd = -1;
	int last_error = 0;
	for (struct addrinfo* address = addresses; address; address = address->ai_next)
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			error = "Replay service deadline expired";
			break;
		}
		fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (fd < 0)
		{
			last_error = errno;
			continue;
		}
		if (!agent_set_socket_timeout(fd, deadline, error))
		{
			close(fd);
			fd = -1;
			break;
		}
		if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
		last_error = errno;
		close(fd);
		fd = -1;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			error = "Replay service deadline expired";
			break;
		}
	}
	freeaddrinfo(addresses);
	if (fd < 0 && error.empty()) error = "Failed to connect to " + hostname + ":" + service + ": " + strerror(last_error);
	return fd;
}

static std::vector<agent_frame_boundary> agent_frame_boundaries(const Json::Value& frame_info)
{
	std::vector<agent_frame_boundary> boundaries;
	if (!frame_info.isMember("frames") || !frame_info["frames"].isArray()) return boundaries;
	for (const Json::Value& value : frame_info["frames"])
	{
		agent_frame_boundary boundary;
		boundary.position = value["position"].asUInt64();
		boundary.frame = value["global_frame"].asUInt() + 1;
		boundaries.push_back(boundary);
	}
	return boundaries;
}

static uint32_t agent_packet_frame(const std::vector<agent_frame_boundary>& boundaries, uint64_t position)
{
	size_t first = 0;
	size_t last = boundaries.size();
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (boundaries[middle].position <= position) first = middle + 1;
		else last = middle;
	}
	return first == 0 ? 0 : boundaries[first - 1].frame;
}

static bool agent_load_packet(random_access_file_reader& source, uint32_t target, uint32_t& current,
	uint64_t& position, std::vector<char>& packet, std::string& error)
{
	while (current <= target && source.remaining() > 0)
	{
		position = source.position();
		if (source.remaining() < sizeof(uint8_t) + sizeof(uint32_t))
		{
			error = "Truncated packet header at packet " + _to_string(current);
			return false;
		}
		(void)source.read_uint8_t();
		const uint32_t size = source.read_uint32_t();
		if (size < sizeof(uint8_t) + sizeof(uint32_t) || size > source.size() - position)
		{
			error = "Invalid packet size at packet " + _to_string(current);
			return false;
		}
		if (current == target)
		{
			packet.resize(size);
			source.seek(position);
			source.read_bytes(packet.data(), packet.size());
			return true;
		}
		source.seek(position + size);
		current++;
	}
	error = "Packet " + _to_string(target) + " does not exist";
	return false;
}

static bool agent_decode_packet(lava_reader& trace, lava_file_reader*& reader,
	const std::vector<agent_frame_boundary>& boundaries, uint32_t thread, uint32_t packet_index,
	uint64_t packet_position, uint8_t stream_version, const std::vector<char>& packet,
	Json::Value& output, std::string& error)
{
	std::vector<Json::Value> decoded;
	trace.print_json_output = &decoded;
	const uint32_t frame = agent_packet_frame(boundaries, packet_position);
	if (!reader) reader = new lava_file_reader(&trace, packet.data(), packet.size(), thread, packet_index, frame, stream_version);
	else reader->reset_fixed_packet(packet.data(), packet.size(), packet_index, frame, stream_version);
	const uint8_t type = reader->step();
	if (type == 0)
	{
		error = "Failed to decode packet " + _to_string(packet_index);
		trace.print_json_output = nullptr;
		return false;
	}
	switchboard_packet(type, *reader);
	if (type != PACKET_VULKAN_API_CALL && !reader->printed_current_packet)
	{
		callback_context context{ *reader };
		print_params_packet(context);
	}
	reader->complete_packet();
	trace.print_json_output = nullptr;
	if (decoded.size() != 1)
	{
		error = "Packet decoder produced " + _to_string(decoded.size()) + " records";
		return false;
	}
	output = decoded[0];
	return true;
}

std::string agent_json_compact(const Json::Value& value)
{
	Json::FastWriter writer;
	std::string output = writer.write(value);
	if (!output.empty() && output.back() == '\n') output.pop_back();
	return output;
}

agent_tools::agent_tools(const agent_tools_options& options)
	: mTraceFile(options.trace_file)
	, mHostname(options.hostname)
	, mPort(options.port)
	, mDeadline(options.deadline)
{
}

agent_tools::~agent_tools()
{
	delete mTrace;
}

lava_reader& agent_tools::trace_reader() const
{
	if (!mTrace)
	{
		mTrace = new lava_reader;
		mTrace->run_type = reader_run_type::isolated;
		mTrace->create_results_file = false;
		mTrace->init_metadata(mTraceFile);
	}
	return *mTrace;
}

agent_tool_result agent_tools::error_result(const std::string& error) const
{
	agent_tool_result result;
	result.error = error;
	result.result["error"] = error;
	return result;
}

agent_tool_result agent_tools::service_command(const std::string& command) const
{
	static const size_t maximum_response = 1024 * 1024;
	std::string error;
	const int fd = agent_service_connect(mHostname, mPort, mDeadline, error);
	if (fd < 0) return error_result(error);
	if (!agent_set_socket_timeout(fd, mDeadline, error))
	{
		close(fd);
		return error_result(error);
	}
	if (!lava_tcp_send_all(fd, command + "\n"))
	{
		error = std::chrono::steady_clock::now() >= mDeadline
			? "Replay service deadline expired"
			: "Failed to send replay command: " + std::string(strerror(errno));
		close(fd);
		return error_result(error);
	}
	if (!agent_set_socket_timeout(fd, mDeadline, error))
	{
		close(fd);
		return error_result(error);
	}
	const std::string response = lava_tcp_receive_all(fd, maximum_response + 1);
	close(fd);
	if (response.empty())
	{
		return error_result(std::chrono::steady_clock::now() >= mDeadline
			? "Replay service deadline expired"
			: "Replay service returned an empty response");
	}
	if (response.size() > maximum_response)
	{
		agent_tool_result result;
		result.ok = true;
		result.result["truncated"] = true;
		result.result["reason"] = "Replay service response exceeded the receive limit";
		result.result["received_bytes"] = (Json::UInt64)response.size();
		result.result["limit_bytes"] = (Json::UInt64)maximum_response;
		result.result["output_head"] = response.substr(0, 15000);
		return result;
	}
	if (response.rfind("ERROR", 0) == 0) return error_result(agent_trim_response(response));
	agent_tool_result result;
	result.ok = true;
	result.result = agent_response_value(response);
	return result;
}

bool agent_tools::validate(std::string& paused_position, std::string& error) const
{
	if (!trace_metadata_validate(mTraceFile, error)) return false;
	struct stat local = {};
	if (stat(mTraceFile.c_str(), &local) != 0)
	{
		error = "Failed to stat local trace: " + std::string(strerror(errno));
		return false;
	}
	agent_tool_result status = service_command("status");
	if (!status.ok)
	{
		error = status.error;
		return false;
	}
	paused_position = status.result["output"].asString();
	if (paused_position.rfind("PAUSED", 0) != 0)
	{
		error = "Replay service is not paused: " + paused_position;
		return false;
	}
	agent_tool_result identity = service_command("info trace");
	if (!identity.ok || !identity.result.isObject() || !identity.result.isMember("device") || !identity.result.isMember("inode"))
	{
		error = identity.ok ? "Replay service did not report trace filesystem identity" : identity.error;
		return false;
	}
	if (identity.result["device"].asUInt64() != (uint64_t)local.st_dev || identity.result["inode"].asUInt64() != (uint64_t)local.st_ino)
	{
		error = "Local trace and replay service trace are different filesystem objects";
		return false;
	}
	return true;
}

bool agent_tools::stable_at(const std::string& paused_position, std::string& error) const
{
	agent_tool_result status = service_command("status");
	if (!status.ok)
	{
		error = status.error;
		return false;
	}
	const std::string current = status.result["output"].asString();
	if (current.rfind("PAUSED", 0) != 0 || current != paused_position)
	{
		error = "Replay position changed from '" + paused_position + "' to '" + current + "'";
		return false;
	}
	return true;
}

Json::Value agent_tools::definitions() const
{
	Json::Value tools(Json::arrayValue);
	tools.append(agent_function_schema("trace_get_metadata", "Return captured metadata.json from the local trace.", agent_empty_parameters()));
	tools.append(agent_function_schema("trace_list_threads", "List trace threads and their immutable metadata summary.", agent_empty_parameters()));
	tools.append(agent_function_schema("trace_get_thread", "Return immutable metadata for one trace thread.", agent_thread_parameters()));
	tools.append(agent_function_schema("trace_get_frame", "Return immutable metadata for one frame in one trace thread.", agent_frame_parameters()));
	tools.append(agent_function_schema("trace_list_objects", "List locally captured Vulkan object types and creation counts.", agent_empty_parameters()));
	tools.append(agent_function_schema("trace_get_object", "Return immutable tracking metadata for one captured Vulkan object.", agent_object_parameters()));
	tools.append(agent_function_schema("trace_get_packets", "Decode one packet or an inclusive range of at most 64 packets from one trace thread.", agent_packet_parameters()));
	tools.append(agent_function_schema("trace_find_calls", "Find exact Vulkan command names in the local trace with optional thread and packet bounds.", agent_find_parameters()));
	tools.append(agent_function_schema("replay_get_status", "Return the live replay service status without changing it.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_list_threads", "Return current replay thread positions and waits.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_diagnose_deadlock", "Diagnose current replay waits and possible deadlocks.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_diagnose_device", "Query the current Vulkan device health while replay remains paused.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_get_memory", "Return current Vulkan memory heap usage and budgets.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_get_suballocator", "Return current replay suballocator state.", agent_empty_parameters()));
	tools.append(agent_function_schema("replay_get_object_state", "Return current tracked state for one replay Vulkan object.", agent_object_parameters()));
	tools.append(agent_function_schema("replay_get_instrumentation", "Return cached instrumentation for a command buffer.", agent_index_parameters("Command buffer index.")));
	tools.append(agent_function_schema("replay_get_as_build", "Return cached acceleration-structure build diagnostics for a command buffer.", agent_index_parameters("Command buffer index.")));
	return tools;
}

agent_tool_result agent_tools::execute(const std::string& name, const Json::Value& arguments) const
{
	if (name == "trace_get_metadata") return trace_get_metadata(arguments);
	if (name == "trace_list_threads") return trace_list_threads(arguments);
	if (name == "trace_get_thread") return trace_get_thread(arguments);
	if (name == "trace_get_frame") return trace_get_frame(arguments);
	if (name == "trace_list_objects") return trace_list_objects(arguments);
	if (name == "trace_get_object") return trace_get_object(arguments);
	if (name == "trace_get_packets") return trace_get_packets(arguments);
	if (name == "trace_find_calls") return trace_find_calls(arguments);
	if (name == "replay_get_status") return replay_command(arguments, "status");
	if (name == "replay_list_threads") return replay_command(arguments, "info threads");
	if (name == "replay_diagnose_deadlock") return replay_command(arguments, "diagnose deadlock");
	if (name == "replay_diagnose_device") return replay_command(arguments, "diagnose device");
	if (name == "replay_get_memory") return replay_command(arguments, "info memory");
	if (name == "replay_get_suballocator") return replay_command(arguments, "info suballocator");
	if (name == "replay_get_object_state") return replay_get_object_state(arguments);
	if (name == "replay_get_instrumentation") return replay_get_indexed_state(arguments, "show instrumentation ");
	if (name == "replay_get_as_build") return replay_get_indexed_state(arguments, "show as-build ");
	return error_result("Unknown or disallowed tool: " + name);
}

agent_tool_result agent_tools::trace_get_metadata(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments)) return error_result("trace_get_metadata takes no arguments");
	agent_tool_result result;
	result.ok = true;
	result.result = packed_json("metadata.json", mTraceFile);
	return result;
}

agent_tool_result agent_tools::trace_list_threads(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments)) return error_result("trace_list_threads takes no arguments");
	const Json::Value metadata = packed_json("metadata.json", mTraceFile);
	const uint32_t count = metadata["threads"].asUInt();
	Json::Value threads(Json::arrayValue);
	for (uint32_t thread = 0; thread < count; thread++)
	{
		const Json::Value info = packed_json("frames_" + _to_string(thread) + ".json", mTraceFile);
		Json::Value summary;
		summary["thread"] = thread;
		if (info.isMember("thread_name")) summary["name"] = info["thread_name"];
		summary["frames"] = info.isMember("frames") && info["frames"].isArray() ? (Json::UInt)info["frames"].size() : 0;
		if (info.isMember("uncompressed_size")) summary["uncompressed_size"] = info["uncompressed_size"];
		threads.append(summary);
	}
	agent_tool_result result;
	result.ok = true;
	result.result["threads"] = threads;
	return result;
}

agent_tool_result agent_tools::trace_get_thread(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "thread")) return error_result("Unexpected argument for trace_get_thread");
	uint32_t thread = 0;
	std::string error;
	if (!agent_read_u32(arguments, "thread", thread, error)) return error_result(error);
	agent_tool_result result;
	result.ok = trace_metadata_thread_json(mTraceFile, thread, result.result, result.error);
	return result.ok ? result : error_result(result.error);
}

agent_tool_result agent_tools::trace_get_frame(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "thread", "frame")) return error_result("Unexpected argument for trace_get_frame");
	uint32_t thread = 0;
	uint32_t frame = 0;
	std::string error;
	if (!agent_read_u32(arguments, "thread", thread, error) || !agent_read_u32(arguments, "frame", frame, error)) return error_result(error);
	agent_tool_result result;
	result.ok = trace_metadata_frame_json(mTraceFile, thread, frame, result.result, result.error);
	return result.ok ? result : error_result(result.error);
}

agent_tool_result agent_tools::trace_list_objects(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments)) return error_result("trace_list_objects takes no arguments");
	const Json::Value limits = packed_json("limits.json", mTraceFile);
	Json::Value objects(Json::arrayValue);
	for (const std::string& type : limits.getMemberNames())
	{
		if (!limits[type].isNumeric() || limits[type].asUInt64() == 0) continue;
		Json::Value object;
		object["type"] = type;
		object["count"] = limits[type];
		objects.append(object);
	}
	agent_tool_result result;
	result.ok = true;
	result.result["objects"] = objects;
	return result;
}

agent_tool_result agent_tools::trace_get_object(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "type", "index")) return error_result("Unexpected argument for trace_get_object");
	std::string type;
	std::string error;
	uint32_t index = 0;
	if (!agent_read_string(arguments, "type", type, error) || !agent_read_u32(arguments, "index", index, error)) return error_result(error);
	const Json::Value tracking = packed_json("tracking.json", mTraceFile);
	if (!tracking.isMember(type) || !tracking[type].isArray()) return error_result("Unknown trace object type: " + type);
	if (index >= tracking[type].size()) return error_result("Trace object index is out of range");
	agent_tool_result result;
	result.ok = true;
	result.result = tracking[type][index];
	return result;
}

agent_tool_result agent_tools::trace_get_packets(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "thread", "start", "end")) return error_result("Unexpected argument for trace_get_packets");
	uint32_t thread = 0;
	uint32_t start = 0;
	uint32_t end = 0;
	std::string error;
	if (!agent_read_u32(arguments, "thread", thread, error) || !agent_read_u32(arguments, "start", start, error)) return error_result(error);
	end = start;
	if (arguments.isMember("end") && !agent_read_u32(arguments, "end", end, error)) return error_result(error);
	if (end < start || (uint64_t)end - start + 1 > 64) return error_result("Packet range must contain between 1 and 64 packets");
	const Json::Value metadata = packed_json("metadata.json", mTraceFile);
	if (thread >= metadata["threads"].asUInt()) return error_result("Trace thread is out of range");

	const Json::Value frame_info = packed_json("frames_" + _to_string(thread) + ".json", mTraceFile);
	const std::vector<agent_frame_boundary> boundaries = agent_frame_boundaries(frame_info);
	random_access_file_reader source(packed_open("thread_" + _to_string(thread) + ".bin", mTraceFile));
	source.load_packet_checkpoints(frame_info);
	uint32_t current = source.seek_to_packet(start);
	std::vector<char> packet;
	uint64_t position = 0;
	lava_reader& trace = trace_reader();
	trace.print_packets = true;
	trace.print_thread_index = thread;
	lava_file_reader* packet_reader = nullptr;
	Json::Value packets(Json::arrayValue);
	for (uint32_t index = start; index <= end; index++)
	{
		if (!agent_load_packet(source, index, current, position, packet, error))
		{
			delete packet_reader;
			return error_result(error);
		}
		Json::Value decoded;
		if (!agent_decode_packet(trace, packet_reader, boundaries, thread, index, position, source.version(), packet, decoded, error))
		{
			delete packet_reader;
			return error_result(error);
		}
		packets.append(decoded);
		current = index + 1;
		if (index == UINT32_MAX) break;
	}
	delete packet_reader;
	agent_tool_result result;
	result.ok = true;
	result.result["packets"] = packets;
	return result;
}

agent_tool_result agent_tools::trace_find_calls(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "name", "thread", "start", "end", "limit")) return error_result("Unexpected argument for trace_find_calls");
	std::string name;
	std::string error;
	if (!agent_read_string(arguments, "name", name, error)) return error_result(error);
	const uint16_t wanted = retrace_getid(name.c_str());
	if (wanted == UINT16_MAX || name.rfind("vk", 0) != 0) return error_result("Unknown exact Vulkan command name: " + name);
	uint32_t start = 0;
	uint32_t end = UINT32_MAX;
	uint32_t limit = 20;
	if (arguments.isMember("start") && !agent_read_u32(arguments, "start", start, error)) return error_result(error);
	if (arguments.isMember("end") && !agent_read_u32(arguments, "end", end, error)) return error_result(error);
	if (arguments.isMember("limit") && !agent_read_u32(arguments, "limit", limit, error)) return error_result(error);
	if (end < start) return error_result("End packet precedes start packet");
	if (limit == 0 || limit > 128) return error_result("Find limit must be from 1 through 128");
	const Json::Value metadata = packed_json("metadata.json", mTraceFile);
	const uint32_t thread_count = metadata["threads"].asUInt();
	uint32_t first_thread = 0;
	uint32_t last_thread = thread_count == 0 ? 0 : thread_count - 1;
	if (arguments.isMember("thread"))
	{
		if (!agent_read_u32(arguments, "thread", first_thread, error)) return error_result(error);
		if (first_thread >= thread_count) return error_result("Trace thread is out of range");
		last_thread = first_thread;
	}

	lava_reader& trace = trace_reader();
	Json::Value matches(Json::arrayValue);
	bool truncated = false;
	for (uint32_t thread = first_thread; thread <= last_thread; thread++)
	{
		const Json::Value frame_info = packed_json("frames_" + _to_string(thread) + ".json", mTraceFile);
		const std::vector<agent_frame_boundary> boundaries = agent_frame_boundaries(frame_info);
		random_access_file_reader source(packed_open("thread_" + _to_string(thread) + ".bin", mTraceFile));
		source.load_packet_checkpoints(frame_info);
		uint32_t packet = source.seek_to_packet(start);
		while (packet <= end && source.remaining() > 0)
		{
			const uint64_t position = source.position();
			if (source.remaining() < sizeof(uint8_t) + sizeof(uint32_t)) return error_result("Truncated packet header");
			const uint8_t type = source.read_uint8_t();
			const uint32_t size = source.read_uint32_t();
			if (size < sizeof(uint8_t) + sizeof(uint32_t) || size > source.size() - position) return error_result("Invalid packet size");
			if (type == PACKET_VULKAN_API_CALL && size >= sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint16_t))
			{
				const uint16_t stored = source.read_uint16_t();
				const auto found = trace.vulkan_dictionary.find(stored);
				if (found != trace.vulkan_dictionary.end() && found->second == wanted)
				{
					Json::Value match;
					match["thread"] = thread;
					match["packet"] = packet;
					match["frame"] = agent_packet_frame(boundaries, position);
					match["name"] = name;
					matches.append(match);
					if (matches.size() >= limit)
					{
						truncated = true;
						break;
					}
				}
			}
			source.seek(position + size);
			if (packet == UINT32_MAX) break;
			packet++;
		}
		if (truncated || thread == UINT32_MAX) break;
	}
	agent_tool_result result;
	result.ok = true;
	result.result["matches"] = matches;
	result.result["truncated"] = truncated;
	return result;
}

agent_tool_result agent_tools::replay_command(const Json::Value& arguments, const std::string& command) const
{
	if (!agent_arguments_only(arguments)) return error_result("Replay tool takes no arguments");
	return service_command(command);
}

agent_tool_result agent_tools::replay_get_object_state(const Json::Value& arguments) const
{
	if (!agent_arguments_only(arguments, "type", "index")) return error_result("Unexpected argument for replay_get_object_state");
	std::string type;
	std::string error;
	uint32_t index = 0;
	if (!agent_read_string(arguments, "type", type, error) || !agent_read_u32(arguments, "index", index, error)) return error_result(error);
	if (type.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
	{
		return error_result("Invalid Vulkan object type");
	}
	return service_command("show " + type + " " + _to_string(index));
}

agent_tool_result agent_tools::replay_get_indexed_state(const Json::Value& arguments, const std::string& command) const
{
	if (!agent_arguments_only(arguments, "command_buffer")) return error_result("Unexpected argument for replay diagnostic tool");
	uint32_t index = 0;
	std::string error;
	if (!agent_read_u32(arguments, "command_buffer", index, error)) return error_result(error);
	return service_command(command + _to_string(index));
}
