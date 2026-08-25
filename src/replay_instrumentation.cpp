#include "replay_instrumentation.h"

#include <cstring>
#include <sstream>
#include <vector>

#include "jsoncpp/json/value.h"
#include "jsoncpp/json/reader.h"
#include "jsoncpp/json/writer.h"
#include "read_auto.h"
#include "tostring.h"

static trackedcmdbuffer::shader_instrumentation_session* active_instrumentation_session(trackedcmdbuffer& commandbuffer_data)
{
	if (commandbuffer_data.shader_instrumentation_sessions.empty()) return nullptr;
	trackedcmdbuffer::shader_instrumentation_session& session = commandbuffer_data.shader_instrumentation_sessions.back();
	return session.recording ? &session : nullptr;
}

static trackedcmdbuffer::nvidia_marker_session* active_nvidia_marker_session(trackedcmdbuffer& commandbuffer_data)
{
	if (commandbuffer_data.nvidia_marker_sessions.empty()) return nullptr;
	trackedcmdbuffer::nvidia_marker_session& session = commandbuffer_data.nvidia_marker_sessions.back();
	return session.recording ? &session : nullptr;
}

static bool command_buffer_supports_instrumentation(const trackedcmdbuffer& commandbuffer_data, std::string& error)
{
	if (commandbuffer_data.device_index >= VkDevice_index.size())
	{
		error = "command buffer device metadata is unavailable";
		return false;
	}
	const trackeddevice& device_data = VkDevice_index.at(commandbuffer_data.device_index);
	const auto pool = device_data.replay_command_pools.find(commandbuffer_data.pool_index);
	if (pool == device_data.replay_command_pools.end())
	{
		error = "command pool metadata is unavailable";
		return false;
	}
	if (pool->second.flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT)
	{
		error = "shader instrumentation is unavailable for protected command buffers";
		return false;
	}
	const VkQueueFlags supported = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_DATA_GRAPH_BIT_ARM;
	if ((pool->second.queue_flags & supported) == 0)
	{
		error = "command pool does not support graphics, compute, or data graph operations";
		return false;
	}
	return true;
}

static bool enumerate_instrumentation_metrics(trackeddevice& device_data, std::string& error)
{
	if (!device_data.shader_instrumentation_metrics.empty()) return true;
	if (!wrap_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM)
	{
		error = "instrumentation metric enumeration is unavailable";
		return false;
	}

	uint32_t count = 0;
	VkResult result = wrap_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM(device_data.physicalDevice, &count, nullptr);
	if (result != VK_SUCCESS)
	{
		error = "failed to enumerate instrumentation metrics: " + std::string(errorString(result));
		return false;
	}
	device_data.shader_instrumentation_metrics.resize(count);
	for (VkShaderInstrumentationMetricDescriptionARM& description : device_data.shader_instrumentation_metrics)
	{
		description = { VK_STRUCTURE_TYPE_SHADER_INSTRUMENTATION_METRIC_DESCRIPTION_ARM, nullptr };
	}
	result = wrap_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM(device_data.physicalDevice, &count,
		device_data.shader_instrumentation_metrics.data());
	if (result != VK_SUCCESS)
	{
		device_data.shader_instrumentation_metrics.clear();
		error = "failed to read instrumentation metric descriptions: " + std::string(errorString(result));
		return false;
	}
	device_data.shader_instrumentation_metrics.resize(count);
	return true;
}

