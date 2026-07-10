#include "replay_diagnostics.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "datatable.h"
#include "read_auto.h"
#include "tostring.h"

struct replay_thread_snapshot
{
	uint32_t thread = 0;
	std::string name;
	cli_thread_state state = cli_thread_state::not_started;
	uint32_t packet = 0;
	uint32_t completed_packet = 0;
	int wait_thread = -1;
	uint32_t wait_packet = UINT32_MAX;
	VkObjectType wait_object_type = VK_OBJECT_TYPE_UNKNOWN;
	uint32_t wait_object_index = CONTAINER_INVALID_INDEX;
	uint32_t wait_aux_index = CONTAINER_INVALID_INDEX;
};

const char* replay_diagnostics_thread_state_name(cli_thread_state state)
{
	switch (state)
	{
		case cli_thread_state::not_started: return "not_started";
		case cli_thread_state::running: return "running";
		case cli_thread_state::cli_paused: return "cli_paused";
		case cli_thread_state::wait_handle: return "wait_handle";
		case cli_thread_state::wait_barrier: return "wait_barrier";
		case cli_thread_state::wait_fence: return "wait_fence";
		case cli_thread_state::wait_queue_idle: return "wait_queue_idle";
		case cli_thread_state::wait_device_idle: return "wait_device_idle";
		case cli_thread_state::terminated: return "terminated";
		default: return "unknown";
	}
}

static bool replay_diagnostics_is_thread_wait(cli_thread_state state)
{
	return state == cli_thread_state::wait_barrier || state == cli_thread_state::wait_handle;
}

static bool replay_diagnostics_is_gpu_wait(cli_thread_state state)
{
	return state == cli_thread_state::wait_fence
	       || state == cli_thread_state::wait_queue_idle
	       || state == cli_thread_state::wait_device_idle;
}

static std::string replay_diagnostics_source_string(const change_source& source)
{
	if (source.packet_type == UINT8_MAX) return "-";
	return "thread " + std::to_string(source.thread) + ", packet " + std::to_string(source.packet) + ", frame " + std::to_string(source.frame);
}

static std::string replay_diagnostics_join_indices(const std::vector<uint32_t>& values)
{
	if (values.empty()) return "-";
	std::string out;
	for (uint32_t value : values)
	{
		if (!out.empty()) out += ", ";
		out += std::to_string(value);
	}
	return out;
}

static std::vector<replay_thread_snapshot> replay_diagnostics_snapshot_threads(lava_reader& replayer)
{
	std::vector<replay_thread_snapshot> out;
	out.reserve(replayer.threads.size());
	for (uint32_t i = 0; i < replayer.threads.size(); i++)
	{
		lava_file_reader& reader = replayer.file_reader(i);
		replay_thread_snapshot snapshot;
		snapshot.thread = i;
		const char* name = reader.get_trace_thread_name();
		if (name) snapshot.name = name;
		snapshot.state = reader.cli_state.load(std::memory_order_acquire);
		snapshot.packet = reader.cli_packet.load(std::memory_order_relaxed);
		if (replayer.thread_packet_numbers && i < replayer.thread_packet_numbers->size())
		{
			snapshot.completed_packet = replayer.thread_packet_numbers->at(i).load(std::memory_order_relaxed);
		}
		snapshot.wait_thread = reader.cli_wait_thread.load(std::memory_order_relaxed);
		snapshot.wait_packet = reader.cli_wait_packet.load(std::memory_order_relaxed);
		snapshot.wait_object_type = (VkObjectType)reader.cli_wait_object_type.load(std::memory_order_relaxed);
		snapshot.wait_object_index = reader.cli_wait_object_index.load(std::memory_order_relaxed);
		snapshot.wait_aux_index = reader.cli_wait_aux_index.load(std::memory_order_relaxed);
		out.push_back(snapshot);
	}
	return out;
}

static std::string replay_diagnostics_object_wait_description(const replay_thread_snapshot& snapshot)
{
	if (!replay_diagnostics_is_gpu_wait(snapshot.state)) return "";
	std::string out;
	switch (snapshot.state)
	{
		case cli_thread_state::wait_fence: out = "fence"; break;
		case cli_thread_state::wait_queue_idle: out = "queue idle"; break;
		case cli_thread_state::wait_device_idle: out = "device idle"; break;
		default: out = "object"; break;
	}
	if (snapshot.wait_object_index != CONTAINER_INVALID_INDEX)
	{
		out += ": " + VkObjectType_to_string(snapshot.wait_object_type) + " " + std::to_string(snapshot.wait_object_index);
	}
	if (snapshot.wait_aux_index != CONTAINER_INVALID_INDEX)
	{
		out += ", commandbuffer " + std::to_string(snapshot.wait_aux_index);
	}
	return out;
}

