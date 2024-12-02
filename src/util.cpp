#include "util.h"

#include <string.h>
#include <spirv/unified1/spirv.h>

#if defined(_GNU_SOURCE) || defined(__BIONIC__)
#include <pthread.h>
#else
#include <sys/prctl.h>
#endif
void set_thread_name(const char* name)
{
	// "length is restricted to 16 characters, including the terminating null byte"
	// http://man7.org/linux/man-pages/man3/pthread_setname_np.3.html
	assert(strlen(name) <= 15);
#if defined(_GNU_SOURCE) || defined(__BIONIC__)
	pthread_setname_np(pthread_self(), name);
#else
	prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#endif
}

void get_thread_name(char *name)
{
	memset(name, 0, 16); // it had better be 16 length!
#if defined(_GNU_SOURCE) || defined(__BIONIC__)
	pthread_getname_np(pthread_self(), name, 16);
#else
	prctl(PR_GET_NAME, name);
#endif
}

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
int STOI(const std::string& value)
{
    int out;
    std::istringstream(value) >> out;
    return out;
}
#endif

static int get_env_int(const char* name, int v)
{
	const char* tmpstr = getenv(name);
	if (tmpstr)
	{
		v = atoi(tmpstr);
	}
	return v;
}

static int get_env_bool(const char* name, int v)
{
	const char* tmpstr = getenv(name);
	if (tmpstr)
	{
		if (tmpstr[0] == 'F' || tmpstr[0] == 'f') return 0;
		else if (tmpstr[0] == 'T' || tmpstr[0] == 't') return 1;
		v = atoi(tmpstr);
		if (v > 1 || v < 0) { fprintf(stderr, "Invalid value for parameter %s: %s\n", name, tmpstr); exit(-1); }
	}
	return v;
}

static FILE* get_env_file(const char* name, FILE* fallback)
{
	const char* tmpstr = getenv(name);
	if (tmpstr)
	{
		FILE* fp = fopen(tmpstr, "w");
		if (fp) return fp; // it is implicitly closed on exit
	}
	return fallback;
}

uint_fast8_t p__virtualqueues = get_env_bool("LAVATUBE_VIRTUAL_QUEUES", 0);
FILE* p__debug_destination = get_env_file("LAVATUBE_DEBUG_FILE", stdout); // must be defined first here
uint_fast8_t p__blackhole = get_env_bool("LAVATUBE_BLACKHOLE", 0);
uint_fast8_t p__dedicated_buffer = get_env_bool("LAVATUBE_DEDICATED_BUFFER", 0);
uint_fast8_t p__dedicated_image = get_env_bool("LAVATUBE_DEDICATED_IMAGE", 0);
uint_fast8_t p__gpu = get_env_int("LAVATUBE_GPU", 0);
uint_fast8_t p__debug_level = get_env_int("LAVATUBE_DEBUG", 0);
uint_fast8_t p__validation = get_env_bool("LAVATUBE_VALIDATION", 0);
uint_fast8_t p__swapchains = get_env_int("LAVATUBE_SWAPCHAINS", 3); // zero means do not override
uint_fast8_t p__noscreen = get_env_bool("LAVATUBE_NOSCREEN", 0);
uint_fast8_t p__virtualswap = get_env_bool("LAVATUBE_VIRTUALSWAPCHAIN", 0);
uint_fast8_t p__virtualperfmode = get_env_bool("LAVATUBE_VIRTUALSWAPCHAIN_PERFMODE", 0);
VkPresentModeKHR p__realpresentmode = (VkPresentModeKHR)get_env_int("LAVATUBE_VIRTUALSWAPCHAIN_PRESENTMODE", VK_PRESENT_MODE_MAX_ENUM_KHR);
uint_fast8_t p__realimages = get_env_int("LAVATUBE_VIRTUALSWAPCHAIN_IMAGES", 0); // zero means do not override
const char* p__save_pipelinecache = getenv("LAVATUBE_SAVE_PIPELINECACHE");
const char* p__load_pipelinecache = getenv("LAVATUBE_LOAD_PIPELINECACHE");
uint_fast8_t p__dedicated_allocation = get_env_bool("LAVATUBE_DEDICATED_ALLOCATION", 1);
uint_fast8_t p__custom_allocator = get_env_bool("LAVATUBE_CUSTOM_ALLOCATOR", 0);
uint_fast8_t p__no_anisotropy = get_env_bool("LAVATUBE_NO_ANISOTROPY", 0);
uint_fast8_t p__delay_fence_success_frames = get_env_int("LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES", 0); // off by default
int p__chunksize = get_env_int("LAVATUBE_CHUNK_SIZE", 64 * 1024 * 1024);
uint_fast8_t p__external_memory = get_env_bool("LAVATUBE_EXTERNAL_MEMORY", 0);

