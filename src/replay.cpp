#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>
#include <string>

#include "vulkan/vulkan.h"
#include "util.h"
#include "read_auto.h"
#include "read.h"
#include "packfile.h"
#include "window.h"
#include "util_auto.h"
#include "allocators.h"
#include "replay_callbacks.h"
#include "sandbox.h"
#include "datatable.h"
#include "pipeline_executable_stats.h"
#include "helpers_read.h"
#include "tostring.h"
#include "trace_metadata.h"
#include "suballocator.h"

static lava_reader replayer;
static std::atomic<bool> done_var { false };
static std::atomic<bool> replay_done { false };
static std::atomic<bool> service_stop_requested { false };
static int port = -1;
static std::string hostname = "localhost";

static std::vector<std::string> split_command(const std::string& keyword)
{
	std::istringstream in(keyword);
	std::vector<std::string> tokens;
	std::string token;
	while (in >> token)
	{
		tokens.push_back(token);
	}
	return tokens;
}

static bool parse_u32(const std::string& text, uint32_t& out)
{
	char* end = nullptr;
	errno = 0;
	const unsigned long value = strtoul(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0' || value > std::numeric_limits<uint32_t>::max())
	{
		return false;
	}
	out = (uint32_t)value;
	return true;
}

static bool parse_positive_u32(const std::string& text, uint32_t& out)
{
	return parse_u32(text, out) && out > 0;
}

static bool parse_debug_level(const std::string& text, uint_fast8_t& out)
{
	uint32_t value = 0;
	if (!parse_u32(text, value) || value > 3) return false;
	out = (uint_fast8_t)value;
	return true;
}

static bool parse_bool(const std::string& text, bool& out)
{
	if (text == "true")
	{
		out = true;
		return true;
	}
	if (text == "false")
	{
		out = false;
		return true;
	}
	return false;
}

static bool cli_show_json(const char* object_type, uint32_t index, Json::Value& out)
{
	if (!cli_show_object_json(object_type, index, out)) return false;
	if (strcmp(object_type, "VkPipeline") != 0) return true;
	if (!replayer.cli_pipeline_executable_stats_enabled.load(std::memory_order_acquire)) return true;
	if (index >= VkPipeline_index.size()) return true;

	trackedpipeline& pipeline_data = VkPipeline_index.at(index);
	if (!pipeline_data.is_state(trackable::states::created)) return true;
	if (pipeline_data.device_index == UINT32_MAX || pipeline_data.device_index >= VkDevice_index.size()) return true;
	if (!index_to_VkPipeline.contains(index)) return true;
	if (!index_to_VkDevice.contains(pipeline_data.device_index)) return true;

	VkDevice device = index_to_VkDevice.at(pipeline_data.device_index);
	VkPipeline pipeline = index_to_VkPipeline.at(index);
	(void)append_pipeline_executable_statistics_json(device, pipeline, out);
	return true;
}

static std::string format_mib(VkDeviceSize bytes)
{
	char text[64];
	snprintf(text, sizeof(text), "%.2f MiB", (double)bytes / (1024.0 * 1024.0));
	return text;
}

static std::string format_percent_left(VkDeviceSize usage, VkDeviceSize budget)
{
	if (budget == 0) return "n/a";
	const VkDeviceSize left = usage < budget ? budget - usage : 0;
	char text[64];
	snprintf(text, sizeof(text), "%.2f%%", ((double)left * 100.0) / (double)budget);
	return text;
}

static std::string cli_memory_info_response()
{
	if (!replayer.cli_memory_budget_enabled.load(std::memory_order_acquire)) return "ERROR\n";
	if (selected_physical_device == VK_NULL_HANDLE || !wrap_vkGetPhysicalDeviceMemoryProperties2) return "ERROR\n";

	VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
		nullptr
	};
	VkPhysicalDeviceMemoryProperties2 properties = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
		&budget
	};
	wrap_vkGetPhysicalDeviceMemoryProperties2(selected_physical_device, &properties);

	data_table out;
	out.set_headers({"Heap", "Flags", "Usage", "Budget", "Left", "% Left"});
	for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; i++)
	{
		const VkDeviceSize usage = budget.heapUsage[i];
		const VkDeviceSize heap_budget = budget.heapBudget[i];
		const VkDeviceSize left = usage < heap_budget ? heap_budget - usage : 0;
		out.add_row({
			std::to_string(i),
			VkMemoryHeapFlags_to_string(properties.memoryProperties.memoryHeaps[i].flags),
			format_mib(usage),
			format_mib(heap_budget),
			format_mib(left),
			format_percent_left(usage, heap_budget)
		});
	}
	return out.to_markdown();
}

static std::string cli_suballocator_info_response()
{
	std::string response;
	for (uint32_t i = 0; i < VkDevice_index.size(); i++)
	{
		trackeddevice& device_data = VkDevice_index.at(i);
		if (!device_data.allocator) continue;
		if (!response.empty()) response += "\n";
		response += device_data.allocator->info_markdown(i);
	}
	if (response.empty()) return "No suballocator data available\n";
	if (response.back() != '\n') response += "\n";
	return response;
}

