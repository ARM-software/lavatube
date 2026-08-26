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
#include <chrono>
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
#include "replay_diagnostics.h"
#include "aftermath.h"
#include "replay_instrumentation.h"
#include "system_log.h"
#include "replay_entry.h"

// Default for this app
#define DEFAULT_SANDBOX_LEVEL 1

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
	if (strcmp(object_type, "VkFence") == 0 && index < VkFence_index.size())
	{
		const trackedfence& fence_data = VkFence_index.at(index);
		Json::Value replay;
		replay["handle_mapped"] = index_to_VkFence.contains(index);
		replay["flags"] = VkFenceCreateFlags_to_string(fence_data.flags);
		replay["pending_commandbuffer_count"] = (int)fence_data.replay_pending_commandbuffers.size();
		Json::Value pending(Json::arrayValue);
		for (uint32_t commandbuffer_index : fence_data.replay_pending_commandbuffers)
		{
			pending.append(commandbuffer_index);
		}
		replay["pending_commandbuffers"] = pending;
		out["replay"] = replay;
	}
	else if (strcmp(object_type, "VkPipeline") == 0 && index < VkPipeline_index.size())
	{
		trackedpipeline& pipeline_data = VkPipeline_index.at(index);
		// sanity checks
		if (!replayer.cli_pipeline_executable_stats_enabled.load(std::memory_order_acquire)) return true;
		if (!pipeline_data.is_state(trackable::states::created)) return true;
		if (pipeline_data.device_index == UINT32_MAX || pipeline_data.device_index >= VkDevice_index.size()) return true;
		if (!index_to_VkPipeline.contains(index)) return true;
		if (!index_to_VkDevice.contains(pipeline_data.device_index)) return true;
		// actual work
		VkDevice device = index_to_VkDevice.at(pipeline_data.device_index);
		VkPipeline pipeline = index_to_VkPipeline.at(index);
		(void)append_pipeline_executable_statistics_json(device, pipeline, out);
	}
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

static bool cli_thread_quiescent(lava_file_reader& reader)
{
	if (reader.terminated.load(std::memory_order_acquire)) return true;
	const cli_thread_state state = reader.cli_state.load(std::memory_order_acquire);
	return state == cli_thread_state::not_started
	       || state == cli_thread_state::cli_paused
	       || state == cli_thread_state::error_paused
	       || state == cli_thread_state::wait_handle
	       || state == cli_thread_state::wait_barrier
	       || state == cli_thread_state::wait_fence
	       || state == cli_thread_state::wait_queue_idle
	       || state == cli_thread_state::wait_device_idle
	       || state == cli_thread_state::terminated;
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
	if (!replayer.cli_idle_check.load(std::memory_order_acquire)) return "OK\n";
	return cli_wait_idle_devices();
}

static bool cli_send_error(int fd, const std::string& error)
{
	return lava_tcp_send_all(fd, "ERROR " + error + "\n");
}

static bool cli_send_memory(int fd, const void* data, VkDeviceSize size)
{
	const char* position = static_cast<const char*>(data);
	while (size > 0)
	{
		const size_t chunk = (size_t)std::min<VkDeviceSize>(size, 4 * 1024 * 1024);
		if (!lava_tcp_send_all(fd, position, chunk)) return false;
		position += chunk;
		size -= chunk;
	}
	return true;
}

static bool cli_send_buffer_stats(int fd, const char* path, uint64_t chunks, uint64_t replay_ns, uint64_t readback_ns, uint64_t send_ns)
{
	const std::string stats = "STATS path=" + std::string(path)
		+ " chunks=" + std::to_string(chunks)
		+ " replay_ns=" + std::to_string(replay_ns)
		+ " readback_ns=" + std::to_string(readback_ns)
		+ " send_ns=" + std::to_string(send_ns) + "\n";
	return lava_tcp_send_all(fd, stats);
}

struct cli_buffer_readback
{
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkBuffer source = VK_NULL_HANDLE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize allocation_size = 0;
	VkMemoryPropertyFlags memory_flags = 0;

	void destroy()
	{
		if (mapped) wrap_vkUnmapMemory(device, memory);
		if (fence != VK_NULL_HANDLE) wrap_vkDestroyFence(device, fence, nullptr);
		if (pool != VK_NULL_HANDLE) wrap_vkDestroyCommandPool(device, pool, nullptr);
		if (staging != VK_NULL_HANDLE) wrap_vkDestroyBuffer(device, staging, nullptr);
		if (memory != VK_NULL_HANDLE) wrap_vkFreeMemory(device, memory, nullptr);
		mapped = nullptr;
		fence = VK_NULL_HANDLE;
		pool = VK_NULL_HANDLE;
		staging = VK_NULL_HANDLE;
		memory = VK_NULL_HANDLE;
	}
};

static bool cli_readback_queue(const trackedbuffer& buffer_data, VkQueue& queue, uint32_t& queue_family, std::string& error)
{
	queue = VK_NULL_HANDLE;
	queue_family = UINT32_MAX;
	uint32_t exclusive_queue_family = UINT32_MAX;
	for (const trackedqueue& queue_data : VkQueue_index)
	{
		if (queue_data.device_index != buffer_data.parent_device_index || queue_data.realQueue == VK_NULL_HANDLE) continue;
		if (buffer_data.sharingMode == VK_SHARING_MODE_EXCLUSIVE)
		{
			// Without tracked ownership, staging is safe only when every queue used by this replay is in one family.
			if (exclusive_queue_family == UINT32_MAX) exclusive_queue_family = queue_data.queueFamily;
			else if (exclusive_queue_family != queue_data.queueFamily)
			{
				error = "exclusive buffer queue-family ownership is unknown";
				return false;
			}
		}
		if ((queue_data.queueFlags & VK_QUEUE_TRANSFER_BIT) == 0) continue;
		if (buffer_data.sharingMode == VK_SHARING_MODE_CONCURRENT)
		{
			if (std::find(buffer_data.queue_family_indices.begin(), buffer_data.queue_family_indices.end(), queue_data.queueFamily)
			    == buffer_data.queue_family_indices.end()) continue;
		}
		if (queue == VK_NULL_HANDLE)
		{
			queue = queue_data.realQueue;
			queue_family = queue_data.queueFamily;
		}
	}
	if (queue == VK_NULL_HANDLE)
	{
		error = "no compatible transfer queue is available";
		return false;
	}
	return true;
}

static bool cli_readback_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits, uint32_t& memory_type, VkMemoryPropertyFlags& flags)
{
	VkPhysicalDeviceMemoryProperties properties = {};
	wrap_vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	const VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		if ((type_bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & preferred) == preferred)
		{
			memory_type = i;
			flags = properties.memoryTypes[i].propertyFlags;
			return true;
		}
	}
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		if ((type_bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		{
			memory_type = i;
			flags = properties.memoryTypes[i].propertyFlags;
			return true;
		}
	}
	return false;
}

