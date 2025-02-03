#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
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
#include "sandbox.h"

static lava_reader replayer;
static bool no_sandbox = false;

static void usage()
{
	printf("lava-replay %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-g/--gpu gpu           Select physical device to use (by index value)\n");
	printf("-o/--debugfile FILE    Output debug output to the given file\n");
	printf("-V/--validate          Enable validation layers\n");
	printf("-f/--frames start end  Select a frame range\n");
	//printf("-p/--preload           Load entire selected frame range into memory before running it\n");
	printf("-w/--wsi wsi           Use the given windowing system [xcb, wayland, headless, none]\n");
	printf("-v/--virtual           Use a virtual swapchain\n");
	printf("  -vpm mode            Use this presentation mode for our real swapchain [immediate, mailbox, fifo, fifo_relaxed]\n");
	printf("  -vi images           Use this number of images with our real swapchain\n");
	printf("  -vp                  Performance measurement mode - do not blit from our virtual swapchain to the real swapchain\n");
	printf("-i/--info              Output information about the trace file and exit (affected by debug level)\n");
	printf("-H/--heap size         Set the suballocator minimum heap size\n");
	printf("-S/--save-cache dir    Save created pipeline objects to the specified directory\n");
	printf("-L/--load-cache dir    Load the pipeline caches from the specified directory\n");
	printf("-D/--no-dedicated      Do not use dedicated object allocations\n");
	printf("-A/--allocator type    Use custom memory allocator callbacks [none, debug]\n");
	printf("-N/--no-anisotropy     Disable any use of sampler anisotropy\n");
	printf("-B/--blackhole         Do not actually submit any work to the GPU. May be useful for CPU measurements.\n");
	printf("-nm/--no-multithread   Do not do decompression and file read in a separate thread. May save some CPU load and memory.\n");
	printf("-ns/--no-sandbox       Do not run inside a compartmentalized sandbox mode for security.\n");
	exit(-1);
}

static inline bool match(const char* in, const char* short_form, const char* long_form, int& remaining)
{
	if (strcmp(in, short_form) == 0 || strcmp(in, long_form) == 0)
	{
		remaining--;
		return true;
	}
	return false;
}

static int get_int(const char* in, int& remaining)
{
	if (remaining == 0)
	{
		usage();
	}
	remaining--;
	return atoi(in);
}

static std::string get_str(const char* in, int& remaining)
{
	if (remaining == 0)
	{
		usage();
	}
	remaining--;
	return in;
}

static void replay_thread(int thread_id)
{
	lava_file_reader& t = replayer.file_reader(thread_id);
	uint8_t instrtype;
	while ((instrtype = t.step()))
	{
		if (instrtype == PACKET_VULKAN_API_CALL)
		{
			t.read_apicall();
		}
		else if (instrtype == PACKET_THREAD_BARRIER)
		{
			t.read_barrier();
		}
		else if (instrtype == PACKET_IMAGE_UPDATE)
		{
			const uint32_t device_index = t.read_handle();
			const uint32_t image_index = t.read_handle();
			image_update(t, device_index, image_index);
		}
		else if (instrtype == PACKET_BUFFER_UPDATE)
		{
			const uint32_t device_index = t.read_handle();
			const uint32_t buffer_index = t.read_handle();
			buffer_update(t, device_index, buffer_index);
		}
		t.device = VK_NULL_HANDLE;
		t.physicalDevice = VK_NULL_HANDLE;
	}
}