std::string replay_diagnostics_thread_wait_description(lava_reader& replayer, lava_file_reader& reader, cli_thread_state state)
{
	if (replay_diagnostics_is_thread_wait(state))
	{
		const int wait_thread = reader.cli_wait_thread.load(std::memory_order_relaxed);
		const uint32_t wait_packet = reader.cli_wait_packet.load(std::memory_order_relaxed);
		if (wait_thread < 0 || wait_thread >= (int)replayer.threads.size() || wait_packet == UINT32_MAX) return "";

		const char* wait_type = state == cli_thread_state::wait_barrier ? "barrier" : "handle";
		return std::string(wait_type) + ": thread " + std::to_string(wait_thread) + ", packet " + std::to_string(wait_packet);
	}

	replay_thread_snapshot snapshot;
	snapshot.state = state;
	snapshot.wait_object_type = (VkObjectType)reader.cli_wait_object_type.load(std::memory_order_relaxed);
	snapshot.wait_object_index = reader.cli_wait_object_index.load(std::memory_order_relaxed);
	snapshot.wait_aux_index = reader.cli_wait_aux_index.load(std::memory_order_relaxed);
	return replay_diagnostics_object_wait_description(snapshot);
}

std::string replay_diagnostics_threads_response(lava_reader& replayer)
{
	data_table out;
	out.set_headers({"Thread", "Name", "State", "Packet", "Waiting On"});
	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		lava_file_reader& reader = replayer.file_reader(i);
		const char* thread_name = reader.get_trace_thread_name();
		const cli_thread_state state = reader.cli_state.load(std::memory_order_acquire);
		out.add_row({
			std::to_string(i),
			thread_name ? thread_name : "",
			replay_diagnostics_thread_state_name(state),
			std::to_string(reader.cli_packet.load(std::memory_order_relaxed)),
			replay_diagnostics_thread_wait_description(replayer, reader, state)
		});
	}
	return out.to_markdown();
}

static bool replay_diagnostics_wait_edge_active(const std::vector<replay_thread_snapshot>& snapshots, const replay_thread_snapshot& snapshot)
{
	if (!replay_diagnostics_is_thread_wait(snapshot.state)) return false;
	if (snapshot.wait_thread < 0 || snapshot.wait_thread >= (int)snapshots.size()) return false;
	if (snapshot.wait_packet == UINT32_MAX) return false;
	const uint32_t completed = snapshots.at(snapshot.wait_thread).completed_packet;
	if (snapshot.state == cli_thread_state::wait_barrier) return snapshot.wait_packet > completed;
	return snapshot.wait_packet >= completed;
}

static std::vector<uint32_t> replay_diagnostics_find_thread_cycle(const std::vector<replay_thread_snapshot>& snapshots)
{
	std::vector<int> edges(snapshots.size(), -1);
	for (uint32_t i = 0; i < snapshots.size(); i++)
	{
		if (replay_diagnostics_wait_edge_active(snapshots, snapshots.at(i))) edges.at(i) = snapshots.at(i).wait_thread;
	}

	for (uint32_t start = 0; start < snapshots.size(); start++)
	{
		std::vector<int> positions(snapshots.size(), -1);
		std::vector<uint32_t> path;
		int current = start;
		while (current >= 0 && current < (int)snapshots.size() && edges.at(current) >= 0)
		{
			if (positions.at(current) >= 0)
			{
				return std::vector<uint32_t>(path.begin() + positions.at(current), path.end());
			}
			positions.at(current) = (int)path.size();
			path.push_back((uint32_t)current);
			current = edges.at(current);
		}
	}
	return {};
}

static std::string replay_diagnostics_thread_wait_target(const replay_thread_snapshot& snapshot)
{
	const char* wait_type = snapshot.state == cli_thread_state::wait_barrier ? "barrier" : "handle";
	return std::string(wait_type) + ": thread " + std::to_string(snapshot.wait_thread) + ", packet " + std::to_string(snapshot.wait_packet);
}

static void replay_diagnostics_add_cycle_table(std::string& response, const std::vector<replay_thread_snapshot>& snapshots, const std::vector<uint32_t>& cycle)
{
	data_table out;
	out.set_headers({"Thread", "Name", "State", "Packet", "Completed", "Waiting On", "Target Completed"});
	for (uint32_t thread : cycle)
	{
		const replay_thread_snapshot& snapshot = snapshots.at(thread);
		std::string target_completed = "-";
		if (snapshot.wait_thread >= 0 && snapshot.wait_thread < (int)snapshots.size())
		{
			target_completed = std::to_string(snapshots.at(snapshot.wait_thread).completed_packet);
		}
		out.add_row({
			std::to_string(snapshot.thread),
			snapshot.name,
			replay_diagnostics_thread_state_name(snapshot.state),
			std::to_string(snapshot.packet),
			std::to_string(snapshot.completed_packet),
			replay_diagnostics_thread_wait_target(snapshot),
			target_completed
		});
	}
	response += "Thread wait cycle:\n";
	response += out.to_markdown();
	response += "\n";
}