static const char* cli_thread_state_name(cli_thread_state state)
{
	switch (state)
	{
		case cli_thread_state::not_started: return "not_started";
		case cli_thread_state::running: return "running";
		case cli_thread_state::cli_paused: return "cli_paused";
		case cli_thread_state::wait_handle: return "wait_handle";
		case cli_thread_state::wait_barrier: return "wait_barrier";
		case cli_thread_state::terminated: return "terminated";
		default: return "unknown";
	}
}

static std::string cli_thread_wait_description(lava_file_reader& reader, cli_thread_state state)
{
	if (state != cli_thread_state::wait_barrier && state != cli_thread_state::wait_handle) return "";

	const int wait_thread = reader.cli_wait_thread.load(std::memory_order_relaxed);
	const uint32_t wait_packet = reader.cli_wait_packet.load(std::memory_order_relaxed);
	if (wait_thread < 0 || wait_thread >= (int)replayer.threads.size() || wait_packet == UINT32_MAX) return "";

	const char* wait_type = state == cli_thread_state::wait_barrier ? "barrier" : "handle";
	return std::string(wait_type) + ": thread " + std::to_string(wait_thread) + ", packet " + std::to_string(wait_packet);
}

static bool cli_thread_quiescent(lava_file_reader& reader)
{
	if (reader.terminated.load(std::memory_order_acquire)) return true;
	const cli_thread_state state = reader.cli_state.load(std::memory_order_acquire);
	return state == cli_thread_state::cli_paused || state == cli_thread_state::wait_handle || state == cli_thread_state::wait_barrier || state == cli_thread_state::terminated;
}

static bool cli_all_threads_quiescent()
{
	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		if (!cli_thread_quiescent(replayer.file_reader(i))) return false;
	}
	return true;
}

static std::string cli_wait_idle_devices()
{
	for (uint32_t i = 0; i < VkDevice_index.size(); i++)
	{
		trackeddevice& device_data = VkDevice_index.at(i);
		if (!device_data.is_state(trackable::states::created)) continue;
		if (!index_to_VkDevice.contains(i)) continue;
		const VkDevice device = index_to_VkDevice.at(i);
		if (device == VK_NULL_HANDLE) continue;
		const VkResult result = wrap_vkDeviceWaitIdle(device);
		if (result == VK_SUCCESS) continue;
		if (result == VK_ERROR_DEVICE_LOST) return "DEVICE_LOST\n";
		return "ERROR\n";
	}
	return "OK\n";
}

static std::string cli_wait_for_quiescence_and_idle()
{
	while (!replay_done.load(std::memory_order_acquire) && !cli_all_threads_quiescent())
	{
		usleep(50);
	}
	return cli_wait_idle_devices();
}

static std::string cli_wait_for_done_and_idle()
{
	while (!replay_done.load(std::memory_order_acquire))
	{
		replay_done.wait(false);
	}
	const std::string idle_response = cli_wait_idle_devices();
	if (idle_response != "OK\n") return idle_response;
	return "DONE\n";
}

static void cli_clear_function_target(lava_file_reader& reader)
{
	reader.cli_function.store(UINT16_MAX, std::memory_order_release);
}

static bool cli_thread_ready()
{
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	return thread_id >= 0 && thread_id < (int)replayer.threads.size();
}

static bool cli_has_paused_command(lava_file_reader& reader)
{
	return reader.cli_paused_call.load(std::memory_order_acquire) != 0;
}

static uint32_t cli_current_packet_count(lava_file_reader& reader)
{
	const uint32_t completed_packets = reader.cli_packet.load(std::memory_order_relaxed);
	return cli_has_paused_command(reader) ? completed_packets + 1 : completed_packets;
}

static uint32_t cli_current_packet_index(lava_file_reader& reader)
{
	if (cli_has_paused_command(reader) && reader.cli_step.load(std::memory_order_acquire) == cli_step_mode::packets)
	{
		return reader.cli_paused_call.load(std::memory_order_acquire) - 1;
	}
	if (cli_has_paused_command(reader)) return reader.current.packet;
	return reader.cli_packet.load(std::memory_order_relaxed);
}

static uint32_t cli_current_call(lava_file_reader& reader)
{
	const uint32_t completed_calls = reader.api_call_count;
	if (cli_has_paused_command(reader) && reader.current.packet_type == PACKET_VULKAN_API_CALL) return completed_calls + 1;
	return completed_calls;
}

static uint32_t cli_completed_count(lava_file_reader& reader, cli_step_mode mode)
{
	return mode == cli_step_mode::calls ? cli_current_call(reader) : cli_current_packet_count(reader);
}