static void run_multithreaded(int n)
{
	if (!no_sandbox)
	{
		const char* err = sandbox_replay_start();
		if (err) WLOG("Warning: Failed to change sandbox to replay mode: %s", err);
	}

	for (int i = 0; i < n; i++)
	{
		replayer.threads.emplace_back(replay_thread, i);
	}

	for (unsigned i = 0; i < replayer.threads.size(); i++)
	{
		replayer.threads[i].join();
	}
}

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int heap_size = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename;
	bool preload = false;
	bool infodump = false;
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
		else if (match(argv[i], "-o", "--debugfile", remaining))
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
		else if (match(argv[i], "-v", "--virtual", remaining))
		{
			p__virtualswap = true;
		}
		else if (match(argv[i], "-vp", "--virtualperfmode", remaining))
		{
			p__virtualperfmode = true;
		}
		else if (match(argv[i], "-vpm", "--realpresentationmode", remaining))
		{
			if (remaining < 1) usage();
			std::string val = get_str(argv[++i], remaining);
			if (val == "immediate") p__realpresentmode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			else if (val == "mailbox") p__realpresentmode = VK_PRESENT_MODE_MAILBOX_KHR;
			else if (val == "fifo") p__realpresentmode = VK_PRESENT_MODE_FIFO_KHR;
			else if (val == "fifo_relaxed") p__realpresentmode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
			else p__realpresentmode = (VkPresentModeKHR) atoi(val.c_str());
		}
		else if (match(argv[i], "-vi", "--realimages", remaining))
		{
			if (remaining < 1) usage();
			p__realimages = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-g", "--gpu", remaining))
		{
			if (remaining < 1) usage();
			p__gpu = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-f", "--frames", remaining))
		{
			if (remaining < 2) usage();
			start = get_int(argv[++i], remaining);
			end = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-p", "--preload", remaining))
		{
			preload = true;
		}
		else if (match(argv[i], "-i", "--info", remaining))
		{
			infodump = true;
		}
		else if (match(argv[i], "-D", "--no-dedicated", remaining))
		{
			p__dedicated_allocation = false;
		}
		else if (match(argv[i], "-N", "--no-anisotropy", remaining))
		{
			p__no_anisotropy = true;
		}
		else if (match(argv[i], "-A", "--allocator", remaining))
		{
			std::string allocator = get_str(argv[++i], remaining);
			if (allocator == "none") p__custom_allocator = 0;
			else if (allocator == "debug") p__custom_allocator = 1;
			else
			{
				DIE("Unsupported custom allocator: %s", allocator.c_str());
			}
		}
		else if (match(argv[i], "-H", "--heap", remaining))
		{
			heap_size = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-S", "--save-cache", remaining))
		{
			p__save_pipelinecache = argv[++i];
			remaining--;
		}
		else if (match(argv[i], "-L", "--load-cache", remaining))
		{
			p__load_pipelinecache = argv[++i];
			remaining--;
		}
		else if (match(argv[i], "-B", "--blackhole", remaining))
		{
			p__blackhole = 1;
		}
		else if (match(argv[i], "-nm", "--no-multithread", remaining))
		{
			p__disable_multithread_read = 1;
		}
		else if (match(argv[i], "-ns", "--no-sandbox", remaining))
		{
			no_sandbox = true;
		}
		else if (match(argv[i], "-w", "--wsi", remaining))
		{
			std::string wsi = get_str(argv[++i], remaining);
			if (wsi == "none")
			{
				p__noscreen = 1;
			}
			else if (wsi != "xcb" && wsi != "wayland" && wsi != "headless")
			{
				DIE("Non-supported window system: %s", wsi.c_str());
			}
			window_set_winsys(wsi);
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
				printf("Options after filename is not valid!\n\n");
				usage();
			}
		}
	}

	if (p__noscreen && !p__virtualswap) DIE("The \"none\" WSI can only be used with a virtual swapchain!");
	if (p__realimages > 0 && !p__virtualswap) DIE("Setting the number of virtual images can only be done with a virtual swapchain!");
	if (p__realpresentmode != VK_PRESENT_MODE_MAX_ENUM_KHR && !p__virtualswap) DIE("Changing present mode can only be used with a virtual swapchain!");

	if (!no_sandbox)
	{
		const char* err = sandbox_tool_init();
		if (err) WLOG("Warning: Failed to initialize sandbox: %s", err);
	}

	if (filename.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}

	VkuVulkanLibrary library = vkuCreateWrapper();
	replayer.init(filename, heap_size);
	if (infodump)
	{
		replayer.dump_info();
		exit(EXIT_SUCCESS);
	}
	replayer.parameters(start, end, preload);

	// Read all thread files
	std::vector<std::string> threadfiles = packed_files(filename, "thread_");
	if (threadfiles.size() == 0) DIE("Failed to find any threads in %s!", filename.c_str());
	run_multithreaded(threadfiles.size());
	if (p__custom_allocator) allocators_print(stdout);
	vkuDestroyWrapper(library);
	if (p__debug_destination) fclose(p__debug_destination);
	return 0;
}