static bool cli_create_buffer_readback(const trackedbuffer& buffer_data, VkBuffer source, VkDeviceSize size,
	cli_buffer_readback& readback, std::string& error)
{
	trackeddevice& device_data = VkDevice_index.at(buffer_data.parent_device_index);
	readback.device = index_to_VkDevice.at(buffer_data.parent_device_index);
	readback.source = source;
	uint32_t queue_family = UINT32_MAX;
	if (!cli_readback_queue(buffer_data, readback.queue, queue_family, error)) return false;

	VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = wrap_vkCreateBuffer(readback.device, &buffer_info, nullptr, &readback.staging);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to create readback buffer: ") + errorString(result);
		return false;
	}

	VkMemoryRequirements requirements = {};
	wrap_vkGetBufferMemoryRequirements(readback.device, readback.staging, &requirements);
	uint32_t memory_type = UINT32_MAX;
	if (!cli_readback_memory_type(device_data.physicalDevice, requirements.memoryTypeBits, memory_type, readback.memory_flags))
	{
		error = "no host-visible staging memory type is available";
		return false;
	}
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = memory_type;
	result = wrap_vkAllocateMemory(readback.device, &allocation, nullptr, &readback.memory);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to allocate readback memory: ") + errorString(result);
		return false;
	}
	readback.allocation_size = requirements.size;
	result = wrap_vkBindBufferMemory(readback.device, readback.staging, readback.memory, 0);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to bind readback memory: ") + errorString(result);
		return false;
	}
	result = wrap_vkMapMemory(readback.device, readback.memory, 0, readback.allocation_size, 0, &readback.mapped);
	if (result != VK_SUCCESS || !readback.mapped)
	{
		error = std::string("failed to map readback memory: ") + errorString(result);
		return false;
	}

	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family;
	result = wrap_vkCreateCommandPool(readback.device, &pool_info, nullptr, &readback.pool);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to create readback command pool: ") + errorString(result);
		return false;
	}
	VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
	command_info.commandPool = readback.pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	result = wrap_vkAllocateCommandBuffers(readback.device, &command_info, &readback.command_buffer);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to allocate readback command buffer: ") + errorString(result);
		return false;
	}
	VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr };
	result = wrap_vkCreateFence(readback.device, &fence_info, nullptr, &readback.fence);
	if (result != VK_SUCCESS)
	{
		error = std::string("failed to create readback fence: ") + errorString(result);
		return false;
	}
	return true;
}

static VkResult cli_copy_buffer_readback(cli_buffer_readback& readback, VkDeviceSize source_offset, VkDeviceSize size)
{
	VkResult result = wrap_vkResetFences(readback.device, 1, &readback.fence);
	if (result != VK_SUCCESS) return result;
	result = wrap_vkResetCommandBuffer(readback.command_buffer, 0);
	if (result != VK_SUCCESS) return result;
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = wrap_vkBeginCommandBuffer(readback.command_buffer, &begin);
	if (result != VK_SUCCESS) return result;

	VkBufferMemoryBarrier source_barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr };
	source_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	source_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	source_barrier.buffer = readback.source;
	source_barrier.offset = source_offset;
	source_barrier.size = size;
	wrap_vkCmdPipelineBarrier(readback.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 1, &source_barrier, 0, nullptr);
	VkBufferCopy copy = { source_offset, 0, size };
	wrap_vkCmdCopyBuffer(readback.command_buffer, readback.source, readback.staging, 1, &copy);
	VkBufferMemoryBarrier staging_barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr };
	staging_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	staging_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	staging_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.buffer = readback.staging;
	staging_barrier.offset = 0;
	staging_barrier.size = size;
	wrap_vkCmdPipelineBarrier(readback.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 0, nullptr, 1, &staging_barrier, 0, nullptr);
	result = wrap_vkEndCommandBuffer(readback.command_buffer);
	if (result != VK_SUCCESS) return result;

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &readback.command_buffer;
	{
		lava::lock_guard lock(sync_mutex);
		result = wrap_vkQueueSubmit(readback.queue, 1, &submit, readback.fence);
	}
	if (result != VK_SUCCESS) return result;
	result = wrap_vkWaitForFences(readback.device, 1, &readback.fence, VK_TRUE, UINT64_MAX);
	if (result != VK_SUCCESS) return result;
	if ((readback.memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
	{
		VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr };
		range.memory = readback.memory;
		range.offset = 0;
		range.size = VK_WHOLE_SIZE;
		result = wrap_vkInvalidateMappedMemoryRanges(readback.device, 1, &range);
	}
	return result;
}