static std::string cli_paused_command_response(lava_file_reader& reader)
{
	if (!cli_has_paused_command(reader)) return "PAUSED\n";
	const char* packet_name = get_packet_name((packet_type)reader.current.packet_type, reader.current.call_id);
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	return "PAUSED @ packet=" + std::to_string(cli_current_packet_index(reader)) + " api_calls=" + std::to_string(cli_current_call(reader))
	       + " name=" + packet_name + " frame="
	       + std::to_string(replayer.global_frame) + "/" + std::to_string(replayer.global_frame_count)
	       + " thread=" + std::to_string(thread_id) + "\n";
}

static void register_replay_callbacks()
{
#define CALLBACK(x) x ## _callbacks.push_back(replay_callback_ ## x)
	CALLBACK(vkCreateInstance);
	CALLBACK(vkDestroyInstance);
	CALLBACK(vkQueuePresentKHR);
	CALLBACK(vkQueueBindSparse);
	CALLBACK(vkQueueSubmit);
	CALLBACK(vkQueueSubmit2);
	CALLBACK(vkQueueSubmit2KHR);
	CALLBACK(vkQueueWaitIdle);
	CALLBACK(vkDeviceWaitIdle);
	CALLBACK(vkGetFenceStatus);
	CALLBACK(vkResetFences);
	CALLBACK(vkWaitForFences);
	CALLBACK(vkAcquireNextImageKHR);
	CALLBACK(vkAcquireNextImage2KHR);
	CALLBACK(vkGetBufferDeviceAddress);
	CALLBACK(vkGetBufferDeviceAddressKHR);
	CALLBACK(vkGetBufferDeviceAddressEXT);
	CALLBACK(vkGetAccelerationStructureDeviceAddressKHR);
	CALLBACK(vkBindBufferMemory2);
	CALLBACK(vkBindBufferMemory2KHR);
	CALLBACK(vkBindImageMemory2);
	CALLBACK(vkBindImageMemory2KHR);
	CALLBACK(vkCreateBuffer);
	CALLBACK(vkCreateAccelerationStructureKHR);
	CALLBACK(vkSubmitDebugUtilsMessageEXT);
	CALLBACK(vkGetAccelerationStructureBuildSizesKHR);
	CALLBACK(vkGetDescriptorEXT);
	CALLBACK(vkWriteSamplerDescriptorsEXT);
	CALLBACK(vkWriteResourceDescriptorsEXT);
	CALLBACK(vkCreateDescriptorUpdateTemplate);
	CALLBACK(vkCreateDescriptorUpdateTemplateKHR);
	CALLBACK(vkGetDataGraphPipelineSessionMemoryRequirementsARM);
	CALLBACK(vkBindDataGraphPipelineSessionMemoryARM);
#undef CALLBACK

	vkCmdBindPipeline_callbacks.push_back(replay_track_vkCmdBindPipeline);
	vkGetRayTracingShaderGroupHandlesKHR_callbacks.push_back(replay_track_vkGetRayTracingShaderGroupHandlesKHR);
	vkGetRayTracingCaptureReplayShaderGroupHandlesKHR_callbacks.push_back(replay_track_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR);
	vkCmdTraceRaysKHR_callbacks.push_back(replay_fixup_vkCmdTraceRaysKHR);
	vkCmdTraceRaysIndirectKHR_callbacks.push_back(replay_fixup_vkCmdTraceRaysIndirectKHR);
	vkCmdTraceRaysIndirect2KHR_callbacks.push_back(replay_fixup_vkCmdTraceRaysIndirect2KHR);
}