static bool create_instrumentation_probe(lava_file_reader& reader, VkCommandBuffer command_buffer, trackedcmdbuffer& commandbuffer_data,
	trackedcmdbuffer::shader_instrumentation_session& session, std::string& error)
{
	if (commandbuffer_data.device_index >= VkDevice_index.size() || !index_to_VkDevice.contains(commandbuffer_data.device_index))
	{
		error = "command buffer device is unavailable";
		return false;
	}
	trackeddevice& device_data = VkDevice_index.at(commandbuffer_data.device_index);
	if (!device_data.shader_instrumentation_enabled)
	{
		error = "VK_ARM_shader_instrumentation is unsupported";
		return false;
	}
	if (!enumerate_instrumentation_metrics(device_data, error)) return false;

	VkShaderInstrumentationCreateInfoARM create_info = {
		VK_STRUCTURE_TYPE_SHADER_INSTRUMENTATION_CREATE_INFO_ARM,
		nullptr
	};
	VkShaderInstrumentationARM instrumentation = VK_NULL_HANDLE;
	VkResult result = wrap_vkCreateShaderInstrumentationARM(commandbuffer_data.device, &create_info, nullptr, &instrumentation);
	if (result != VK_SUCCESS)
	{
		error = "failed to create shader instrumentation: " + std::string(errorString(result));
		return false;
	}

	trackedcmdbuffer::shader_instrumentation_probe probe;
	probe.handle = instrumentation;
	probe.source = reader.current;
	session.probes.push_back(std::move(probe));
	wrap_vkCmdBeginShaderInstrumentationARM(command_buffer, instrumentation);
	return true;
}

