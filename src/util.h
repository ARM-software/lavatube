#pragma once

//
//  Header hacks
//

#include <stdarg.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/unistd.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <vector>
#include <string>

#define VK_NO_PROTOTYPES
#include "vulkan_utility.h"

#if defined(_MSC_VER)
	#define EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
	#define EXPORT __attribute__((visibility("default")))
#else
	#error No way to mark functions for export
#endif

// A way to add function parameters only to debug builds
#ifdef DEBUG
#define DEBUGPARAM(x) x
#else
#define DEBUGPARAM(x)
#endif

#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

static inline void clear_timespec(struct timespec *t) { t->tv_sec = 0; t->tv_nsec = 0; }

/// Return the difference between two timespec structs in microseconds
static inline uint64_t diff_timespec(const struct timespec *t1, const struct timespec *t0)
{
	return (t1->tv_sec - t0->tv_sec) * 1000000 + (t1->tv_nsec - t0->tv_nsec) / 1000;
}

/// Implement support for naming threads, missing from c++11
void set_thread_name(const char* name);
/// Get current thread name, if any. Input parameter must be char[16].
void get_thread_name(char* name);

static __attribute__((pure)) inline pid_t lava_gettid()
{
	return syscall(__NR_gettid);
}

extern uint_fast8_t p__virtualqueues;
extern uint_fast8_t p__blackhole;
extern uint_fast8_t p__dedicated_buffer;
extern uint_fast8_t p__dedicated_image;
extern uint_fast8_t p__gpu;
extern uint_fast8_t p__debug_level;
extern uint_fast8_t p__validation;
extern uint_fast8_t p__swapchains;
extern uint_fast8_t p__noscreen;
extern uint_fast8_t p__virtualswap;
extern uint_fast8_t p__virtualperfmode;
extern VkPresentModeKHR p__realpresentmode;
extern uint_fast8_t p__realimages;
extern const char* p__save_pipelinecache;
extern const char* p__load_pipelinecache;
extern uint_fast8_t p__dedicated_allocation;
extern uint_fast8_t p__custom_allocator;
extern uint_fast8_t p__no_anisotropy;
extern uint_fast8_t p__delay_fence_success_frames;
extern FILE* p__debug_destination;
extern int p__chunksize;
extern uint_fast8_t p__external_memory;
extern uint_fast8_t p__disable_multithread_writeout;
extern uint_fast8_t p__disable_multithread_compress;
extern uint_fast8_t p__disable_multithread_read;
extern uint_fast8_t p__allow_stalls;
extern uint_fast16_t p__preload;
extern uint_fast8_t p__compression_type;
extern uint_fast16_t p__compression_level;
extern uint_fast8_t p__sandbox_level;
extern uint_fast8_t p__trust_host_flushes;

/// Logging to be enable as needed by source recompilation
#define NEVER(_format, ...)

#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include <sstream>
#include <android/log.h>
#include "android_utils.h"

#ifndef NDEBUG
#define DLOG3(_format, ...) do { if (p__debug_level >= 3) { ((void)__android_log_print(ANDROID_LOG_DEBUG, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__)); } } while(0)
#define DLOG2(_format, ...) do { if (p__debug_level >= 2) { ((void)__android_log_print(ANDROID_LOG_DEBUG, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__)); } } while(0)
#define DLOG(_format, ...) do { if (p__debug_level >= 1) { ((void)__android_log_print(ANDROID_LOG_DEBUG, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__)); } } while(0)
#else
#define DLOG3(_format, ...)
#define DLOG2(_format, ...)
#define DLOG(_format, ...)
#endif

#define ILOG(_format, ...) ((void)__android_log_print(ANDROID_LOG_INFO, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__))
#define WLOG(_format, ...) ((void)__android_log_print(ANDROID_LOG_WARN, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__))
#define ELOG(_format, ...) ((void)__android_log_print(ANDROID_LOG_ERROR, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__))
#define FELOG(_format, ...) ((void)__android_log_print(ANDROID_LOG_FATAL, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__))
#define ABORT(_format, ...) do { ((void)__android_log_print(ANDROID_LOG_FATAL, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__)); fflush(p__debug_destination); abort(); } while(0)
#define DIE(_format, ...) do { ((void)__android_log_print(ANDROID_LOG_FATAL, "VULKAN_LAVATUBE", "%s:%d: " _format, __FILE__, __LINE__, ## __VA_ARGS__)); fflush(p__debug_destination); exit(-1); } while(0)

// Hack to workaround strange missing support for std::to_string in Android
template <typename T>
static inline std::string _to_string(T value)
{
    std::ostringstream os;
    os << value;
    return os.str();
}
int STOI(const std::string& value) __attribute__((pure));

#else // VK_USE_PLATFORM_ANDROID_KHR

#ifndef NDEBUG
/// Using DLOGn() instead of DLOG(n,...) so that we can conditionally compile without some of them
#define DLOG3(_format, ...) do { if (p__debug_level >= 3) { fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); } } while(0)
#define DLOG2(_format, ...) do { if (p__debug_level >= 2) { fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); } } while(0)
#define DLOG(_format, ...) do { if (p__debug_level >= 1) { fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); } } while(0)
#else
#define DLOG3(_format, ...)
#define DLOG2(_format, ...)
#define DLOG(_format, ...)
#endif
#define ILOG(_format, ...) fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__);
#define WLOG(_format, ...) fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__);
#define ELOG(_format, ...) fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__);
#define FELOG(_format, ...) fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__);
#define ABORT(_format, ...) do { fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); fflush(p__debug_destination); abort(); } while(0)
#define DIE(_format, ...) do { fprintf(p__debug_destination, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); fflush(p__debug_destination); exit(-1); } while(0)