static std::string replay_diagnostics_fence_pending_commandbuffers(uint32_t fence_index)
{
	if (fence_index == CONTAINER_INVALID_INDEX || !index_to_VkFence.contains(fence_index)) return "-";
	return replay_diagnostics_join_indices(VkFence_index.at(fence_index).replay_pending_commandbuffers);
}

static std::string replay_diagnostics_fence_last_submit(uint32_t fence_index)
{
	if (fence_index == CONTAINER_INVALID_INDEX || !index_to_VkFence.contains(fence_index)) return "-";
	const trackedfence& fence_data = VkFence_index.at(fence_index);
	if (!fence_data.replay_last_submit_source_valid) return "-";
	return replay_diagnostics_source_string(fence_data.replay_last_submit_source);
}

static std::string replay_diagnostics_commandbuffer_summary(uint32_t commandbuffer_index)
{
	if (commandbuffer_index == CONTAINER_INVALID_INDEX || !index_to_VkCommandBuffer.contains(commandbuffer_index)) return "-";
	return "commandbuffer " + std::to_string(commandbuffer_index);
}

static std::string replay_diagnostics_commandbuffer_last_submit(uint32_t commandbuffer_index)
{
	if (commandbuffer_index == CONTAINER_INVALID_INDEX || !index_to_VkCommandBuffer.contains(commandbuffer_index)) return "-";
	const trackedcmdbuffer& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);
	if (!commandbuffer_data.replay_last_submit_source_valid) return "-";
	return replay_diagnostics_source_string(commandbuffer_data.replay_last_submit_source);
}

static void replay_diagnostics_add_gpu_waits_table(std::string& response, const std::vector<replay_thread_snapshot>& snapshots)
{
	data_table out;
	bool any = false;
	out.set_headers({"Thread", "Name", "State", "Packet", "Waiting On", "Pending Commandbuffers", "Last Submit"});
	for (const replay_thread_snapshot& snapshot : snapshots)
	{
		if (!replay_diagnostics_is_gpu_wait(snapshot.state)) continue;
		any = true;
		std::string pending = "-";
		std::string last_submit = "-";
		if (snapshot.state == cli_thread_state::wait_fence)
		{
			pending = replay_diagnostics_fence_pending_commandbuffers(snapshot.wait_object_index);
			last_submit = replay_diagnostics_fence_last_submit(snapshot.wait_object_index);
			if (last_submit == "-") last_submit = replay_diagnostics_commandbuffer_last_submit(snapshot.wait_aux_index);
		}
		else if (snapshot.state == cli_thread_state::wait_queue_idle)
		{
			pending = replay_diagnostics_commandbuffer_summary(snapshot.wait_aux_index);
			last_submit = replay_diagnostics_commandbuffer_last_submit(snapshot.wait_aux_index);
		}
		out.add_row({
			std::to_string(snapshot.thread),
			snapshot.name,
			replay_diagnostics_thread_state_name(snapshot.state),
			std::to_string(snapshot.packet),
			replay_diagnostics_object_wait_description(snapshot),
			pending,
			last_submit
		});
	}
	if (!any) return;
	response += "GPU waits:\n";
	response += out.to_markdown();
	response += "\n";
}

std::string replay_diagnostics_deadlock_response(lava_reader& replayer)
{
	const std::vector<replay_thread_snapshot> snapshots = replay_diagnostics_snapshot_threads(replayer);
	const std::vector<uint32_t> cycle = replay_diagnostics_find_thread_cycle(snapshots);
	bool has_gpu_wait = false;
	for (const replay_thread_snapshot& snapshot : snapshots)
	{
		if (replay_diagnostics_is_gpu_wait(snapshot.state))
		{
			has_gpu_wait = true;
			break;
		}
	}

	if (cycle.empty() && !has_gpu_wait)
	{
		return "NO_DEADLOCK\nNo thread wait cycles found.\nNo blocking fence/device wait currently published.\n";
	}

	std::string response = cycle.empty() ? "DEADLOCK suspected\n\n" : "DEADLOCK suspected\n\n";
	if (!cycle.empty())
	{
		replay_diagnostics_add_cycle_table(response, snapshots, cycle);
	}
	else
	{
		response += "No thread wait cycles found.\n\n";
	}
	replay_diagnostics_add_gpu_waits_table(response, snapshots);
	if (response.empty() || response.back() != '\n') response += "\n";
	return response;
}