static bool cli_stream_buffer(int fd, uint32_t buffer_index)
{
	if (replayer.cli_running.load(std::memory_order_acquire)) return cli_send_error(fd, "replay is running");
	while (!replay_done.load(std::memory_order_acquire) && !cli_all_threads_quiescent()) usleep(50);
	const std::string idle = cli_wait_idle_devices();
	if (idle == "DEVICE_LOST\n") return lava_tcp_send_all(fd, idle);
	if (idle != "OK\n") return cli_send_error(fd, "failed to wait for the replay device");
	if (service_stop_requested.load(std::memory_order_acquire)) return cli_send_error(fd, "replay stopped");
	if (replayer.abort_requested()) return cli_send_error(fd, "replay device is unavailable");
	if (buffer_index >= VkBuffer_index.size()) return cli_send_error(fd, "invalid buffer index");
	trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
	if (!buffer_data.is_state(trackedobject::states::bound)) return cli_send_error(fd, "buffer is not bound and live");
	if (!index_to_VkBuffer.contains(buffer_index)) return cli_send_error(fd, "buffer has no replay handle");
	if ((buffer_data.flags & VK_BUFFER_CREATE_PROTECTED_BIT) || (buffer_data.memory_flags & VK_MEMORY_PROPERTY_PROTECTED_BIT))
	{
		return cli_send_error(fd, "protected buffers cannot be saved");
	}
	if (buffer_data.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) return cli_send_error(fd, "sparse buffers cannot be saved");
	if (buffer_data.parent_device_index >= VkDevice_index.size()) return cli_send_error(fd, "buffer device is unavailable");
	trackeddevice& device_data = VkDevice_index.at(buffer_data.parent_device_index);
	if (!device_data.allocator || !index_to_VkDevice.contains(buffer_data.parent_device_index)) return cli_send_error(fd, "buffer memory is unavailable");

	const suballoc_location location = device_data.allocator->inspect_buffer_memory(buffer_index);
	if (buffer_data.size > location.size) return cli_send_error(fd, "buffer size exceeds its replay allocation");
	const std::string header = "OK " + std::to_string((uint64_t)buffer_data.size) + "\n";
	if (location.mapped && (location.memory_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
	{
		const uint64_t replay_start = gettime();
		const uint64_t readback_start = replay_start;
		const VkResult result = device_data.allocator->invalidate_buffer_memory(buffer_index, buffer_data.size);
		if (result != VK_SUCCESS) return cli_send_error(fd, std::string("failed to invalidate buffer memory: ") + errorString(result));
		const uint64_t readback_ns = gettime() - readback_start;
		if (!lava_tcp_send_all(fd, header)) return false;
		const uint64_t send_start = gettime();
		const bool sent = cli_send_memory(fd, location.mapped, buffer_data.size);
		const uint64_t send_ns = gettime() - send_start;
		if (!sent) return false;
		const uint64_t replay_ns = gettime() - replay_start;
		const uint64_t chunks = (uint64_t)(buffer_data.size / (4 * 1024 * 1024))
			+ (buffer_data.size % (4 * 1024 * 1024) != 0 ? 1 : 0);
		return cli_send_buffer_stats(fd, "mapped", chunks, replay_ns, readback_ns, send_ns);
	}

	static constexpr VkDeviceSize maximum_staging_size = 4 * 1024 * 1024;
	const VkDeviceSize staging_size = std::min(buffer_data.size, maximum_staging_size);
	cli_buffer_readback readback;
	std::string error;
	if (!cli_create_buffer_readback(buffer_data, index_to_VkBuffer.at(buffer_index), staging_size, readback, error))
	{
		readback.destroy();
		return cli_send_error(fd, error);
	}
	const uint64_t replay_start = gettime();
	uint64_t readback_ns = 0;
	uint64_t send_ns = 0;
	uint64_t chunks = 0;
	VkDeviceSize offset = 0;
	VkDeviceSize chunk = std::min(staging_size, buffer_data.size - offset);
	uint64_t operation_start = gettime();
	VkResult result = cli_copy_buffer_readback(readback, offset, chunk);
	readback_ns += gettime() - operation_start;
	if (result != VK_SUCCESS)
	{
		readback.destroy();
		return cli_send_error(fd, std::string("failed to read back buffer: ") + errorString(result));
	}
	bool sent = lava_tcp_send_all(fd, header);
	while (sent && offset < buffer_data.size)
	{
		if (service_stop_requested.load(std::memory_order_acquire))
		{
			sent = false;
			break;
		}
		if (offset != 0)
		{
			chunk = std::min(staging_size, buffer_data.size - offset);
			operation_start = gettime();
			result = cli_copy_buffer_readback(readback, offset, chunk);
			readback_ns += gettime() - operation_start;
			if (result != VK_SUCCESS)
			{
				sent = false;
				break;
			}
		}
		operation_start = gettime();
		sent = lava_tcp_send_all(fd, readback.mapped, (size_t)chunk);
		send_ns += gettime() - operation_start;
		if (sent) chunks++;
		offset += chunk;
	}
	const uint64_t replay_ns = gettime() - replay_start;
	readback.destroy();
	return sent && cli_send_buffer_stats(fd, "staging", chunks, replay_ns, readback_ns, send_ns);
}

static bool cli_thread_ready();
static std::string cli_paused_command_response(lava_file_reader& reader);

static std::string cli_wait_for_done_and_idle()
{
	while (!replay_done.load(std::memory_order_acquire))
	{
		if (!replayer.cli_running.load(std::memory_order_acquire) && cli_thread_ready())
		{
			// The only way cli_running can go false during a full-speed continue is an error pause;
			// a selected thread finishing just means we keep waiting for the whole replay.
			const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
			lava_file_reader& reader = replayer.file_reader(thread_id);
			if (reader.cli_state.load(std::memory_order_acquire) == cli_thread_state::error_paused)
			{
				return cli_paused_command_response(reader);
			}
		}
		if (replayer.cli_running.load(std::memory_order_acquire))
		{
			replayer.cli_running.wait(true, std::memory_order_acquire);
		}
		else
		{
			replay_done.wait(false);
		}
	}
	if (replayer.abort_requested()) return "ABORTED " + replayer.abort_reason() + "\n";
	const std::string idle_response = replayer.cli_idle_check.load(std::memory_order_acquire)
		? cli_wait_idle_devices() : "OK\n";
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

static uint32_t cli_stable_completed_count(lava_file_reader& reader, cli_step_mode mode)
{
	const cli_thread_state state = reader.cli_state.load(std::memory_order_acquire);
	// cli_paused_call may describe an old pause after this thread was deselected. Count the
	// current packet or call only while the thread is actually stopped in check_cli().
	if (state == cli_thread_state::cli_paused || state == cli_thread_state::error_paused)
	{
		return cli_completed_count(reader, mode);
	}
	return mode == cli_step_mode::calls ? reader.api_call_count : reader.cli_packet.load(std::memory_order_relaxed);
}

static std::string cli_paused_command_response(lava_file_reader& reader)
{
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	if (!cli_has_paused_command(reader)) return "PAUSED thread=" + std::to_string(thread_id) + "\n";
	const char* packet_name = get_packet_name((packet_type)reader.current.packet_type, reader.current.call_id);
	std::string response = "PAUSED @ packet=" + std::to_string(cli_current_packet_index(reader)) + " api_calls=" + std::to_string(cli_current_call(reader))
	       + " name=" + packet_name + " frame="
	       + std::to_string(replayer.global_frame) + "/" + std::to_string(replayer.global_frame_count)
	       + " thread=" + std::to_string(thread_id);
	const int_fast32_t result = reader.cli_paused_error.load(std::memory_order_acquire);
	if (result != 0) response += " result=" + std::string(errorString((VkResult)result));
	return response + "\n";
}

/// Response for the currently selected thread. Re-resolves the selection because an error pause
/// may have retargeted it while a control command was in flight.
static std::string cli_selected_pause_response()
{
	if (!cli_thread_ready()) return "PAUSED\n";
	const int thread_id = replayer.cli_thread.load(std::memory_order_acquire);
	return cli_paused_command_response(replayer.file_reader(thread_id));
}

static std::string cli_blocked_command_response(lava_file_reader& reader, cli_thread_state state)
{
	const char* reason = "handle";
	if (state == cli_thread_state::wait_barrier) reason = "barrier";
	else if (state == cli_thread_state::wait_fence) reason = "fence";
	else if (state == cli_thread_state::wait_queue_idle) reason = "queue_idle";
	else if (state == cli_thread_state::wait_device_idle) reason = "device_idle";
	std::string response = "BLOCKED thread=" + std::to_string(reader.current.thread)
	       + " reason=" + reason
	       + " dependency_thread=" + std::to_string(reader.cli_wait_thread.load(std::memory_order_relaxed))
	       + " dependency_packet=" + std::to_string(reader.cli_wait_packet.load(std::memory_order_relaxed));
	if (state == cli_thread_state::wait_fence || state == cli_thread_state::wait_queue_idle
	    || state == cli_thread_state::wait_device_idle)
	{
		response += " object_type=" + std::to_string(reader.cli_wait_object_type.load(std::memory_order_relaxed))
		            + " object_index=" + std::to_string(reader.cli_wait_object_index.load(std::memory_order_relaxed))
		            + " auxiliary_index=" + std::to_string(reader.cli_wait_aux_index.load(std::memory_order_relaxed));
	}
	return response + "\n";
}

static std::string cli_wait_for_pause_or_block(lava_file_reader& reader)
{
	cli_thread_state gpu_wait_state = cli_thread_state::running;
	std::chrono::steady_clock::time_point gpu_wait_start;
	while (replayer.cli_running.load(std::memory_order_acquire))
	{
		const cli_thread_state state = reader.cli_state.load(std::memory_order_acquire);
		if (replayer.cli_isolate_thread.load(std::memory_order_acquire))
		{
			if (state == cli_thread_state::wait_barrier || state == cli_thread_state::wait_handle)
			{
				replayer.cli_running.store(false, std::memory_order_release);
				replayer.cli_running.notify_all();
				return cli_blocked_command_response(reader, state);
			}
			const bool gpu_wait = state == cli_thread_state::wait_fence
			                      || state == cli_thread_state::wait_queue_idle
			                      || state == cli_thread_state::wait_device_idle;
			if (gpu_wait && state != gpu_wait_state)
			{
				gpu_wait_state = state;
				gpu_wait_start = std::chrono::steady_clock::now();
			}
			else if (gpu_wait && std::chrono::steady_clock::now() - gpu_wait_start >= std::chrono::milliseconds(10))
			{
				replayer.cli_running.store(false, std::memory_order_release);
				replayer.cli_running.notify_all();
				return cli_blocked_command_response(reader, state);
			}
			else if (!gpu_wait)
			{
				gpu_wait_state = cli_thread_state::running;
			}
		}
		usleep(50);
	}
	if (replayer.abort_requested()) return "ABORTED " + replayer.abort_reason() + "\n";
	if (reader.terminated.load(std::memory_order_acquire))
	{
		return "THREAD_DONE thread=" + std::to_string(reader.current.thread) + "\n";
	}
	return std::string();
}

static std::string cli_prepare_thread(uint32_t thread_id, bool& selection_changed)
{
	selection_changed = false;
	if (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire)) return "ERROR\n";
	if (thread_id >= replayer.threads.size()) return "ERROR\n";
	lava_file_reader& reader = replayer.file_reader(thread_id);
	if (reader.terminated.load(std::memory_order_acquire)) return "ERROR\n";

	const int current_thread = replayer.cli_thread.load(std::memory_order_acquire);
	if (current_thread == (int)thread_id)
	{
		const std::string idle_response = cli_wait_for_quiescence_and_idle();
		if (replayer.abort_requested())
		{
			return "ABORTED " + replayer.abort_reason() + "\n";
		}
		return idle_response == "OK\n" ? std::string() : idle_response;
	}

	selection_changed = true;
	const uint32_t current_packet = reader.cli_packet.load(std::memory_order_relaxed);
	if (current_packet == UINT32_MAX) return "ERROR\n";
	reader.cli_paused_call.store(0, std::memory_order_release);
	cli_clear_function_target(reader);
	reader.cli_step.store(cli_step_mode::packets, std::memory_order_release);
	reader.cli_call.store(current_packet + 1, std::memory_order_release);
	replayer.cli_thread.store((int)thread_id, std::memory_order_release);
	replayer.cli_running.store(true, std::memory_order_release);
	replayer.cli_running.notify_all();

	const std::string blocked_response = cli_wait_for_pause_or_block(reader);

	if (replayer.abort_requested()) return "ABORTED " + replayer.abort_reason() + "\n";
	// An unexpected result on another thread may have taken over the global selection while
	// this thread was being prepared. Preserve that error pause instead of starting the
	// caller's second operation with a different thread selected.
	if (replayer.cli_thread.load(std::memory_order_acquire) != (int)thread_id)
	{
		return cli_selected_pause_response();
	}
	if (!blocked_response.empty()) return blocked_response;

	if (replay_done.load(std::memory_order_acquire)) return "DONE\n";

	const std::string idle_response = cli_wait_for_quiescence_and_idle();
	if (idle_response != "OK\n") return idle_response;
	return std::string();
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
	CALLBACK(vkCmdBuildAccelerationStructuresKHR);
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
#ifdef VK_USE_PLATFORM_ANDROID_KHR
	printf("-w/--wsi wsi           Use the given windowing system [android, headless, none]\n");
#else
	printf("-w/--wsi wsi           Use the given windowing system [xcb, wayland, headless, none]\n");
#endif
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
	printf("-s/--sandbox level     Set security sandbox level (from 1 to 3, with 3 the most strict, default %d)\n", (int)DEFAULT_SANDBOX_LEVEL);
	printf("--skip-remove-unused   Do not attempt to cleverly remove unused features and extensions\n");
	printf("--device-fault-report  Track more data for device fault diagnosis\n");
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

static bool cli_command_is_observer(const std::vector<std::string>& command)
{
	if (command.size() == 1 && command[0] == "status") return true;
	if (command.size() == 2 && command[0] == "log" && command[1] == "session") return true;
	if (command.size() == 2 && command[0] == "syslog" && command[1] == "session") return true;
	if (command.size() == 2 && command[0] == "diagnose" && command[1] == "deadlock") return true;
	if (command.size() == 2 && command[0] == "info" && command[1] == "trace") return true;
	if (command.size() == 2 && command[0] == "info" && command[1] == "threads") return true;
	if (command.size() == 2 && command[0] == "info" && command[1] == "objects") return true;
	if (command.size() == 3 && command[0] == "info" && command[1] == "thread") return true;
	if (command.size() == 4 && command[0] == "info" && command[1] == "frame") return true;
	return false;
}

static bool cli_command_is_interrupt(const std::vector<std::string>& command)
{
	return command.size() == 1 && command[0] == "stop";
}

struct service_log_stream_state
{
	std::atomic_bool update_active{ false };
	FILE* file = nullptr;
	off_t cursor = 0;
	off_t snapshot_end = 0;
};

struct service_client_state
{
	std::atomic_uint active_clients{ 0 };
	std::string log_session;
	service_log_stream_state replay_log;
	service_log_stream_state system_log;
	system_log_collector system_collector;
};

static std::string service_log_session_id()
{
	const uint64_t nanoseconds = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::ostringstream id;
	id << std::hex << (uint64_t)getpid() << "-" << nanoseconds;
	return id.str();
}

static std::string service_log_update_response(service_client_state* state, service_log_stream_state* stream, const char* description, off_t& cursor_commit)
{
	cursor_commit = -1;
	if (!stream->file) return std::string("ERROR ") + description + " is unavailable\n";
	flockfile(stream->file);
	if (fflush(stream->file) != 0)
	{
		funlockfile(stream->file);
		return std::string("ERROR failed to flush ") + description + "\n";
	}

	const off_t file_end = ftello(stream->file);
	if (file_end < 0 || file_end < stream->cursor)
	{
		funlockfile(stream->file);
		return std::string("ERROR failed to inspect ") + description + "\n";
	}
	if (stream->cursor == stream->snapshot_end)
	{
		stream->snapshot_end = file_end;
	}
	if (stream->snapshot_end < stream->cursor || stream->snapshot_end > file_end)
	{
		funlockfile(stream->file);
		return std::string("ERROR invalid ") + description + " cursor\n";
	}

	const off_t maximum_end = stream->cursor + 512 * 1024;
	const off_t chunk_end = std::min(maximum_end, stream->snapshot_end);
	std::string payload((size_t)(chunk_end - stream->cursor), '\0');
	size_t received = 0;
	while (received < payload.size())
	{
		const ssize_t result = pread(fileno(stream->file), payload.data() + received, payload.size() - received, stream->cursor + (off_t)received);
		if (result < 0)
		{
			if (errno == EINTR) continue;
			funlockfile(stream->file);
			return std::string("ERROR failed to read ") + description + "\n";
		}
		if (result == 0)
		{
			funlockfile(stream->file);
			return std::string("ERROR incomplete ") + description + " read\n";
		}
		received += (size_t)result;
	}
	funlockfile(stream->file);

	std::ostringstream header;
	header << "OK " << state->log_session << " " << stream->cursor << " " << chunk_end << " " << stream->snapshot_end << "\n";
	cursor_commit = chunk_end;
	return header.str() + payload;
}

static bool service_system_log_available(service_client_state* state, std::string& prefix, std::string& error)
{
	std::string warning;
	if (!state->system_collector.available(warning, error)) return false;
	if (!warning.empty()) prefix = "WARNING " + warning + "\n";
	return true;
}

static void cli_cancel_pending_requests()
{
	replayer.cli_marker_requested.store(cli_marker_placement::none, std::memory_order_release);
	replayer.cli_marker_ready.store(true, std::memory_order_release);
	replayer.cli_marker_ready.notify_all();
	replayer.cli_instrument_requested.store(cli_instrument_mode::none, std::memory_order_release);
	replayer.cli_instrument_ready.store(true, std::memory_order_release);
	replayer.cli_instrument_ready.notify_all();
	replayer.cli_params_requested.store(false, std::memory_order_release);
	replayer.cli_params_ready.store(true, std::memory_order_release);
	replayer.cli_params_ready.notify_all();
}

void lava_replay_request_stop()
{
	service_stop_requested.store(true, std::memory_order_release);
	cli_cancel_pending_requests();
	replayer.cli_running.store(true, std::memory_order_release);
	replayer.cli_running.notify_all();
	replayer.request_stop();
	done_var.store(true, std::memory_order_release);
	done_var.notify_all();
}

static std::string service_command_response(service_client_state* state, const std::vector<std::string>& command, off_t& log_cursor_commit, off_t& system_log_cursor_commit)
{
	std::string response;
	if (command.empty())
	{
		response = "ERROR\n";
	}
	else if (command.size() == 1 && command[0] == "status")
	{
		if (replayer.abort_requested()) response = "ABORTED " + replayer.abort_reason() + "\n";
		else if (replay_done.load(std::memory_order_acquire)) response = "DONE\n";
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
		for (unsigned i = 0; i < replayer.threads.size(); i++)
		{
			lava_file_reader& reader = replayer.file_reader(i);
			cli_clear_function_target(reader);
			reader.cli_call.store(UINT32_MAX, std::memory_order_release);
		}
		replayer.cli_isolate_thread.store(false, std::memory_order_release);
		replayer.cli_running.store(true, std::memory_order_release);
		replayer.cli_running.notify_all();
		response = cli_wait_for_done_and_idle();
	}
	else if (command.size() == 1 && command[0] == "stop")
	{
		lava_replay_request_stop();
		response = "OK\n";
	}
	else if (command.size() == 2 && command[0] == "log" && command[1] == "update")
	{
		response = service_log_update_response(state, &state->replay_log, "replay log", log_cursor_commit);
	}
	else if (command.size() == 2 && command[0] == "log" && command[1] == "session")
	{
		response = "OK " + state->log_session + "\n";
	}
	else if (command.size() >= 2 && command[0] == "log" && command[1] == "tail")
	{
		response = "ERROR log tail is handled by lava-cli\n";
	}
	else if (command.size() == 2 && command[0] == "syslog" && command[1] == "update")
	{
		std::string prefix;
		std::string error;
		if (!service_system_log_available(state, prefix, error)) response = "ERROR system log unavailable: " + error + "\n";
		else response = prefix + service_log_update_response(state, &state->system_log, "system log", system_log_cursor_commit);
	}
	else if (command.size() == 2 && command[0] == "syslog" && command[1] == "session")
	{
		std::string prefix;
		std::string error;
		if (!service_system_log_available(state, prefix, error)) response = "ERROR system log unavailable: " + error + "\n";
		else response = prefix + "OK " + state->log_session + "\n";
	}
	else if (command.size() >= 2 && command[0] == "syslog" && command[1] == "tail")
	{
		response = "ERROR syslog tail is handled by lava-cli\n";
	}
	else if (command.size() == 2 && command[0] == "diagnose" && command[1] == "deadlock")
	{
		response = replay_diagnostics_deadlock_response(replayer);
	}
	else if (command.size() == 2 && command[0] == "diagnose" && command[1] == "device")
	{
		if (replayer.cli_running.load(std::memory_order_acquire)) response = "ERROR replay is running\n";
		else response = cli_wait_idle_devices();
	}
	else if (command.size() == 1 && command[0] == "self-test")
	{
		if (replayer.cli_running.load(std::memory_order_acquire)) response = "ERROR replay is running\n";
		else if (!cli_thread_ready()) response = "ERROR replay is not initialized\n";
		else
		{
			response = cli_wait_for_quiescence_and_idle();
			if (response == "OK\n") replayer.self_test();
		}
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
	else if (command.size() == 3 && command[0] == "set" && command[1] == "idle-check")
	{
		bool enabled = false;
		if (parse_bool(command[2], enabled))
		{
			replayer.cli_idle_check.store(enabled, std::memory_order_release);
			response = "OK\n";
		}
		else
		{
			response = "ERROR\n";
		}
	}
	else if (command.size() == 3 && command[0] == "set" && command[1] == "isolate-thread")
	{
		bool enabled = false;
		if (parse_bool(command[2], enabled))
		{
			replayer.cli_isolate_thread.store(enabled, std::memory_order_release);
			response = "OK\n";
		}
		else
		{
			response = "ERROR\n";
		}
	}
	else if (command[0] == "instrument")
	{
		uint32_t thread_id = 0;
		cli_instrument_mode mode = cli_instrument_mode::whole;
		if (command.size() == 3 && command[2] == "detailed") mode = cli_instrument_mode::detailed;
		else if (command.size() != 2)
		{
			response = "ERROR expected 'instrument THREAD' or 'instrument THREAD detailed'\n";
		}
		if (response.empty() && !parse_u32(command[1], thread_id)) response = "ERROR invalid thread index\n";
		bool selection_changed = false;
		if (response.empty()) response = cli_prepare_thread(thread_id, selection_changed);
		if (response.empty() && selection_changed
		    && replayer.file_reader(thread_id).cli_state.load(std::memory_order_acquire) == cli_thread_state::error_paused)
		{
			response = cli_paused_command_response(replayer.file_reader(thread_id));
		}
		if (response.empty() && replayer.cli_running.load(std::memory_order_acquire))
		{
			response = "ERROR replay is not paused on a selected thread\n";
		}
		if (response.empty())
		{
			lava_file_reader& reader = replayer.file_reader(thread_id);
			if (!cli_has_paused_command(reader) || reader.current.packet_type != PACKET_VULKAN_API_CALL || reader.current.call_id != VKBEGINCOMMANDBUFFER)
			{
				response = "ERROR instrument requires a pause on vkBeginCommandBuffer\n";
			}
			else
			{
				const std::string idle_response = cli_wait_for_quiescence_and_idle();
				if (idle_response != "OK\n") response = idle_response;
				else
				{
					replayer.cli_response.clear();
					replayer.cli_instrument_ready.store(false, std::memory_order_release);
					if (service_stop_requested.load(std::memory_order_acquire))
					{
						response = "ERROR replay stopped\n";
					}
					else
					{
						replayer.cli_instrument_requested.store(mode, std::memory_order_release);
						if (service_stop_requested.load(std::memory_order_acquire))
						{
							replayer.cli_instrument_requested.store(cli_instrument_mode::none, std::memory_order_release);
							response = "ERROR replay stopped\n";
						}
						else
						{
							replayer.cli_instrument_ready.wait(false);
							if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
							else response = replayer.cli_response.empty() ? "ERROR instrumentation request failed\n" : replayer.cli_response;
						}
					}
				}
			}
		}
	}
	else if (command[0] == "add-markers")
	{
		uint32_t thread_id = 0;
		cli_marker_placement placement = cli_marker_placement::both;
		bool valid = command.size() >= 5 && parse_u32(command[1], thread_id) && command[2] == "nvidia";
		bool has_call = false;
		for (size_t i = 3; valid && i < command.size(); i++)
		{
			if (command[i] == "--call" && i + 1 < command.size())
			{
				has_call = command[++i] == "vkCmdBuildAccelerationStructuresKHR";
				valid = has_call;
			}
			else if (command[i] == "--placement" && i + 1 < command.size())
			{
				const std::string& value = command[++i];
				if (value == "before") placement = cli_marker_placement::before;
				else if (value == "after") placement = cli_marker_placement::after;
				else if (value == "both") placement = cli_marker_placement::both;
				else valid = false;
			}
			else valid = false;
		}
		if (!valid || !has_call)
		{
			response = "ERROR expected 'add-markers THREAD nvidia --call vkCmdBuildAccelerationStructuresKHR [--placement before|after|both]'\n";
		}
		else if (thread_id >= replayer.threads.size())
		{
			response = "ERROR invalid thread index\n";
		}
		else
		{
			bool selection_changed = false;
			response = cli_prepare_thread(thread_id, selection_changed);
			lava_file_reader& reader = replayer.file_reader(thread_id);
			if (response.empty() && selection_changed
			    && reader.cli_state.load(std::memory_order_acquire) == cli_thread_state::error_paused)
			{
				response = cli_paused_command_response(reader);
			}
			if (response.empty() && replayer.cli_running.load(std::memory_order_acquire))
			{
				response = "ERROR replay is not paused on the targeted thread\n";
			}
			if (response.empty() && (!cli_has_paused_command(reader) || reader.current.packet_type != PACKET_VULKAN_API_CALL
			    || reader.current.call_id != VKBEGINCOMMANDBUFFER))
			{
				response = "ERROR add-markers requires a pause on vkBeginCommandBuffer\n";
			}
			else if (response.empty())
			{
				const std::string idle_response = cli_wait_for_quiescence_and_idle();
				if (idle_response != "OK\n") response = idle_response;
				else
				{
					replayer.cli_response.clear();
					replayer.cli_marker_ready.store(false, std::memory_order_release);
					replayer.cli_marker_requested.store(placement, std::memory_order_release);
					if (service_stop_requested.load(std::memory_order_acquire))
					{
						replayer.cli_marker_requested.store(cli_marker_placement::none, std::memory_order_release);
						response = "ERROR replay stopped\n";
					}
					else
					{
						replayer.cli_marker_ready.wait(false);
						if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
						else response = replayer.cli_response.empty() ? "ERROR marker request failed\n" : replayer.cli_response;
					}
				}
			}
		}
	}
	else if (command[0] == "step")
	{
		uint32_t thread_id = 0;
		uint32_t count = 1;
		cli_step_mode step_mode = cli_step_mode::packets;
		if (command.size() == 4 && (command[2] == "calls" || command[2] == "packets"))
		{
			step_mode = command[2] == "calls" ? cli_step_mode::calls : cli_step_mode::packets;
			if (!parse_positive_u32(command[3], count))
			{
				response = "ERROR\n";
			}
		}
		else if (command.size() != 2)
		{
			response = "ERROR\n";
		}
		if (response.empty() && !parse_u32(command[1], thread_id)) response = "ERROR\n";
		if (response.empty())
		{
			if (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire)
			    || thread_id >= replayer.threads.size())
			{
				response = "ERROR\n";
			}
			else
			{
				lava_file_reader& reader = replayer.file_reader(thread_id);
				if (reader.terminated.load(std::memory_order_acquire))
				{
					response = "ERROR\n";
				}
				else
				{
					bool selection_changed = false;
					response = cli_prepare_thread(thread_id, selection_changed);
					if (response.empty() && selection_changed
					    && reader.cli_state.load(std::memory_order_acquire) == cli_thread_state::error_paused)
					{
						response = cli_paused_command_response(reader);
					}
					if (response.empty() && replayer.abort_requested())
					{
						response = "ABORTED " + replayer.abort_reason() + "\n";
					}
					else if (response.empty())
					{
						const uint32_t base_count = cli_stable_completed_count(reader, step_mode);
						if (count > UINT32_MAX - base_count)
						{
							response = "ERROR\n";
						}
						else
						{
							if (selection_changed) reader.cli_paused_call.store(0, std::memory_order_release);
							cli_clear_function_target(reader);
							reader.cli_step.store(step_mode, std::memory_order_release);
							reader.cli_call.store(base_count + count, std::memory_order_release);
							replayer.cli_thread.store((int)thread_id, std::memory_order_release);
							replayer.cli_running.store(true, std::memory_order_release);
							replayer.cli_running.notify_all();
							response = cli_wait_for_pause_or_block(reader);
							if (response.empty())
							{
								const std::string pause_idle_response = cli_wait_for_quiescence_and_idle();
								if (replayer.abort_requested()) response = "ABORTED " + replayer.abort_reason() + "\n";
								else response = pause_idle_response == "OK\n" ? cli_selected_pause_response() : pause_idle_response;
							}
						}
					}
				}
			}
		}
	}
	else if (command[0] == "goto")
	{
		uint32_t thread_id = 0;
		uint32_t target_packet = 0;
		uint16_t function_id = UINT16_MAX;
		if (command.size() != 3 || !parse_u32(command[1], thread_id) || thread_id >= replayer.threads.size())
		{
			response = "ERROR\n";
		}
		else
		{
			const bool packet_target = parse_u32(command[2], target_packet);
			if (packet_target && target_packet == UINT32_MAX)
			{
				response = "ERROR\n";
			}
			else if (!packet_target)
			{
				function_id = retrace_getid(command[2].c_str());
				if (function_id == UINT16_MAX) response = "ERROR\n";
			}
			bool selection_changed = false;
			if (response.empty()) response = cli_prepare_thread(thread_id, selection_changed);
			lava_file_reader& reader = replayer.file_reader(thread_id);
			if (response.empty() && selection_changed
			    && reader.cli_state.load(std::memory_order_acquire) == cli_thread_state::error_paused)
			{
				response = cli_paused_command_response(reader);
			}
			if (response.empty() && (replay_done.load(std::memory_order_acquire) || replayer.cli_running.load(std::memory_order_acquire)))
			{
				response = "ERROR\n";
			}
			else if (response.empty() && packet_target)
			{
				const uint32_t current_packet = cli_current_packet_index(reader);
				if (target_packet < current_packet)
				{
					response = "ERROR\n";
				}
				else if (target_packet == current_packet && cli_has_paused_command(reader))
				{
					const std::string idle_response = cli_wait_for_quiescence_and_idle();
					if (replayer.abort_requested()) response = "ABORTED " + replayer.abort_reason() + "\n";
					else response = idle_response == "OK\n" ? cli_selected_pause_response() : idle_response;
				}
				else
				{
					cli_clear_function_target(reader);
					reader.cli_step.store(cli_step_mode::packets, std::memory_order_release);
					reader.cli_call.store(target_packet + 1, std::memory_order_release);
					replayer.cli_running.store(true, std::memory_order_release);
					replayer.cli_running.notify_all();
					response = cli_wait_for_pause_or_block(reader);
					if (response.empty())
					{
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						if (idle_response != "OK\n") response = idle_response;
						else if (replayer.abort_requested()) response = "ABORTED " + replayer.abort_reason() + "\n";
						else response = replay_done.load(std::memory_order_acquire) ? "DONE\n" : cli_selected_pause_response();
					}
				}
			}
			else if (response.empty())
			{
				if (selection_changed && cli_has_paused_command(reader)
				    && reader.current.packet_type == PACKET_VULKAN_API_CALL && reader.current.call_id == function_id)
				{
					response = cli_selected_pause_response();
				}
				else
				{
					reader.cli_function.store(function_id, std::memory_order_release);
					reader.cli_call.store(cli_current_call(reader) + 1, std::memory_order_release);
					reader.cli_step.store(cli_step_mode::function, std::memory_order_release);
					replayer.cli_running.store(true, std::memory_order_release);
					replayer.cli_running.notify_all();
					response = cli_wait_for_pause_or_block(reader);
					if (response.empty())
					{
						const std::string idle_response = cli_wait_for_quiescence_and_idle();
						if (idle_response != "OK\n") response = idle_response;
						else if (replayer.abort_requested()) response = "ABORTED " + replayer.abort_reason() + "\n";
						else response = replay_done.load(std::memory_order_acquire) ? "DONE\n" : cli_selected_pause_response();
					}
				}
			}
		}
	}
	else if (command.size() == 2 && (command[0] == "params" || command[0] == "parameters")) // show parameters
	{
		uint32_t thread_id = 0;
		if (!parse_u32(command[1], thread_id) || thread_id >= replayer.threads.size())
		{
			response = "ERROR\n";
		}
		else
		{
			bool selection_changed = false;
			response = cli_prepare_thread(thread_id, selection_changed);
			lava_file_reader& reader = replayer.file_reader(thread_id);
			if (response.empty() && !cli_has_paused_command(reader))
			{
				response = "ERROR\n";
			}
			else if (response.empty() && reader.terminated.load(std::memory_order_acquire))
			{
				// A dead thread (abort or THREAD_DONE) will never publish parameters
				response = replayer.abort_requested() ? "ABORTED " + replayer.abort_reason() + "\n" : "ERROR\n";
			}
			else if (response.empty())
			{
				replayer.cli_response.clear();
				replayer.cli_params_ready.store(false, std::memory_order_release);
				if (service_stop_requested.load(std::memory_order_acquire))
				{
					response = "ERROR replay stopped\n";
				}
				else
				{
					replayer.cli_params_requested.store(true, std::memory_order_release);
					if (service_stop_requested.load(std::memory_order_acquire))
					{
						replayer.cli_params_requested.store(false, std::memory_order_release);
						response = "ERROR replay stopped\n";
					}
					else
					{
						replayer.cli_params_ready.wait(false);
						if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
						else response = replayer.cli_response.empty() ? "ERROR\n" : replayer.cli_response;
					}
				}
			}
		}
	}
	else if (command.size() == 3 && command[0] == "show" && command[1] == "instrumentation")
	{
		uint32_t index = 0;
		if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
		else if (replayer.cli_running.load(std::memory_order_acquire)) response = "ERROR replay is running\n";
		else if (!parse_u32(command[2], index)) response = "ERROR invalid command buffer index\n";
		else
		{
			// These diagnostics are cached host data. Once the replay has aborted because the
			// device was lost, do not issue another Vulkan idle wait before inspecting them.
			const std::string idle_response = replayer.abort_requested() ? "OK\n" : cli_wait_for_quiescence_and_idle();
			if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
			else response = idle_response == "OK\n" ? replay_instrumentation_show(index) : idle_response;
		}
	}
	else if (command.size() == 3 && command[0] == "show" && command[1] == "as-build")
	{
		uint32_t index = 0;
		if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
		else if (replayer.cli_running.load(std::memory_order_acquire)) response = "ERROR replay is running\n";
		else if (!parse_u32(command[2], index)) response = "ERROR invalid command buffer index\n";
		else
		{
			// AS build snapshots are cached host data and remain valid after device loss.
			const std::string idle_response = replayer.abort_requested() ? "OK\n" : cli_wait_for_quiescence_and_idle();
			if (service_stop_requested.load(std::memory_order_acquire)) response = "ERROR replay stopped\n";
			else response = idle_response == "OK\n" ? replay_as_build_show(index) : idle_response;
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
	else if (command.size() == 2 && command[0] == "info" && command[1] == "trace")
	{
		Json::Value value;
		value["filename"] = replayer.packed_file();
		value["file_size"] = (Json::UInt64)replayer.trace_file_size();
		if (replayer.trace_file_creation_timestamp().empty()) value["creation_timestamp"] = Json::nullValue;
		else value["creation_timestamp"] = replayer.trace_file_creation_timestamp();
		response = trace_metadata_json_compact(value) + "\n";
	}
	else if (command.size() == 2 && command[0] == "info" && command[1] == "threads") // list thread info
	{
		response = replay_diagnostics_threads_response(replayer);
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
	const bool bypass_active = cli_command_is_observer(command) || cli_command_is_interrupt(command);
	const bool log_update = command.size() == 2 && command[0] == "log" && command[1] == "update";
	const bool system_log_update = command.size() == 2 && command[0] == "syslog" && command[1] == "update";
	const bool save_buffer = command.size() == 3 && command[0] == "save" && command[1] == "buffer";
	bool command_active = false;
	bool log_update_active = false;
	bool system_log_update_active = false;
	off_t log_cursor_commit = -1;
	off_t system_log_cursor_commit = -1;

	if (log_update)
	{
		bool expected = false;
		log_update_active = state->replay_log.update_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
		if (!log_update_active)
		{
			response = "ERROR another log update is active\n";
		}
	}
	if (system_log_update)
	{
		bool expected = false;
		system_log_update_active = state->system_log.update_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
		if (!system_log_update_active)
		{
			response = "ERROR another syslog update is active\n";
		}
	}

	if (response.empty() && !bypass_active)
	{
		bool expected = false;
		while (!replayer.cli_command_active.compare_exchange_weak(expected, true, std::memory_order_acq_rel))
		{
			replayer.cli_command_active.wait(true, std::memory_order_acquire);
			expected = false;
		}
		command_active = true;
	}

	if (response.empty())
	{
		if (save_buffer)
		{
			uint32_t buffer_index = 0;
			if (!parse_u32(command[2], buffer_index)) response = "ERROR invalid buffer index\n";
			else if (!cli_stream_buffer(client_fd, buffer_index)) ELOG("Failed to stream replay buffer %u", buffer_index);
		}
		else response = service_command_response(state, command, log_cursor_commit, system_log_cursor_commit);
	}

	if (command_active)
	{
		replayer.cli_command_active.store(false, std::memory_order_release);
		replayer.cli_command_active.notify_one();
	}

	const bool sent = save_buffer && response.empty() ? true : lava_tcp_send_all(client_fd, response);
	if (sent && log_cursor_commit >= 0)
	{
		state->replay_log.cursor = log_cursor_commit;
	}
	if (sent && system_log_cursor_commit >= 0)
	{
		state->system_log.cursor = system_log_cursor_commit;
	}
	if (log_update_active)
	{
		state->replay_log.update_active.store(false, std::memory_order_release);
	}
	if (system_log_update_active)
	{
		state->system_log.update_active.store(false, std::memory_order_release);
	}
	if (!sent)
	{
		ELOG("Failed to send remote control response: %s", strerror(errno));
	}
	close(client_fd);
	service_client_done(state);
}

static void service_listener(service_client_state* client_state)
{
	set_thread_name("replay-listener");
	const int listen_fd = lava_tcp_listen(hostname, port);
	ILOG("Remote control listening on %s:%d", hostname.c_str(), port);
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
		client_state->active_clients.fetch_add(1, std::memory_order_acq_rel);
		std::thread client_thread(service_client, client_state, client_fd);
		client_thread.detach();
	}

	close(listen_fd);
	unsigned active_clients = client_state->active_clients.load(std::memory_order_acquire);
	while (active_clients != 0)
	{
		client_state->active_clients.wait(active_clients);
		active_clients = client_state->active_clients.load(std::memory_order_acquire);
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
	if (replayer.cli_thread.load(std::memory_order_acquire) == thread_id)
	{
		replayer.cli_running.store(false, std::memory_order_release);
		replayer.cli_running.notify_all();
	}
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

int lava_replay_main(int argc, char **argv)
{
	int start = 0;
	int end = -1;
	int remaining = argc - 1; // zeroth is name of program
	std::string filename;
	bool infodump = false;
	bool skip_missing_input = false;
	std::string wsi;
	std::string logfile;
	std::vector<replay_screenshot_range> screenshot_ranges;
	std::string screenshot_prefix = "screenshot_frame_";
	bool screenshot_prefix_set = false;
	bool service = false;
	std::thread service_thread;
	service_client_state service_state;

	port = p__port;
	if (p__sandbox_level == -1) p__sandbox_level = DEFAULT_SANDBOX_LEVEL;
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
			if (p__debug_destination != stdout) DIE("We already have a different debug file destination!");
			logfile = get_str(argv[++i], remaining);
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
		else if (match(argv[i], nullptr, "--device-fault-report", remaining))
		{
			replayer.device_fault_report_requested = true;
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
			else if (wsi != "xcb" && wsi != "wayland" && wsi != "headless"
#ifdef VK_USE_PLATFORM_ANDROID_KHR
				&& wsi != "android"
#endif
			)
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

	if (service && !logfile.empty()) DIE("Cannot use custom logfile with replay service mode");
	if (!logfile.empty())
	{
		p__debug_destination = fopen(logfile.c_str(), "w");
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
	if (replayer.device_fault_report_requested)
	{
		replayer.aftermath_context = aftermath_initialize(filename.c_str());
		if (replayer.aftermath_context)
		{
			replayer.aftermath_device_lost_callback = aftermath_handle_device_lost;
			replayer.aftermath_register_marker_callback = aftermath_register_marker;
		}
	}
	replayer.collect_trace_file_info(filename);

	if (wsi.empty()) wsi_initialize(nullptr);
	else wsi_initialize(wsi.c_str());

	if (service)
	{
#ifdef __ANDROID__
		FILE* service_log = fopen("lava-replay-service.log", "w+");
#else
		FILE* service_log = tmpfile();
#endif
		if (!service_log) DIE("Failed to create replay service log: %s", strerror(errno));
		p__debug_destination = service_log;
		service_state.log_session = service_log_session_id();
		service_state.replay_log.file = service_log;
		service_state.system_collector.start(filename);
		service_state.system_log.file = service_state.system_collector.output();
		replayer.cli_service.store(true, std::memory_order_release);
		replayer.cli_pipeline_executable_stats_requested = true;
		replayer.cli_memory_budget_requested = true;
		replayer.cli_shader_instrumentation_requested = true;
	}
	replayer.collect_trace_file_info(filename);

	if (wsi.empty()) wsi_initialize(nullptr);
	else wsi_initialize(wsi.c_str());

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
		service_thread = std::thread(service_listener, &service_state);
		replayer.cli_running.wait(false);
		if (service_stop_requested.load(std::memory_order_acquire))
		{
			replay_done.store(true, std::memory_order_release);
			replay_done.notify_all();
			service_thread.join();
			service_state.system_collector.stop();
			aftermath_shutdown(replayer.aftermath_context);
			replayer.aftermath_context = nullptr;
			close_debug_destination();
			return replayer.exit_status;
		}
	}

	run_multithreaded();
	if (!service_stop_requested.load(std::memory_order_acquire)) replay_instrumentation_cleanup_all();
	if (service)
	{
		replay_done.store(true, std::memory_order_release);
		replay_done.notify_all();
		replayer.cli_running.store(false, std::memory_order_release);
		replayer.cli_running.notify_all();
		done_var.wait(false);
		service_thread.join();
		service_state.system_collector.stop();
	}
	replayer.destroy_screenshot_resources();
	replayer.finalize();
	if (p__custom_allocator) allocators_print(stdout);
	if (!replayer.cleanup_after_stop()) cleanup_xcb_wsi_objects();
	vkuDestroyWrapper(library);
	aftermath_shutdown(replayer.aftermath_context);
	replayer.aftermath_context = nullptr;
	wsi_shutdown();
	close_debug_destination();
	return replayer.exit_status;
}