void usage()
{
	printf("lava-replay %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
#endif
	printf("-o/--logfile FILE      Output log output to the given file\n");
	printf("-D/--device #          Select physical device to use (by index value)\n");
	printf("-G/--gpu               Use the GPU, fails if not available\n");
	printf("-C/--cpu               Use a CPU software rasterizer as your GPU, fails if not available\n");
	printf("-V/--validate          Enable validation layers\n");
	printf("-f/--frames start end  Select a measurement frame range\n");
	printf("-w/--wsi wsi           Use the given windowing system [xcb, headless, none]\n");
	printf("-i/--info              Output information about the trace file and exit (affected by debug level)\n");
	printf("-p/--preload size      The size of our readahead buffer and amount of data to preload before starting replay (default %d)\n", (int)p__preload);
	printf("-a/--allow-stalls      Allow stalls if we run out of input data from our readahead thread while in measurement frame range\n");
	printf("-S/--save-cache dir    Save cached objects to the specified directory\n");
	printf("-L/--load-cache dir    Load cached objects from the specified directory\n");
	printf("-B/--blackhole         Do not actually submit any work to the GPU. May be useful for CPU measurements.\n");
	printf("--screenshots frames   Generate PNG screenshots for zero-based global frames N[-M][,...]\n");
	printf("--screenshot-prefix p  Prefix for screenshot PNG names, producing p<frame>.png\n");
	printf("--skip-missing-input   Exit with code 77 if the input trace file does not exist\n");
	printf("--no-multithreaded-io  Do not do decompression and file read in a separate thread. May save some CPU load and memory.\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
	printf("--skip-remove-unused   Do not attempt to cleverly remove unused features and extensions\n");
	printf("Service specific options:\n");
	printf("--service              Turn replay into a provided service listening on a network port\n");
	printf("-P/--port PORT         Port number (default %d)\n", (int)p__port);
	printf("-H/--host HOST         Host name\n");
	printf("Vulkan specific options:\n");
	printf("--swapchain mode       Swapchain mode [virtual, captured, offscreen]\n"); // swapchain offscreen == wsi none
	printf("--virtualperfmode      Performance measurement mode - do not blit from our virtual swapchain to the real swapchain\n");
	printf("--no-dedicated         Do not use dedicated object allocations\n");
	printf("--allocator type       Use custom memory allocator callbacks [none, debug]\n");
	printf("--no-anisotropy        Disable any use of sampler anisotropy\n");
	printf("--presentation mode    Use this Vulkan presentation mode [immediate, mailbox, fifo, fifo_relaxed]\n");
	printf("--swapchain-images num Use this number of swapchain images\n");
	printf("--heap size            Set the suballocator minimum heap size\n");
	exit(-1);
}

static bool cli_command_bypasses_active(const std::vector<std::string>& command)
{
	if (command.size() == 1 && command[0] == "status") return true;
	if (command.size() == 1 && command[0] == "stop") return true;
	if (command.size() == 2 && command[0] == "info" && command[1] == "threads") return true;
	return false;
}

static std::string service_command_response(const std::vector<std::string>& command)
{
	std::string response;
		if (command.empty())
		{
			response = "ERROR\n";
		}
		else if (command.size() == 1 && command[0] == "status")
		{
			if (replay_done.load(std::memory_order_acquire)) response = "DONE\n";
			else if (replayer.cli_running.load(std::memory_order_acquire)) response = "RUNNING\n";
			else
			{
				if (cli_thread_ready())
				{
					const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
					lava_file_reader& reader = replayer.file_reader(thread_id);
					response = cli_paused_command_response(reader);
				}
				else
				{
					response = "PAUSED\n";
				}
			}
		}
		else if (command.size() == 1 && command[0] == "continue")
		{
			if (cli_thread_ready())
			{
				lava_file_reader& reader = replayer.file_reader(replayer.cli_thread.load(std::memory_order_acquire));
				cli_clear_function_target(reader);
				reader.cli_call.store(UINT32_MAX, std::memory_order_release);
			}
			replayer.cli_running.store(true, std::memory_order_release);
			replayer.cli_running.notify_all();
			response = cli_wait_for_done_and_idle();
		}
		else if (command.size() == 1 && command[0] == "stop")
		{
			service_stop_requested.store(true, std::memory_order_release);
			replayer.cli_running.store(true, std::memory_order_release);
			replayer.cli_running.notify_all();
			replayer.request_stop();
			done_var.store(true, std::memory_order_release);
			done_var.notify_all();
			response = "OK\n";
		}
		else if (command.size() == 3 && command[0] == "set" && command[1] == "debug")
		{
			uint_fast8_t level = 0;
			if (parse_debug_level(command[2], level))
			{
				p__debug_level = level;
				response = "OK\n";
			}
			else
			{
				response = "ERROR\n";
			}
		}
		else if (command.size() == 3 && command[0] == "set" && command[1] == "blackhole")
		{
			bool enabled = false;
			if (parse_bool(command[2], enabled))
			{
				p__blackhole = enabled ? 1 : 0;
				response = "OK\n";
			}
			else
			{
				response = "ERROR\n";
			}
		}
		else if (command[0] == "step")
		{
			uint32_t count = 1;
			cli_step_mode step_mode = cli_step_mode::packets;
			if (command.size() == 3 && (command[1] == "calls" || command[1] == "packets"))
			{
				step_mode = command[1] == "calls" ? cli_step_mode::calls : cli_step_mode::packets;
				if (!parse_positive_u32(command[2], count))
				{
					response = "ERROR\n";
				}
			}
			else if (command.size() != 1)
			{
				response = "ERROR\n";
			}
			if (response.empty())
			{
				if (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire) || !cli_thread_ready())
				{
					response = "ERROR\n";
				}
				else
				{
					const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
					lava_file_reader& reader = replayer.file_reader(thread_id);
					const uint32_t base_count = cli_completed_count(reader, step_mode);
					if (count > UINT32_MAX - base_count)
					{
						response = "ERROR\n";
					}
					else
					{
						cli_clear_function_target(reader);
						reader.cli_step.store(step_mode, std::memory_order_release);
						reader.cli_call.store(base_count + count, std::memory_order_release);
						replayer.cli_running.store(true, std::memory_order_release);
						replayer.cli_running.notify_all();
						replayer.cli_running.wait(true);
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						response = idle_response == "OK\n" ? cli_paused_command_response(reader) : idle_response;
					}
				}
			}
		}
		else if (command[0] == "goto")
		{
			uint32_t target_packet = 0;
			if (command.size() != 2)
			{
				response = "ERROR\n";
			}
			else if (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire) || !cli_thread_ready())
			{
				response = "ERROR\n";
			}
			else
			{
				const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
				lava_file_reader& reader = replayer.file_reader(thread_id);
				if (parse_u32(command[1], target_packet))
				{
					const uint32_t current_packet = cli_current_packet_index(reader);
					if (target_packet < current_packet)
					{
						response = "ERROR\n";
					}
					else if (target_packet == current_packet && cli_has_paused_command(reader))
					{
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						response = idle_response == "OK\n" ? cli_paused_command_response(reader) : idle_response;
					}
					else
					{
						cli_clear_function_target(reader);
						reader.cli_step.store(cli_step_mode::packets, std::memory_order_release);
						reader.cli_call.store(target_packet + 1, std::memory_order_release);
						replayer.cli_running.store(true, std::memory_order_release);
						replayer.cli_running.notify_all();
						replayer.cli_running.wait(true);
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						if (idle_response != "OK\n") response = idle_response;
						else response = replay_done.load(std::memory_order_acquire) ? "DONE\n" : cli_paused_command_response(reader);
					}
				}
				else
				{
					const uint16_t function_id = retrace_getid(command[1].c_str());
					if (function_id == UINT16_MAX)
					{
						response = "ERROR\n";
					}
					else
					{
						reader.cli_function.store(function_id, std::memory_order_release);
						reader.cli_call.store(cli_current_call(reader) + 1, std::memory_order_release);
						reader.cli_step.store(cli_step_mode::function, std::memory_order_release);
						replayer.cli_running.store(true, std::memory_order_release);
						replayer.cli_running.notify_all();
						replayer.cli_running.wait(true);
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						if (idle_response != "OK\n") response = idle_response;
						else response = replay_done.load(std::memory_order_acquire) ? "DONE\n" : cli_paused_command_response(reader);
					}
				}
			}
		}
		else if (command.size() == 1 && (command[0] == "params" || command[0] == "parameters")) // show parameters
		{
			if (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire) || !cli_thread_ready())
			{
				response = "ERROR\n";
			}
			else
			{
				const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
				lava_file_reader& reader = replayer.file_reader(thread_id);
				if (!cli_has_paused_command(reader))
				{
					response = "ERROR\n";
				}
				else
				{
					replayer.cli_response.clear();
					replayer.cli_params_ready.store(false, std::memory_order_release);
					replayer.cli_params_requested.store(true, std::memory_order_release);
					replayer.cli_params_ready.wait(false);
					response = replayer.cli_response.empty() ? "ERROR\n" : replayer.cli_response;
				}
			}
		}
		else if (command.size() == 3 && command[0] == "show")
		{
			uint32_t index = 0;
			Json::Value v;
			if (replayer.cli_running.load(std::memory_order_acquire) || !parse_u32(command[2], index) || !cli_show_json(command[1].c_str(), index, v))
			{
				response = "ERROR\n";
			}
			else
			{
				response = v.toStyledString();
				if (response.empty() || response.back() != '\n') response += "\n";
			}
		}
		else if (command.size() == 1 && command[0] == "info") // general info
		{
			response = "INFO\n"; // TODO just a placeholder for now
		}
		else if (command.size() == 2 && command[0] == "info" && command[1] == "threads") // list thread info
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
					thread_name,
					cli_thread_state_name(state),
					std::to_string(reader.cli_packet.load(std::memory_order_relaxed)),
					cli_thread_wait_description(reader, state)
				});
			}
			response = out.to_markdown();
		}
		else if (command.size() == 2 && command[0] == "info" && command[1] == "memory")
		{
			if (replayer.cli_running.load(std::memory_order_acquire))
			{
				response = "ERROR\n";
			}
			else
			{
				response = cli_memory_info_response();
			}
		}
		else if (command.size() == 2 && command[0] == "info" && command[1] == "suballocator")
		{
			if (replayer.cli_running.load(std::memory_order_acquire))
			{
				response = "ERROR\n";
			}
			else
			{
				response = cli_suballocator_info_response();
			}
		}
		else if (command.size() == 2 && command[0] == "info" && command[1] == "objects")
		{
			response = trace_metadata_objects_markdown(replayer.packed_file());
		}
		else if (command.size() == 3 && command[0] == "info" && command[1] == "thread")
		{
			uint32_t thread = 0;
			Json::Value v;
			std::string error;
			if (!parse_u32(command[2], thread) || !trace_metadata_thread_json(replayer.packed_file(), thread, v, error))
			{
				response = "ERROR\n";
			}
			else
			{
				response = trace_metadata_json_pretty(v);
				if (response.empty() || response.back() != '\n') response += "\n";
			}
		}
		else if (command.size() == 4 && command[0] == "info" && command[1] == "frame")
		{
			uint32_t thread = 0;
			uint32_t frame = 0;
			Json::Value v;
			std::string error;
			if (!parse_u32(command[2], thread) || !parse_u32(command[3], frame) || !trace_metadata_frame_json(replayer.packed_file(), thread, frame, v, error))
			{
				response = "ERROR\n";
			}
			else
			{
				response = trace_metadata_json_pretty(v);
				if (response.empty() || response.back() != '\n') response += "\n";
			}
		}
		else
		{
			response = "ERROR\n";
		}

	return response;
}

