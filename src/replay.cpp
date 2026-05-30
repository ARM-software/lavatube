#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
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

static bool parse_positive_u32(const std::string& text, uint32_t& out)
{
	char* end = nullptr;
	errno = 0;
	const unsigned long value = strtoul(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0' || value == 0 || value > std::numeric_limits<uint32_t>::max())
	{
		return false;
	}
	out = (uint32_t)value;
	return true;
}

static bool cli_thread_ready()
{
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	return thread_id >= 0 && thread_id < (int)replayer.threads.size();
}

static uint32_t cli_completed_call(lava_file_reader& reader)
{
	const uint32_t paused_call = reader.cli_paused_call.load(std::memory_order_acquire);
	if (paused_call != 0) return paused_call;
	return replayer.thread_call_numbers->at(reader.thread_index()).load(std::memory_order_relaxed);
}

static bool cli_has_paused_command(lava_file_reader& reader)
{
	return reader.cli_paused_call.load(std::memory_order_acquire) != 0;
}

static std::string cli_paused_command_response(lava_file_reader& reader)
{
	const char* api_name = "-";
	if (reader.current.call_id != UINT16_MAX)
	{
		api_name = get_function_name(reader.current.call_id);
	}
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	return "PAUSED @ call=" + std::to_string(cli_completed_call(reader)) + " api=" + api_name + " frame="
	       + std::to_string(replayer.global_frame) + "/" + std::to_string(replayer.global_frame_count)
	       + " thread=" + std::to_string(thread_id) + "\n";
}

static void register_replay_callbacks()
{
#define CALLBACK(x) x ## _callbacks.push_back(replay_callback_ ## x)
	CALLBACK(vkCreateInstance);
	CALLBACK(vkDestroyInstance);
	CALLBACK(vkQueuePresentKHR);
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

static void service_listener()
{
	set_thread_name("replay-listener");
	const int listen_fd = lava_tcp_listen(hostname, port);
	ILOG("Remote control listening on %s:%d", hostname.c_str(), port);

	while (!done_var.load(std::memory_order_acquire))
	{
		const int client_fd = accept(listen_fd, nullptr, nullptr);
		if (client_fd < 0)
		{
			if (errno == EINTR) continue;
			ELOG("Failed to accept remote control connection: %s", strerror(errno));
			continue;
		}

		const std::string keyword = lava_tcp_receive_line(client_fd);
		const std::vector<std::string> command = split_command(keyword);
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
				const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
				lava_file_reader& reader = replayer.file_reader(thread_id);
				if (cli_thread_ready()) response = cli_paused_command_response(reader);
				else response = "PAUSED";
				response += "\n";
			}
		}
		else if (command.size() == 1 && command[0] == "continue")
		{
			if (cli_thread_ready())
			{
				lava_file_reader& reader = replayer.file_reader(replayer.cli_thread.load(std::memory_order_acquire));
				reader.cli_call.store(UINT32_MAX, std::memory_order_release);
			}
			replayer.cli_running.store(true, std::memory_order_release);
			replayer.cli_running.notify_all();
			response = "OK\n";
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
		else if (command[0] == "step")
		{
			uint32_t calls = 1;
			if (command.size() == 3 && command[1] == "calls")
			{
				if (!parse_positive_u32(command[2], calls))
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
					const uint32_t base_call = cli_completed_call(reader);
					if (calls > UINT32_MAX - base_call)
					{
						response = "ERROR\n";
					}
					else
					{
						reader.cli_call.store(base_call + calls, std::memory_order_release);
						replayer.cli_running.store(true, std::memory_order_release);
						replayer.cli_running.notify_all();
						replayer.cli_running.wait(true);
						response = cli_paused_command_response(reader);
					}
				}
			}
			// TODO we should not return until _all_ threads are back in pause state
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
		else if (command.size() == 1 && command[0] == "info") // general info
		{
			response = "INFO\n"; // TODO just a placeholder for now
		}
		else if (command.size() == 2 && command[0] == "info" && command[1] == "threads") // list thread info
		{
			data_table out;
			out.set_headers({"Thread", "Name", "State"});
			ILOG("Threads = %d", (int)replayer.threads.size());
			for (unsigned i = 0; i < replayer.threads.size(); i++)
			{
				const char* thread_name = replayer.file_reader(i).get_trace_thread_name();
				out.add_row({std::to_string(i), thread_name, "-"});
				ILOG("Adding thread %u : %s", i, thread_name);
			}
			response = out.to_markdown();
		}
		else
		{
			response = "ERROR\n";
		}

		if (!lava_tcp_send_all(client_fd, response))
		{
			ELOG("Failed to send remote control response: %s", strerror(errno));
		}
		close(client_fd);
	}
	close(listen_fd);
}

static void replay_thread(int thread_id)
{
	lava_file_reader& t = replayer.file_reader(thread_id);
	t.bind_runner_thread();
	if (t.start_measurement_on_thread_entry()) t.start_measurement();
	uint8_t instrtype;
	try
	{
		while ((instrtype = t.step()))
		{
			switchboard_packet(instrtype, t);
		}
	}
	catch (const replay_stop_requested&)
	{
	}
	t.terminated.store(true, std::memory_order_release);
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
			service_thread.join();
			close_debug_destination();
			return replayer.exit_status;
		}
	}

	run_multithreaded();
	if (service)
	{
		replay_done.store(true, std::memory_order_release);
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
