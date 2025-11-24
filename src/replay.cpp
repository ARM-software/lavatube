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

static void usage()
{
	printf("lava-replay %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("-h/--help              This help\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
#endif
	printf("-o/--logfile FILE      Output log output to the given file\n");
	printf("-g/--gpu gpu           Select physical device to use (by index value)\n");
	printf("-V/--validate          Enable validation layers\n");
	printf("-f/--frames start end  Select a measurement frame range\n");
	printf("-w/--wsi wsi           Use the given windowing system [xcb, wayland, headless, none]\n");
	printf("-i/--info              Output information about the trace file and exit (affected by debug level)\n");
	printf("-p/--preload size      The amount of file data to preload before starting replay (default %d)\n", (int)p__preload);
	printf("-a/--allow-stalls      Allow stalls if we run out of input data from our readahead thread while in measurement frame range\n");
	printf("-S/--save-cache dir    Save cached objects to the specified directory\n");
	printf("-L/--load-cache dir    Load cached objects from the specified directory\n");
	printf("-B/--blackhole         Do not actually submit any work to the GPU. May be useful for CPU measurements.\n");
	printf("--no-multithreaded-io  Do not do decompression and file read in a separate thread. May save some CPU load and memory.\n");
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)p__sandbox_level);
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

static inline bool match(const char* in, const char* short_form, const char* long_form, int& remaining)
{
	if ((short_form && strcmp(in, short_form) == 0) || (long_form && strcmp(in, long_form) == 0))
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
			DLOG2("Update image packet on thread %d", thread_id);
			const uint32_t device_index = t.read_handle(DEBUGPARAM("VkDevice"));
			const uint32_t image_index = t.read_handle(DEBUGPARAM("VkImage"));
			image_update(t, device_index, image_index);
		}
		else if (instrtype == PACKET_BUFFER_UPDATE)
		{
			DLOG2("Update buffer packet on thread %d", thread_id);
			const uint32_t device_index = t.read_handle(DEBUGPARAM("VkDevice"));
			const uint32_t buffer_index = t.read_handle(DEBUGPARAM("VkBuffer"));
			buffer_update(t, device_index, buffer_index);
		}
		else if (instrtype == PACKET_TENSOR_UPDATE)
		{
			DLOG2("Update tensor packet on thread %d", thread_id);
			const uint32_t device_index = t.read_handle(DEBUGPARAM("VkDevice"));
			const uint32_t tensor_index = t.read_handle(DEBUGPARAM("VkTensorARM"));
			tensor_update(t, device_index, tensor_index);
		}
		t.device = VK_NULL_HANDLE;
		t.physicalDevice = VK_NULL_HANDLE;
	}
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

int main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int heap_size = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename;
	bool infodump = false;
	std::string wsi;

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
			heap_size = get_int(argv[++i], remaining);
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
		else if (match(argv[i], "-B", "--blackhole", remaining))
		{
			p__blackhole = 1;
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
				printf("Options after filename is not valid!\n\n");
				usage();
			}
		}
	}

	if (p__noscreen && !p__virtualswap) DIE("The \"none\" WSI can only be used with a virtual swapchain!");
	if (p__realimages > 0 && !p__virtualswap) DIE("Setting the number of virtual images can only be done with a virtual swapchain!");
	if (p__realpresentmode != VK_PRESENT_MODE_MAX_ENUM_KHR && !p__virtualswap) DIE("Changing present mode can only be used with a virtual swapchain!");

	if (wsi.empty()) wsi_initialize(nullptr);
	else wsi_initialize(wsi.c_str());

	if (p__sandbox_level >= 2) sandbox_level_two();

	if (filename.empty())
	{
		printf("No file argument given\n\n");
		usage();
	}

	VkuVulkanLibrary library = vkuCreateWrapper();
	replayer.set_frames(start, end);
	replayer.init(filename, heap_size);
	if (infodump)
	{
		replayer.dump_info();
		exit(EXIT_SUCCESS);
	}

	run_multithreaded();
	if (p__custom_allocator) allocators_print(stdout);
	wsi_shutdown();
	vkuDestroyWrapper(library);
	if (p__debug_destination) fclose(p__debug_destination);
	return 0;
}