struct service_client_state
{
	std::atomic_uint active_clients{ 0 };
};

static void service_client_done(service_client_state* state)
{
	state->active_clients.fetch_sub(1, std::memory_order_acq_rel);
	state->active_clients.notify_all();
}

static void service_client(service_client_state* state, int client_fd)
{
	set_thread_name("replay-client");
	const std::string keyword = lava_tcp_receive_line(client_fd);
	const std::vector<std::string> command = split_command(keyword);
	std::string response;
	const bool bypass_active = cli_command_bypasses_active(command);
	bool command_active = false;

	if (!bypass_active)
	{
		bool expected = false;
		command_active = replayer.cli_command_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
		if (!command_active)
		{
			response = "ERROR\n";
		}
	}

	if (response.empty())
	{
		response = service_command_response(command);
	}

	if (command_active)
	{
		replayer.cli_command_active.store(false, std::memory_order_release);
	}

	if (!lava_tcp_send_all(client_fd, response))
	{
		ELOG("Failed to send remote control response: %s", strerror(errno));
	}
	close(client_fd);
	service_client_done(state);
}

static void service_listener()
{
	set_thread_name("replay-listener");
	const int listen_fd = lava_tcp_listen(hostname, port);
	ILOG("Remote control listening on %s:%d", hostname.c_str(), port);
	service_client_state client_state;

	while (!done_var.load(std::memory_order_acquire))
	{
		struct pollfd poll_fd = {};
		poll_fd.fd = listen_fd;
		poll_fd.events = POLLIN;
		const int poll_result = poll(&poll_fd, 1, 100);
		if (poll_result < 0)
		{
			if (errno == EINTR) continue;
			ELOG("Failed to poll remote control listener: %s", strerror(errno));
			continue;
		}
		if (poll_result == 0) continue;
		if ((poll_fd.revents & POLLIN) == 0) continue;

		const int client_fd = accept(listen_fd, nullptr, nullptr);
		if (client_fd < 0)
		{
			if (errno == EINTR) continue;
			ELOG("Failed to accept remote control connection: %s", strerror(errno));
			continue;
		}
		client_state.active_clients.fetch_add(1, std::memory_order_acq_rel);
		std::thread client_thread(service_client, &client_state, client_fd);
		client_thread.detach();
	}

	close(listen_fd);
	unsigned active_clients = client_state.active_clients.load(std::memory_order_acquire);
	while (active_clients != 0)
	{
		client_state.active_clients.wait(active_clients);
		active_clients = client_state.active_clients.load(std::memory_order_acquire);
	}
}