const char* errorString(const VkResult errorCode)
{
	switch (errorCode)
	{
#define STR(r) case VK_ ##r: return #r
	STR(SUCCESS);
	STR(NOT_READY);
	STR(TIMEOUT);
	STR(EVENT_SET);
	STR(EVENT_RESET);
	STR(INCOMPLETE);
	STR(ERROR_OUT_OF_HOST_MEMORY);
	STR(ERROR_OUT_OF_DEVICE_MEMORY);
	STR(ERROR_INITIALIZATION_FAILED);
	STR(ERROR_DEVICE_LOST);
	STR(ERROR_MEMORY_MAP_FAILED);
	STR(ERROR_LAYER_NOT_PRESENT);
	STR(ERROR_EXTENSION_NOT_PRESENT);
	STR(ERROR_FEATURE_NOT_PRESENT);
	STR(ERROR_INCOMPATIBLE_DRIVER);
	STR(ERROR_TOO_MANY_OBJECTS);
	STR(ERROR_FORMAT_NOT_SUPPORTED);
	STR(ERROR_FRAGMENTED_POOL);
	STR(ERROR_UNKNOWN);
	STR(ERROR_OUT_OF_POOL_MEMORY);
	STR(ERROR_INVALID_EXTERNAL_HANDLE);
	STR(ERROR_FRAGMENTATION);
	STR(ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
	STR(PIPELINE_COMPILE_REQUIRED);
	STR(ERROR_SURFACE_LOST_KHR);
	STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
	STR(SUBOPTIMAL_KHR);
	STR(ERROR_OUT_OF_DATE_KHR);
	STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
	STR(ERROR_VALIDATION_FAILED_EXT);
	STR(ERROR_INVALID_SHADER_NV);
	STR(ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
	STR(ERROR_NOT_PERMITTED_KHR);
	STR(ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
	STR(THREAD_IDLE_KHR);
	STR(THREAD_DONE_KHR);
	STR(OPERATION_DEFERRED_KHR);
	STR(OPERATION_NOT_DEFERRED_KHR);
	STR(ERROR_COMPRESSION_EXHAUSTED_EXT);
	STR(ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR);
	STR(ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR);
	STR(ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
	STR(ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR);
	STR(ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
	STR(ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR);
	STR(ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
	STR(ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR);
	STR(PIPELINE_BINARY_MISSING_KHR);
	STR(ERROR_NOT_ENOUGH_SPACE_KHR);
#undef STR
	case VK_RESULT_MAX_ENUM:
		return "(bad error code)";
	}
	return "(unrecognized error code)";
}

void check_retval(VkResult stored_retval, VkResult retval)
{
	if (stored_retval == VK_SUCCESS && retval != VK_SUCCESS)
	{
		const char* err = errorString(retval);
		ABORT("LAVATUBE ERROR: Returncode does not match stored value, got error: %s (code %u)", err, (unsigned)retval);
	}
}

const std::vector<std::string> split(const std::string& str, const char& delimiter)
{
	std::vector<char> buffer;
	buffer.reserve(100);

	std::vector<std::string> tokens;

	for(auto alpha : str) {
		if (alpha != delimiter) {
			buffer.push_back(alpha);
		} else if (alpha == delimiter && buffer.size() != 0) {
			tokens.push_back(std::string(&buffer[0], buffer.size()));
			buffer.clear();
		}
	}

	if (buffer.size() != 0) {
		tokens.push_back(std::string(&buffer[0], buffer.size()));
	}

	return tokens;
}

const std::string join(const std::vector<std::string>& tokens, char joiner)
{
	if (tokens.size() == 0) {
		return "";
	} else if (tokens.size() == 1) {
		return tokens[0];
	}

	std::string result = "";

	for (auto token : tokens)
	{
		result += token + joiner;
	}

	result.erase(result.end() - 1);

	return result;
}

std::string get_trace_path(const std::string& base)
{
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	std::string dir = AndroidGlobs::ANDROID_OUT_TRACE_PATH;
	return dir;
#else
	const char* outpath = getenv("LAVATUBE_DESTINATION");
	if (outpath) return outpath;
	return base;
#endif
}

std::string get_vulkan_lib_path()
{
	std::string filepath = "";
	std::string path = "";
	std::string soname = "";

#ifndef VK_USE_PLATFORM_ANDROID_KHR
	path = std::string(getenv("VULKAN_PATH") ? getenv("VULKAN_PATH") : "");
	soname = std::string(getenv("VULKAN_SONAME") ? getenv("VULKAN_SONAME") : "");
#endif

	if (path.empty()) // try to get system vulkan library
	{
		std::vector<std::string> ld_paths = {
#if VK_USE_PLATFORM_ANDROID_KHR
#if __LP64__
			"/system/lib64",
#else
			"/system/lib",
#endif
#elif __LP64__ // not android
			"/usr/lib64",
			"/usr/local/lib64",
			"/usr/lib",
			"/usr/lib/x86_64-linux-gnu",
#else // not android, and not 64bit
			"/usr/lib32",
			"/usr/local/lib32",
			"/usr/lib",
#endif
		};

#ifndef VK_USE_PLATFORM_ANDROID_KHR
		// Add all LD_LIBRARY_PATH dirs
		const std::vector<std::string> ld_lib_paths = split(std::string(getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : ""), ':');
		for (const auto& mypath : ld_lib_paths)
		{
			ld_paths.push_back(mypath);
		}
#endif

		std::vector<std::string> paths;

		for (auto& ld_path : ld_paths)
		{
			if (!soname.empty())
			{
				if (soname[0] != '/')
				{
					paths.push_back(ld_path + "/" + soname);
				} else {
					paths.push_back(ld_path + soname);
				}
			} else {
				paths.push_back(ld_path + "/libvulkan.so.1");
				paths.push_back(ld_path + "/libvulkan.so");
			}
		}

		for (const std::string& p : paths)
		{
			if (access(p.c_str(), R_OK) == 0)
			{
				filepath = p;
				break;
			}
		}

		if (filepath.empty())
		{
			FELOG("Failed to find a vulkan loader. Make sure your libvulkan.so(.1) is available on your system.");
			FELOG("Tried the following paths:");
			for (const std::string& p : paths)
			{
				FELOG("\t%s", p.c_str());
			}
			ABORT("Giving up");
		}

		ILOG("Found system Vulkan library: %s", filepath.c_str());
	}
	else // get specified vulkan library
	{
		filepath = std::string(path) + std::string("/") + (soname.empty() ? "libvulkan.so.1" : soname);
		if (access(filepath.c_str(), R_OK) != 0) // try .so.1 first, fallback to .so
		{
			filepath = std::string(path) + std::string("/") + (soname.empty() ? "libvulkan.so" : soname);
		}
		ILOG("Found Vulkan library: %s", filepath.c_str());
	}

	return filepath;
}

void* find_extension_parent(void* sptr, VkStructureType sType)
{
	VkBaseOutStructure* ptr = (VkBaseOutStructure*)sptr;
	while (ptr != nullptr && (!ptr->pNext || ptr->pNext->sType != sType)) ptr = ptr->pNext;
	return ptr;
}

void* find_extension(void* sptr, VkStructureType sType)
{
	VkBaseOutStructure* ptr = (VkBaseOutStructure*)sptr;
	while (ptr != nullptr && ptr->sType != sType) ptr = ptr->pNext;
	return ptr;
}

void purge_extension_parent(void* sptr, VkStructureType sType)
{
	VkBaseOutStructure* ptr = (VkBaseOutStructure*)sptr;
	while (ptr != nullptr && ptr->pNext != nullptr)
	{
		if (ptr->pNext->sType == sType)
		{
			ptr->pNext = ptr->pNext->pNext;
		}
		ptr = ptr->pNext;
	}
}

const void* find_extension(const void* sptr, VkStructureType sType)
{
	const VkBaseOutStructure* ptr = (VkBaseOutStructure*)sptr;
	while (ptr != nullptr && ptr->sType != sType) ptr = ptr->pNext;
	return ptr;
}

bool shader_has_buffer_devices_addresses(const uint32_t* code, uint32_t code_size)
{
	uint16_t opcode;
	uint16_t word_count;
	const uint32_t* insn = code + 5;
	code_size /= 4; // bytes to words
	do {
		opcode = uint16_t(insn[0]);
		word_count = uint16_t(insn[0] >> 16);
		if (opcode == SpvOpExtension)
		{
			if (strcmp((char*)&insn[2], "KHR_physical_storage_buffer") == 0) return true;
			else if (strcmp((char*)&insn[2], "EXT_physical_storage_buffer") == 0) return true;
		}
		insn += word_count;
	}
	while (insn != code + code_size && opcode != SpvOpMemoryModel);
	return false;
}

const char* pretty_print_VkObjectType(VkObjectType val)
{
	switch (val)
	{
	case VK_OBJECT_TYPE_UNKNOWN: return "UNKNOWN";
	case VK_OBJECT_TYPE_INSTANCE: return "Instance";
	case VK_OBJECT_TYPE_PHYSICAL_DEVICE: return "PhysicalDevice";
	case VK_OBJECT_TYPE_DEVICE: return "Device";
	case VK_OBJECT_TYPE_QUEUE: return "Queue";
	case VK_OBJECT_TYPE_SEMAPHORE: return "Semaphore";
	case VK_OBJECT_TYPE_COMMAND_BUFFER: return "CommandBuffer";
	case VK_OBJECT_TYPE_FENCE: return "Fence";
	case VK_OBJECT_TYPE_DEVICE_MEMORY: return "Memory";
	case VK_OBJECT_TYPE_BUFFER: return "Buffer";
	case VK_OBJECT_TYPE_IMAGE: return "Image";
	case VK_OBJECT_TYPE_EVENT: return "Event";
	case VK_OBJECT_TYPE_QUERY_POOL: return "QueryPool";
	case VK_OBJECT_TYPE_BUFFER_VIEW: return "BufferView";
	case VK_OBJECT_TYPE_IMAGE_VIEW: return "ImageView";
	case VK_OBJECT_TYPE_SHADER_MODULE: return "ShaderModule";
	case VK_OBJECT_TYPE_PIPELINE_CACHE: return "PipelineCache";
	case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return "PipelineLayout";
	case VK_OBJECT_TYPE_RENDER_PASS: return "RenderPass";
	case VK_OBJECT_TYPE_PIPELINE: return "Pipeline";
	case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return "DescriptorSetLayout";
	case VK_OBJECT_TYPE_SAMPLER: return "Sampler";
	case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return "DescriptorPool";
	case VK_OBJECT_TYPE_DESCRIPTOR_SET: return "DescriptorSet";
	case VK_OBJECT_TYPE_FRAMEBUFFER: return "Framebuffer";
	case VK_OBJECT_TYPE_COMMAND_POOL: return "CommandPool";
	case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION: return "YcbcrConversion";
	case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return "DescriptorUpdateTemplate";
	case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT: return "PrivateDataSlot";
	case VK_OBJECT_TYPE_SURFACE_KHR: return "Surface";
	case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return "Swapchain";
	case VK_OBJECT_TYPE_DISPLAY_KHR: return "Display";
	case VK_OBJECT_TYPE_DISPLAY_MODE_KHR: return "DisplayMode";
	case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT: return "DebugReportCallback";
	case VK_OBJECT_TYPE_CU_FUNCTION_NVX: return "CuFunctionNVX";
	case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT: return "DebugUtilsMessenger";
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return "AccelerationStructure";
	case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT: return "ValidationCache";
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV: return "AccelerationStructureNV";
	case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL: return "PerformanceConfigurationINTEL";
	case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR: return "DeferredOperation";
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV: return "IndirectCommandsLayoutNV";
	case VK_OBJECT_TYPE_CU_MODULE_NVX: return "CuModuleNVX";
	case VK_OBJECT_TYPE_BUFFER_COLLECTION_FUCHSIA: return "BufferCollectionFUCHSIA";
	case VK_OBJECT_TYPE_MICROMAP_EXT: return "Micromap";
	case VK_OBJECT_TYPE_OPTICAL_FLOW_SESSION_NV: return "OpticalFlowSessionNV";
	case VK_OBJECT_TYPE_MAX_ENUM: assert(false); return "Error";
	case VK_OBJECT_TYPE_VIDEO_SESSION_KHR: return "VideoSession";
	case VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR: return "VideoSessionParameters";
	case VK_OBJECT_TYPE_SHADER_EXT: return "Shader";
	case VK_OBJECT_TYPE_CUDA_MODULE_NV: return "CudaModule";
	case VK_OBJECT_TYPE_CUDA_FUNCTION_NV: return "CudaFunction";
	case VK_OBJECT_TYPE_PIPELINE_BINARY_KHR: return "PipelineBinary";
	case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT: return "IndirectCommandsLayout";
	case VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT: return "IndirectExecutionSet";
	}
	return "Error";
}