#define _to_string(_x) std::to_string(_x)
#define STOI(_x) std::stoi(_x)

#endif // VK_USE_PLATFORM_ANDROID_KHR

// Another weird android issue...
#if defined(VK_USE_PLATFORM_ANDROID_KHR) && !defined(UINT32_MAX)
#define UINT32_MAX (4294967295U)
#endif

static inline std::string version_to_string(uint32_t v) { return _to_string(VK_VERSION_MAJOR(v)) + "." + _to_string(VK_VERSION_MINOR(v)) + "." + _to_string(VK_VERSION_PATCH(v)); }
const char* errorString(const VkResult errorCode);

void check_retval(VkResult stored_retval, VkResult retval);

static inline bool is_blackhole_mode() { return p__blackhole; }
static inline int selected_gpu() { return p__gpu; }
static inline bool is_debug() { return p__debug_level; }
static inline bool is_validation() { return p__validation; }
static inline unsigned num_swapchains() { return p__swapchains; } // tracing only
static inline bool is_noscreen() { return p__noscreen; } // replay only, for now
static inline bool is_virtualswapchain() { return p__virtualswap; } // replay only, for now
static inline const char* save_pipelinecache() { return p__save_pipelinecache; } // replay only, for now
static inline const char* load_pipelinecache() { return p__load_pipelinecache; } // replay only, for now
static inline bool use_dedicated_allocation() { return p__dedicated_allocation; } // replay only, for now
static inline bool use_custom_allocator() { return p__custom_allocator; } // replay only, for now
static inline bool no_anisotropy() { return p__no_anisotropy; } // replay only, for now

/// Consistent top header for any extension struct. Used to iterate them and handle the ones we recognize.
struct dummy_ext { VkStructureType sType; dummy_ext* pNext; };

const std::vector<std::string> split(const std::string& str, const char& delimiter);
const std::string join(const std::vector<std::string>& tokens, char joiner);

static __attribute__((pure)) inline uint32_t adler32(unsigned char *data, size_t len)
{
	const uint32_t MOD_ADLER = 65521;
	uint32_t a = 1, b = 0;
	for (size_t index = 0; index < len; ++index)
	{
		a = (a + data[index]) % MOD_ADLER;
		b = (b + a) % MOD_ADLER;
	}
	return (b << 16) | a;
}

std::string get_trace_path(const std::string& base);

static __attribute__((pure)) inline uint64_t gettime()
{
	struct timespec t;
	// CLOCK_MONOTONIC_COARSE is much more light-weight, but resolution is quite poor.
	// CLOCK_PROCESS_CPUTIME_ID is another possibility, it ignores rest of system, but costs more,
	// and also on some CPUs process migration between cores can screw up such measurements.
	// CLOCK_MONOTONIC is therefore a reasonable and portable compromise.
	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

std::string get_vulkan_lib_path();

/// Faster than std::vector but with much the same interface. The performance improvement mostly
/// comes from not filling memory beforehand.
struct buffer
{
	char* mPtr = nullptr;
	uint_fast32_t mSize = 0;

	inline const char* data() const noexcept { return mPtr; }
	inline char* data() noexcept { return mPtr; }
	inline uint_fast32_t size() const noexcept { return mSize; }
	inline void shrink(uint_fast32_t _size) noexcept { mSize = _size; }
	buffer() noexcept {}
	buffer(uint_fast32_t _size) noexcept { mPtr = (char*)malloc(_size); mSize = _size; }
	inline void release() noexcept { free(mPtr); mPtr = nullptr; mSize = 0; }
};

static __attribute__((const)) inline uint64_t aligned_size(uint64_t size, uint64_t alignment) { return size + alignment - 1ull - (size + alignment - 1ull) % alignment; }
static __attribute__((const)) inline uint64_t aligned_start(uint64_t size, uint64_t alignment) { return (size & ~(alignment - 1)); }

template<typename T>
inline T fake_handle(uint32_t index) { return (T)((intptr_t)index + 1); }

const char* pretty_print_VkObjectType(VkObjectType val);

enum lavatube_compression_type
{
	LAVATUBE_COMPRESSION_UNCOMPRESSED,
	LAVATUBE_COMPRESSION_DENSITY,
	LAVATUBE_COMPRESSION_LZ4,
	LAVATUBE_COMPRESSION_LZ4F, // lz4 with frame
};

// Hackish Vulkan extension-like function for testing lavatube internals (no longer hosted in tracetooltests)
#define VK_TRACETOOLTEST_OBJECT_PROPERTY_EXTENSION_NAME "VK_TRACETOOLTEST_object_property"
typedef enum VkTracingObjectPropertyTRACETOOLTEST {
	VK_TRACING_OBJECT_PROPERTY_UPDATES_COUNT_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_UPDATES_BYTES_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_BACKING_STORE_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_ADDRESS_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_MARKED_RANGES_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_MARKED_BYTES_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_MARKED_OBJECTS_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_SIZE_TRACETOOLTEST,
	VK_TRACING_OBJECT_PROPERTY_INDEX_TRACETOOLTEST,
} VkTracingObjectPropertyTRACETOOLTEST;
typedef uint64_t (VKAPI_PTR *PFN_vkGetDeviceTracingObjectPropertyTRACETOOLTEST)(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkTracingObjectPropertyTRACETOOLTEST valueType);

// Environment functions
int get_env_int(const char* name, int v);
int get_env_bool(const char* name, int v);
FILE* get_env_file(const char* name, FILE* fallback);