static void replay_thread(int thread_id)
{
	lava_file_reader& t = replayer.file_reader(thread_id);
	t.cli_state.store(cli_thread_state::running, std::memory_order_release);
	t.bind_runner_thread();
	if (t.start_measurement_on_thread_entry()) t.start_measurement();
	uint8_t instrtype;
	try
	{
		while ((instrtype = t.step()))
		{
			switchboard_packet(instrtype, t);
			callback_context cb_context{ t };
			const bool needs_packet_pause = instrtype != PACKET_VULKAN_API_CALL || t.cli_step.load(std::memory_order_acquire) == cli_step_mode::packets;
			while (needs_packet_pause && check_cli(cb_context))
			{
				if (instrtype == PACKET_VULKAN_API_CALL) cli_params_unavailable(cb_context);
				else cli_params_packet(cb_context);
			}
			t.cli_packet.fetch_add(1, std::memory_order_relaxed);
		}
	}
	catch (const replay_stop_requested&)
	{
	}
	t.terminated.store(true, std::memory_order_release);
	t.cli_state.store(cli_thread_state::terminated, std::memory_order_release);
	uint64_t worker_local = 0;
	uint64_t runner_local = 0;
	t.stop_measurement(worker_local, runner_local);
}

static void run_multithreaded()
{
	if (p__sandbox_level >= 3) sandbox_level_three();

	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i] = std::thread(replay_thread, i);
	}

	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i].join();
	}
}