static bool cache_instrumentation_probe(trackedcmdbuffer& commandbuffer_data,
	trackedcmdbuffer::shader_instrumentation_probe& probe, std::string& error)
{
	if (probe.cached) return true;
	if (probe.handle == VK_NULL_HANDLE)
	{
		error = "uncached instrumentation probe has no live object";
		return false;
	}
	if (commandbuffer_data.device_index >= VkDevice_index.size())
	{
		error = "instrumentation device metadata is unavailable";
		return false;
	}
	trackeddevice& device_data = VkDevice_index.at(commandbuffer_data.device_index);
	const size_t metric_count = device_data.shader_instrumentation_metrics.size();
	const size_t block_size = sizeof(VkShaderInstrumentationMetricDataHeaderARM) + metric_count * sizeof(uint64_t);

	std::vector<uint64_t> storage;
	uint32_t block_count = 0;
	uint32_t capacity = 1;
	VkResult result = VK_INCOMPLETE;
	while (true)
	{
		const size_t bytes = (size_t)capacity * block_size;
		storage.resize((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
		block_count = capacity;
		result = wrap_vkGetShaderInstrumentationValuesARM(commandbuffer_data.device, probe.handle, &block_count, storage.data(), 0);
		if (result != VK_INCOMPLETE) break;
		if (capacity > UINT32_MAX / 2)
		{
			error = "shader instrumentation result count overflow";
			return false;
		}
		capacity *= 2;
	}
	if (result != VK_SUCCESS)
	{
		error = "failed to read shader instrumentation results: " + std::string(errorString(result));
		return false;
	}

	probe.blocks.clear();
	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(storage.data());
	for (uint32_t block = 0; block < block_count; block++)
	{
		trackedcmdbuffer::shader_instrumentation_block value;
		const uint8_t* block_data = bytes + (size_t)block * block_size;
		memcpy(&value.header, block_data, sizeof(value.header));
		value.values.resize(metric_count);
		if (metric_count > 0)
		{
			memcpy(value.values.data(), block_data + sizeof(value.header), metric_count * sizeof(uint64_t));
		}
		probe.blocks.push_back(std::move(value));
	}
	probe.cached = true;
	return true;
}

void cli_process_instrument_request(callback_context& cb, VkCommandBuffer command_buffer)
{
	lava_reader* parent = cb.reader.parent;
	const cli_instrument_mode mode = parent->cli_instrument_requested.exchange(cli_instrument_mode::none, std::memory_order_acq_rel);
	const cli_marker_placement marker_placement = parent->cli_marker_requested.exchange(cli_marker_placement::none,
		std::memory_order_acq_rel);
	if (mode == cli_instrument_mode::none && marker_placement == cli_marker_placement::none) return;

	std::string response;
	const uint32_t commandbuffer_index = index_to_VkCommandBuffer.index_or_invalid(command_buffer);
	if (cb.result.vkresult != VK_SUCCESS)
	{
		response = "ERROR vkBeginCommandBuffer did not succeed\n";
	}
	else if (mode != cli_instrument_mode::none && !parent->cli_shader_instrumentation_enabled.load(std::memory_order_acquire))
	{
		response = "ERROR VK_ARM_shader_instrumentation is unsupported\n";
	}
	else if (commandbuffer_index == CONTAINER_INVALID_INDEX || commandbuffer_index >= VkCommandBuffer_index.size())
	{
		response = "ERROR command buffer is not tracked\n";
	}
	else
	{
		trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
		std::string error;
		if (marker_placement != cli_marker_placement::none &&
			(commandbuffer_data.device_index >= VkDevice_index.size() ||
			!VkDevice_index.at(commandbuffer_data.device_index).replay_aftermath_enabled))
		{
			response = "ERROR NVIDIA diagnostic checkpoints are unavailable; use --device-fault-report on a supported NVIDIA device\n";
		}
		else if (marker_placement != cli_marker_placement::none)
		{
			trackedcmdbuffer::nvidia_marker_session session;
			session.id = parent->cli_marker_next_session.fetch_add(1, std::memory_order_relaxed);
			session.placement = (uint8_t)marker_placement;
			session.recording = true;
			commandbuffer_data.nvidia_marker_sessions.push_back(std::move(session));
			response = std::to_string(commandbuffer_data.nvidia_marker_sessions.back().id) + " " +
				std::to_string(commandbuffer_index) + "\n";
		}
		else if (!command_buffer_supports_instrumentation(commandbuffer_data, error))
		{
			response = "ERROR " + error + "\n";
		}
		else if (active_instrumentation_session(commandbuffer_data))
		{
			response = "ERROR command buffer is already instrumented\n";
		}
		else
		{
			trackedcmdbuffer::shader_instrumentation_session session;
			session.detailed = mode == cli_instrument_mode::detailed;
			session.recording = true;
			commandbuffer_data.shader_instrumentation_sessions.push_back(std::move(session));
			trackedcmdbuffer::shader_instrumentation_session& active = commandbuffer_data.shader_instrumentation_sessions.back();
			if (!active.detailed && !create_instrumentation_probe(cb.reader, command_buffer, commandbuffer_data, active, error))
			{
				commandbuffer_data.shader_instrumentation_sessions.pop_back();
				response = "ERROR " + error + "\n";
			}
			else
			{
				response = std::to_string(commandbuffer_index) + "\n";
			}
		}
	}

	parent->cli_response = response;
	if (marker_placement != cli_marker_placement::none)
	{
		parent->cli_marker_ready.store(true, std::memory_order_release);
		parent->cli_marker_ready.notify_all();
	}
	else
	{
		parent->cli_instrument_ready.store(true, std::memory_order_release);
		parent->cli_instrument_ready.notify_all();
	}
}

void replay_nvidia_marker_as_build(lava_file_reader& reader, VkCommandBuffer command_buffer, uint32_t info_count,
	const VkAccelerationStructureBuildGeometryInfoKHR* infos,
	const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, bool before)
{
	const uint32_t commandbuffer_index = index_to_VkCommandBuffer.index_or_invalid(command_buffer);
	if (commandbuffer_index == CONTAINER_INVALID_INDEX || commandbuffer_index >= VkCommandBuffer_index.size()) return;
	trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
	trackedcmdbuffer::nvidia_marker_session* session = active_nvidia_marker_session(commandbuffer_data);
	if (!session) return;
	const uint8_t wanted = before ? (uint8_t)cli_marker_placement::before : (uint8_t)cli_marker_placement::after;
	if ((session->placement & wanted) == 0) return;
	if (before)
	{
		session->as_build_diagnostics.push_back(
			replay_as_build_diagnostic(reader, commandbuffer_index, info_count, infos, ranges));
	}

	PFN_vkCmdSetCheckpointNV set_checkpoint = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
		wrap_vkGetDeviceProcAddr(commandbuffer_data.device, "vkCmdSetCheckpointNV"));
	if (!set_checkpoint) ABORT("vkCmdSetCheckpointNV is unavailable for an active NVIDIA marker session");

	trackedcmdbuffer::nvidia_checkpoint_marker marker;
	marker.source = reader.current;
	marker.session_id = session->id;
	marker.ordinal = session->next_ordinal++;
	marker.before = before;
	std::ostringstream label;
	label << "lava session=" << marker.session_id << " marker=" << marker.ordinal
		<< " placement=" << (before ? "before" : "after")
		<< " call=vkCmdBuildAccelerationStructuresKHR"
		<< " thread=" << (unsigned)reader.current.thread << " frame=" << reader.current.frame
		<< " packet=" << reader.current.packet << " command_buffer=" << commandbuffer_index
		<< " info_count=" << info_count;
	for (uint32_t i = 0; infos && i < info_count; i++)
	{
		label << " info[" << i << "]={dst="
			<< index_to_VkAccelerationStructureKHR.index_or_invalid(infos[i].dstAccelerationStructure)
			<< ",src=" << index_to_VkAccelerationStructureKHR.index_or_invalid(infos[i].srcAccelerationStructure)
			<< ",mode=" << (unsigned)infos[i].mode << ",geometries=" << infos[i].geometryCount
			<< ",scratch=0x" << std::hex << infos[i].scratchData.deviceAddress << std::dec << "}";
	}
	marker.label = label.str();
	session->markers.push_back(std::move(marker));
	trackedcmdbuffer::nvidia_checkpoint_marker& stored = session->markers.back();
	if (reader.parent->aftermath_register_marker_callback)
	{
		reader.parent->aftermath_register_marker_callback(reader.parent->aftermath_context, &stored, stored.label.c_str());
	}
	set_checkpoint(command_buffer, &stored);
}

std::string replay_as_build_show(uint32_t commandbuffer_index)
{
	if (commandbuffer_index >= VkCommandBuffer_index.size()) return "ERROR invalid command buffer index\n";
	const trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
	if (commandbuffer_data.nvidia_marker_sessions.empty()) return "ERROR command buffer has no AS diagnostic snapshots\n";
	Json::Value root;
	root["command_buffer"] = commandbuffer_index;
	Json::Value sessions(Json::arrayValue);
	for (const trackedcmdbuffer::nvidia_marker_session& session : commandbuffer_data.nvidia_marker_sessions)
	{
		if (session.as_build_diagnostics.empty()) continue;
		Json::Value value;
		value["session"] = session.id;
		Json::Value builds(Json::arrayValue);
		for (const std::string& diagnostic : session.as_build_diagnostics)
		{
			Json::Value build;
			Json::Reader parser;
			if (parser.parse(diagnostic, build)) builds.append(build);
		}
		value["snapshots"] = builds;
		Json::Value fixups(Json::arrayValue);
		for (const std::string& diagnostic : session.instance_fixup_diagnostics)
		{
			Json::Value fixup;
			Json::Reader parser;
			if (parser.parse(diagnostic, fixup)) fixups.append(fixup);
		}
		value["submission_instance_fixups"] = fixups;
		sessions.append(value);
	}
	if (sessions.empty()) return "ERROR command buffer has no AS diagnostic snapshots\n";
	root["sessions"] = sessions;
	Json::StyledWriter writer;
	return writer.write(root);
}

void replay_instrumentation_pre_shader_command(lava_file_reader& reader, VkCommandBuffer command_buffer, trackedcmdbuffer& commandbuffer_data)
{
	trackedcmdbuffer::shader_instrumentation_session* session = active_instrumentation_session(commandbuffer_data);
	if (!session || !session->detailed) return;
	std::string error;
	if (!create_instrumentation_probe(reader, command_buffer, commandbuffer_data, *session, error))
	{
		ABORT("Failed to instrument %s: %s", get_packet_name((packet_type)reader.current.packet_type, reader.current.call_id), error.c_str());
	}
}

void replay_instrumentation_post_shader_command(lava_file_reader& reader, VkCommandBuffer command_buffer, trackedcmdbuffer& commandbuffer_data)
{
	(void)reader;
	trackedcmdbuffer::shader_instrumentation_session* session = active_instrumentation_session(commandbuffer_data);
	if (!session || !session->detailed || session->probes.empty()) return;
	wrap_vkCmdEndShaderInstrumentationARM(command_buffer);
}

void replay_instrumentation_end_command_buffer(lava_file_reader& reader, VkCommandBuffer command_buffer)
{
	(void)reader;
	const uint32_t commandbuffer_index = index_to_VkCommandBuffer.index_or_invalid(command_buffer);
	if (commandbuffer_index == CONTAINER_INVALID_INDEX || commandbuffer_index >= VkCommandBuffer_index.size()) return;
	trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
	trackedcmdbuffer::shader_instrumentation_session* session = active_instrumentation_session(commandbuffer_data);
	if (session)
	{
		if (!session->detailed) wrap_vkCmdEndShaderInstrumentationARM(command_buffer);
		session->recording = false;
	}
	trackedcmdbuffer::nvidia_marker_session* marker_session = active_nvidia_marker_session(commandbuffer_data);
	if (marker_session) marker_session->recording = false;
}

static void replay_instrumentation_mark_submitted_recursive(trackedcmdbuffer& commandbuffer_data,
	std::unordered_set<trackedcmdbuffer*>& visited)
{
	if (!visited.insert(&commandbuffer_data).second) return;
	if (!commandbuffer_data.shader_instrumentation_sessions.empty())
	{
		trackedcmdbuffer::shader_instrumentation_session& session = commandbuffer_data.shader_instrumentation_sessions.back();
		if (session.live && !session.recording) session.submitted = true;
	}
	for (uint32_t secondary_index : commandbuffer_data.replay_secondary_commandbuffers)
	{
		if (secondary_index >= VkCommandBuffer_index.size()) continue;
		replay_instrumentation_mark_submitted_recursive(VkCommandBuffer_index.at(secondary_index), visited);
	}
}

void replay_instrumentation_mark_submitted(trackedcmdbuffer& commandbuffer_data)
{
	std::unordered_set<trackedcmdbuffer*> visited;
	replay_instrumentation_mark_submitted_recursive(commandbuffer_data, visited);
}

void replay_instrumentation_cleanup_command_buffer(trackedcmdbuffer& commandbuffer_data)
{
	for (trackedcmdbuffer::shader_instrumentation_session& session : commandbuffer_data.shader_instrumentation_sessions)
	{
		for (trackedcmdbuffer::shader_instrumentation_probe& probe : session.probes)
		{
			if (probe.handle == VK_NULL_HANDLE) continue;
			if (!probe.cached)
			{
				std::string error;
				if (!cache_instrumentation_probe(commandbuffer_data, probe, error))
				{
					ELOG("Failed to cache shader instrumentation before destruction: %s", error.c_str());
				}
			}
			wrap_vkDestroyShaderInstrumentationARM(commandbuffer_data.device, probe.handle, nullptr);
			probe.handle = VK_NULL_HANDLE;
		}
		session.recording = false;
		session.live = false;
	}
}

void replay_instrumentation_cleanup_all()
{
	for (uint32_t device_index = 0; device_index < VkDevice_index.size(); device_index++)
	{
		trackeddevice& device_data = VkDevice_index.at(device_index);
		if (!device_data.is_state(trackable::states::created) || !index_to_VkDevice.contains(device_index)) continue;
		const VkDevice device = index_to_VkDevice.at(device_index);
		if (device == VK_NULL_HANDLE) continue;
		(void)wrap_vkDeviceWaitIdle(device);
		for (trackedcmdbuffer& commandbuffer_data : VkCommandBuffer_index)
		{
			if (commandbuffer_data.device_index == device_index) replay_instrumentation_cleanup_command_buffer(commandbuffer_data);
		}
	}
}

static Json::Value instrumentation_source_json(const change_source& source)
{
	Json::Value value;
	value["packet"] = source.packet;
	value["frame"] = source.frame;
	value["thread"] = source.thread;
	if (source.packet_type != UINT8_MAX)
	{
		value["command"] = get_packet_name((packet_type)source.packet_type, source.call_id);
	}
	return value;
}

std::string replay_instrumentation_show(uint32_t commandbuffer_index)
{
	if (commandbuffer_index >= VkCommandBuffer_index.size()) return "ERROR invalid command buffer index\n";
	trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
	if (commandbuffer_data.shader_instrumentation_sessions.empty()) return "ERROR command buffer has no instrumentation\n";
	if (commandbuffer_data.device_index >= VkDevice_index.size()) return "ERROR instrumentation device metadata is unavailable\n";
	trackeddevice& device_data = VkDevice_index.at(commandbuffer_data.device_index);

	for (trackedcmdbuffer::shader_instrumentation_session& session : commandbuffer_data.shader_instrumentation_sessions)
	{
		if (!session.submitted && session.live) return "ERROR instrumented command buffer has not been submitted\n";
		for (trackedcmdbuffer::shader_instrumentation_probe& probe : session.probes)
		{
			if (probe.cached) continue;
			std::string error;
			if (!cache_instrumentation_probe(commandbuffer_data, probe, error)) return "ERROR " + error + "\n";
		}
	}

	Json::Value root;
	root["commandBuffer"] = commandbuffer_index;
	Json::Value metrics(Json::arrayValue);
	for (uint32_t i = 0; i < device_data.shader_instrumentation_metrics.size(); i++)
	{
		Json::Value metric;
		metric["index"] = i;
		metric["name"] = device_data.shader_instrumentation_metrics[i].name;
		metric["description"] = device_data.shader_instrumentation_metrics[i].description;
		metrics.append(metric);
	}
	root["metrics"] = metrics;

	Json::Value sessions(Json::arrayValue);
	for (const trackedcmdbuffer::shader_instrumentation_session& session : commandbuffer_data.shader_instrumentation_sessions)
	{
		Json::Value session_json;
		session_json["mode"] = session.detailed ? "detailed" : "whole";
		session_json["submitted"] = session.submitted;
		Json::Value probes(Json::arrayValue);
		for (const trackedcmdbuffer::shader_instrumentation_probe& probe : session.probes)
		{
			Json::Value probe_json;
			probe_json["source"] = instrumentation_source_json(probe.source);
			Json::Value blocks(Json::arrayValue);
			for (const trackedcmdbuffer::shader_instrumentation_block& block : probe.blocks)
			{
				Json::Value block_json;
				block_json["resultIndex"] = block.header.resultIndex;
				block_json["resultSubIndex"] = block.header.resultSubIndex;
				block_json["stages"] = VkShaderStageFlags_to_string(block.header.stages);
				block_json["stageFlags"] = block.header.stages;
				block_json["basicBlockIndex"] = block.header.basicBlockIndex;
				Json::Value values;
				for (uint32_t i = 0; i < block.values.size() && i < device_data.shader_instrumentation_metrics.size(); i++)
				{
					values[device_data.shader_instrumentation_metrics[i].name] = Json::Value::UInt64(block.values[i]);
				}
				block_json["values"] = values;
				blocks.append(block_json);
			}
			probe_json["blocks"] = blocks;
			probes.append(probe_json);
		}
		session_json["probes"] = probes;
		sessions.append(session_json);
	}
	root["sessions"] = sessions;
	return root.toStyledString();
}