static void cleanup_xcb_wsi_objects()
{
	if (strcmp(window_winsys(), "xcb") != 0) return;

	for (uint32_t i = 0; i < index_to_VkSwapchainKHR.size(); i++)
	{
		if (!index_to_VkSwapchainKHR.contains(i)) continue;
		trackedswapchain_replay& t = VkSwapchainKHR_index.at(i);
		VkSwapchainKHR swapchain = index_to_VkSwapchainKHR.at(i);
		if (t.device != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE)
		{
			wrap_vkDeviceWaitIdle(t.device);
			wrap_vkDestroySwapchainKHR(t.device, swapchain, nullptr);
		}
		index_to_VkSwapchainKHR.unset(i);
	}

	VkInstance instance = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < index_to_VkInstance.size(); i++)
	{
		if (index_to_VkInstance.contains(i))
		{
			instance = index_to_VkInstance.at(i);
			break;
		}
	}
	if (instance != VK_NULL_HANDLE)
	{
		for (uint32_t i = 0; i < index_to_VkSurfaceKHR.size(); i++)
		{
			if (!index_to_VkSurfaceKHR.contains(i)) continue;
			window_destroy(instance, i);
			VkSurfaceKHR surface = index_to_VkSurfaceKHR.at(i);
			wrap_vkDestroySurfaceKHR(instance, surface, nullptr);
			index_to_VkSurfaceKHR.unset(i);
		}
	}
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename;
	bool infodump = false;
	bool skip_missing_input = false;
	std::string wsi;
	std::vector<replay_screenshot_range> screenshot_ranges;
	std::string screenshot_prefix = "screenshot_frame_";
	bool screenshot_prefix_set = false;
	bool service = false;
	std::thread service_thread;

	port = p__port;
	if (p__sandbox_level >= 1) sandbox_level_one();

	// override defaults
	//p__allow_stalls = get_env_bool("LAVATUBE_ALLOW_STALLS", false);

	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-o", "--logfile", remaining))
		{
			if (remaining < 1) usage();
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else if (match(argv[i], "-V", "--validate", remaining))
		{
			p__validation = 1;
		}
		else if (match(argv[i], nullptr, "--virtualperfmode", remaining))
		{
			p__virtualperfmode = true;
		}
		else if (match(argv[i], nullptr, "--swapchain", remaining))
		{
			if (remaining < 1) usage();
			std::string val = get_str(argv[++i], remaining);
			if (val == "captured") p__virtualswap = false;
			else if (val == "virtual") p__virtualswap = true;
			else if (val == "offscreen") p__noscreen = 1;
			else ABORT("Bad --swapchain mode");
		}
		else if (match(argv[i], nullptr, "--presentation", remaining))
		{
			if (remaining < 1) usage();
			std::string val = get_str(argv[++i], remaining);
			if (val == "immediate") p__realpresentmode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			else if (val == "mailbox") p__realpresentmode = VK_PRESENT_MODE_MAILBOX_KHR;
			else if (val == "fifo") p__realpresentmode = VK_PRESENT_MODE_FIFO_KHR;
			else if (val == "fifo_relaxed") p__realpresentmode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
			else p__realpresentmode = (VkPresentModeKHR) atoi(val.c_str());
		}
		else if (match(argv[i], nullptr, "--swapchainimages", remaining))
		{
			p__realimages = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-D", "--device", remaining))
		{
			p__device = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-P", "--port", remaining))
		{
			port = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-H", "--host", remaining))
		{
			hostname = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], nullptr, "--service", remaining))
		{
			service = true;
		}
		else if (match(argv[i], "-C", "--cpu", remaining))
		{
			p__cpu = true;
		}
		else if (match(argv[i], "-G", "--gpu", remaining))
		{
			p__gpu = true;
		}
		else if (match(argv[i], "-f", "--frames", remaining))
		{
			if (remaining < 2) usage();
			start = get_int(argv[++i], remaining);
			end = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-i", "--info", remaining))
		{
			infodump = true;
		}
		else if (match(argv[i], nullptr, "--no-dedicated", remaining))
		{
			p__dedicated_allocation = false;
		}
		else if (match(argv[i], nullptr, "--no-anisotropy", remaining))
		{
			p__no_anisotropy = true;
		}
		else if (match(argv[i], nullptr, "--allocator", remaining))
		{
			std::string allocator = get_str(argv[++i], remaining);
			if (allocator == "none") p__custom_allocator = 0;
			else if (allocator == "debug") p__custom_allocator = 1;
			else
			{
				DIE("Unsupported custom allocator: %s", allocator.c_str());
			}
		}
		else if (match(argv[i], nullptr, "--heap", remaining))
		{
			p__suballocator_heap_size = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-S", "--save-cache", remaining))
		{
			p__save_pipelinecache = argv[++i];
			remaining--;
		}
		else if (match(argv[i], "-s", "--sandbox", remaining))
		{
			p__sandbox_level = get_int(argv[++i], remaining);
			if (p__sandbox_level <= 0 || p__sandbox_level > 3) DIE("Invalid sandbox level %d", (int)p__sandbox_level);
		}
		else if (match(argv[i], "-L", "--load-cache", remaining))
		{
			p__load_pipelinecache = argv[++i];
			remaining--;
		}
		else if (match(argv[i], "-p", "--preload", remaining))
		{
			p__preload = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-a", "--allow-stalls", remaining))
		{
			p__allow_stalls = 1;
		}
		else if (match(argv[i], nullptr, "--skip-remove-unused", remaining))
		{
			p__skip_remove_unused = 1;
		}
		else if (match(argv[i], "-B", "--blackhole", remaining))
		{
			p__blackhole = 1;
		}
		else if (match(argv[i], nullptr, "--screenshots", remaining))
		{
			std::string error;
			if (!parse_replay_screenshot_ranges(get_str(argv[++i], remaining), screenshot_ranges, error))
			{
				DIE("Bad --screenshots value: %s", error.c_str());
			}
		}
		else if (match(argv[i], nullptr, "--screenshot-prefix", remaining))
		{
			screenshot_prefix = get_str(argv[++i], remaining);
			screenshot_prefix_set = true;
		}
		else if (match(argv[i], nullptr, "--skip-missing-input", remaining))
		{
			skip_missing_input = true;
		}
		else if (match(argv[i], nullptr, "--no-multithreaded-io", remaining))
		{
			p__disable_multithread_read = 1;
		}
		else if (match(argv[i], "-w", "--wsi", remaining))
		{
			wsi = get_str(argv[++i], remaining);
			if (wsi == "none")
			{
				p__noscreen = 1;
			}
			else if (wsi != "xcb" && wsi != "wayland" && wsi != "headless")
			{
				DIE("Non-supported window system: %s", wsi.c_str());
			}
		}
		else if (strcmp(argv[i], "--") == 0) // eg in case you have a file named -f ...
		{
			remaining--;
			filename = get_str(argv[++i], remaining);
			if (remaining > 0) usage();
			break; // stop parsing cmd line options
		}
		else
		{
			filename = get_str(argv[i], remaining);
			if (remaining > 0)
			{
				printf("Invalid options\n\n");
				usage();
			}
		}
	}

	if (p__noscreen && !p__virtualswap) DIE("The \"none\" WSI can only be used with a virtual swapchain!");
	if (p__realimages > 0 && !p__virtualswap) DIE("Setting the number of virtual images can only be done with a virtual swapchain!");
	if (p__realpresentmode != VK_PRESENT_MODE_MAX_ENUM_KHR && !p__virtualswap) DIE("Changing present mode can only be used with a virtual swapchain!");
	if (p__cpu && p__gpu) DIE("Cannot use both --cpu/-C and --gpu/-G at the same time!");
	if (screenshot_prefix_set && screenshot_ranges.empty()) DIE("The --screenshot-prefix option requires --screenshots");
	if (!screenshot_ranges.empty() && !p__virtualswap) DIE("The --screenshots option currently only supports the virtual/offscreen swapchain path");
	if (!screenshot_ranges.empty() && p__blackhole) DIE("The --screenshots option cannot be used together with --blackhole");
	if (service && infodump) DIE("The --service option cannot be used together with --info");

	if (filename.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}

	if (skip_missing_input && access(filename.c_str(), R_OK) != 0)
	{
		printf("SKIP: input trace file does not exist or is not readable: %s\n", filename.c_str());
		return 77;
	}

	if (wsi.empty()) wsi_initialize(nullptr);
	else wsi_initialize(wsi.c_str());

	if (service)
	{
		replayer.cli_service.store(true, std::memory_order_release);
		replayer.cli_pipeline_executable_stats_requested = true;
		replayer.cli_memory_budget_requested = true;
	}

	if (service)
	{
		service_thread = std::thread(service_listener);
	}

	if (p__sandbox_level >= 2) sandbox_level_two();

	VkuVulkanLibrary library = vkuCreateWrapper();
	replayer.set_frames(start, end);
	replayer.set_screenshot_prefix(std::move(screenshot_prefix));
	replayer.set_screenshot_ranges(std::move(screenshot_ranges));
	replayer.init(filename);
	register_replay_callbacks();
	if (infodump)
	{
		replayer.dump_info();
		exit(EXIT_SUCCESS);
	}

	if (service)
	{
		replayer.cli_thread.store(0, std::memory_order_release); // set currently probed thread, indicates active CLI operation
		replayer.cli_running.wait(false);
		if (service_stop_requested.load(std::memory_order_acquire))
		{
			replay_done.store(true, std::memory_order_release);
			replay_done.notify_all();
			service_thread.join();
			close_debug_destination();
			return replayer.exit_status;
		}
	}

	run_multithreaded();
	if (service)
	{
		replay_done.store(true, std::memory_order_release);
		replay_done.notify_all();
		replayer.cli_running.store(false, std::memory_order_release);
		replayer.cli_running.notify_all();
		done_var.wait(false);
		service_thread.join();
	}
	replayer.destroy_screenshot_resources();
	replayer.finalize();
	if (p__custom_allocator) allocators_print(stdout);
	if (!replayer.cleanup_after_stop()) cleanup_xcb_wsi_objects();
	vkuDestroyWrapper(library);
	wsi_shutdown();
	close_debug_destination();
	return replayer.exit_status;
}
