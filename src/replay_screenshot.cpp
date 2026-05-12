#include "replay_screenshot.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "generated/vk_wrapper_auto.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/tracetooltests/external/stb_image_write.h"

static std::string trim_copy(const std::string& in)
{
	size_t begin = 0;
	size_t end = in.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(in[begin]))) begin++;
	while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) end--;
	return in.substr(begin, end - begin);
}

static bool parse_frame_value(const std::string& text, uint32_t& out, std::string& error)
{
	const std::string trimmed = trim_copy(text);
	if (trimmed.empty())
	{
		error = "empty frame value";
		return false;
	}

	errno = 0;
	char* endptr = nullptr;
	const unsigned long long value = strtoull(trimmed.c_str(), &endptr, 10);
	if (errno != 0 || endptr == trimmed.c_str() || *endptr != '\0')
	{
		error = "invalid frame value \"" + trimmed + "\"";
		return false;
	}
	if (value > std::numeric_limits<uint32_t>::max())
	{
		error = "frame value out of range \"" + trimmed + "\"";
		return false;
	}
	out = static_cast<uint32_t>(value);
	return true;
}

bool parse_replay_screenshot_ranges(const std::string& spec, std::vector<replay_screenshot_range>& out, std::string& error)
{
	out.clear();
	error.clear();
	if (trim_copy(spec).empty())
	{
		error = "screenshot frame list cannot be empty";
		return false;
	}

	const std::vector<std::string> tokens = split(spec, ',');
	uint32_t previous_last = 0;
	bool have_previous = false;

	for (const std::string& token_raw : tokens)
	{
		const std::string token = trim_copy(token_raw);
		if (token.empty())
		{
			error = "empty screenshot frame token";
			return false;
		}

		replay_screenshot_range range{};
		const size_t dash = token.find('-');
		if (dash == std::string::npos)
		{
			if (!parse_frame_value(token, range.first, error)) return false;
			range.last = range.first;
		}
		else
		{
			if (token.find('-', dash + 1) != std::string::npos)
			{
				error = "invalid screenshot frame range \"" + token + "\"";
				return false;
			}
			if (!parse_frame_value(token.substr(0, dash), range.first, error)) return false;
			if (!parse_frame_value(token.substr(dash + 1), range.last, error)) return false;
			if (range.last < range.first)
			{
				error = "descending screenshot frame range \"" + token + "\"";
				return false;
			}
		}

		if (have_previous && range.first <= previous_last)
		{
			error = "screenshot frame ranges must be strictly ascending and non-overlapping";
			return false;
		}

		out.push_back(range);
		previous_last = range.last;
		have_previous = true;
	}

	return true;
}

void replay_screenshot_handler::set_prefix(std::string&& prefix)
{
	mPrefix = std::move(prefix);
}

void replay_screenshot_handler::set_ranges(std::vector<replay_screenshot_range>&& ranges)
{
	mRanges = std::move(ranges);
	mCurrentRange = 0;
}

bool replay_screenshot_handler::is_frame_selected(uint32_t frame)
{
	while (mCurrentRange < mRanges.size() && mRanges[mCurrentRange].last < frame) mCurrentRange++;
	if (mCurrentRange >= mRanges.size()) return false;
	const replay_screenshot_range& range = mRanges[mCurrentRange];
	return range.first <= frame && frame <= range.last;
}

static uint32_t choose_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	wrap_vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
	{
		if ((type_filter & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}

	const VkMemoryPropertyFlags relaxed = properties & ~(VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
	{
		if ((type_filter & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & relaxed) == relaxed)
		{
			return i;
		}
	}

	ABORT("Failed to find screenshot readback memory type");
	return UINT32_MAX;
}

static void destroy_internal_buffer(VkDevice device, internal_buffer& buffer)
{
	if (buffer.buffer != VK_NULL_HANDLE) wrap_vkDestroyBuffer(device, buffer.buffer, nullptr);
	if (buffer.memory != VK_NULL_HANDLE) wrap_vkFreeMemory(device, buffer.memory, nullptr);
	buffer = {};
}

static bool create_internal_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memory_flags, internal_buffer& out)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr };
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkResult result = wrap_vkCreateBuffer(device, &info, nullptr, &out.buffer);
	if (result != VK_SUCCESS)
	{
		ELOG("Failed to create screenshot readback buffer (size=%lu, usage=%u)", (unsigned long)size, (unsigned)usage);
		return false;
	}

	VkMemoryRequirements req = {};
	wrap_vkGetBufferMemoryRequirements(device, out.buffer, &req);

	VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr };
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = choose_memory_type(physical_device, req.memoryTypeBits, memory_flags);
	result = wrap_vkAllocateMemory(device, &alloc, nullptr, &out.memory);
	if (result != VK_SUCCESS)
	{
		wrap_vkDestroyBuffer(device, out.buffer, nullptr);
		out.buffer = VK_NULL_HANDLE;
		ELOG("Failed to allocate screenshot readback memory (size=%lu)", (unsigned long)req.size);
		return false;
	}

	result = wrap_vkBindBufferMemory(device, out.buffer, out.memory, 0);
	if (result != VK_SUCCESS)
	{
		wrap_vkFreeMemory(device, out.memory, nullptr);
		wrap_vkDestroyBuffer(device, out.buffer, nullptr);
		out = {};
		ELOG("Failed to bind screenshot readback buffer memory");
		return false;
	}

	out.size = req.size;
	out.usage = usage;
	out.memory_flags = memory_flags;
	return true;
}

void replay_screenshot_handler::destroy_resources(VkDevice device, device_resources& resources)
{
	if (resources.command_buffer != VK_NULL_HANDLE && resources.command_pool != VK_NULL_HANDLE)
	{
		wrap_vkFreeCommandBuffers(device, resources.command_pool, 1, &resources.command_buffer);
	}
	if (resources.fence != VK_NULL_HANDLE) wrap_vkDestroyFence(device, resources.fence, nullptr);
	if (resources.command_pool != VK_NULL_HANDLE) wrap_vkDestroyCommandPool(device, resources.command_pool, nullptr);
	destroy_internal_buffer(device, resources.staging);
	resources = {};
}

bool replay_screenshot_handler::ensure_resources(VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, VkDeviceSize size, device_resources& out)
{
	if (out.queue_family != UINT32_MAX && out.queue_family != queue_family)
	{
		destroy_resources(device, out);
	}

	if (out.staging.buffer == VK_NULL_HANDLE || out.staging.size < size)
	{
		destroy_internal_buffer(device, out.staging);
		if (!create_internal_buffer(device, physical_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, out.staging))
		{
			return false;
		}
	}

	if (out.command_pool == VK_NULL_HANDLE)
	{
		VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = queue_family;
		if (wrap_vkCreateCommandPool(device, &pool_info, nullptr, &out.command_pool) != VK_SUCCESS)
		{
			ELOG("Failed to create screenshot command pool");
			return false;
		}

		VkCommandBufferAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
		alloc_info.commandPool = out.command_pool;
		alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;
		if (wrap_vkAllocateCommandBuffers(device, &alloc_info, &out.command_buffer) != VK_SUCCESS)
		{
			ELOG("Failed to allocate screenshot command buffer");
			return false;
		}

		VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr };
		if (wrap_vkCreateFence(device, &fence_info, nullptr, &out.fence) != VK_SUCCESS)
		{
			ELOG("Failed to create screenshot fence");
			return false;
		}
	}

	out.queue_family = queue_family;
	return true;
}

static bool is_supported_screenshot_format(VkFormat format, bool& swizzle_bgra)
{
	swizzle_bgra = false;
	switch (format)
	{
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		return true;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		swizzle_bgra = true;
		return true;
	default:
		return false;
	}
}

bool replay_screenshot_handler::capture(uint32_t frame, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family,
	VkImage image, VkFormat format, uint32_t width, uint32_t height)
{
	bool swizzle_bgra = false;
	if (!is_supported_screenshot_format(format, swizzle_bgra))
	{
		DIE("Unsupported swapchain format %u for screenshot frame %u", (unsigned)format, frame);
	}
	if (width == 0 || height == 0)
	{
		DIE("Cannot create screenshot for zero-sized image at frame %u", frame);
	}

	const VkDeviceSize pixel_bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
	device_resources& resources = mResources[device];
	if (!ensure_resources(physical_device, device, queue_family, pixel_bytes, resources))
	{
		return false;
	}

	VkResult result = wrap_vkResetFences(device, 1, &resources.fence);
	if (result != VK_SUCCESS) DIE("Failed to reset screenshot fence: %s", errorString(result));
	result = wrap_vkResetCommandBuffer(resources.command_buffer, 0);
	if (result != VK_SUCCESS) DIE("Failed to reset screenshot command buffer: %s", errorString(result));

	VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr };
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = wrap_vkBeginCommandBuffer(resources.command_buffer, &begin_info);
	if (result != VK_SUCCESS) DIE("Failed to begin screenshot command buffer: %s", errorString(result));

	VkImageMemoryBarrier image_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr };
	image_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image_barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_barrier.image = image;
	image_barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	wrap_vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &image_barrier);

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { width, height, 1 };
	wrap_vkCmdCopyImageToBuffer(resources.command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resources.staging.buffer, 1, &region);

	VkBufferMemoryBarrier buffer_barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr };
	buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer = resources.staging.buffer;
	buffer_barrier.offset = 0;
	buffer_barrier.size = pixel_bytes;
	wrap_vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
	image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image_barrier.dstAccessMask = 0;
	image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	wrap_vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &image_barrier);

	result = wrap_vkEndCommandBuffer(resources.command_buffer);
	if (result != VK_SUCCESS) DIE("Failed to end screenshot command buffer: %s", errorString(result));

	VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &resources.command_buffer;
	result = wrap_vkQueueSubmit(queue, 1, &submit_info, resources.fence);
	if (result != VK_SUCCESS) DIE("Failed to submit screenshot command buffer: %s", errorString(result));
	result = wrap_vkWaitForFences(device, 1, &resources.fence, VK_TRUE, UINT64_MAX);
	if (result != VK_SUCCESS) DIE("Failed to wait for screenshot fence: %s", errorString(result));

	void* mapped = nullptr;
	result = wrap_vkMapMemory(device, resources.staging.memory, 0, pixel_bytes, 0, &mapped);
	if (result != VK_SUCCESS || !mapped) DIE("Failed to map screenshot readback memory: %s", errorString(result));
	if ((resources.staging.memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
	{
		VkMappedMemoryRange invalidate = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr };
		invalidate.memory = resources.staging.memory;
		invalidate.offset = 0;
		invalidate.size = pixel_bytes;
		result = wrap_vkInvalidateMappedMemoryRanges(device, 1, &invalidate);
		if (result != VK_SUCCESS) DIE("Failed to invalidate screenshot readback memory: %s", errorString(result));
	}

	std::vector<unsigned char> pixels(pixel_bytes);
	const uint8_t* src = static_cast<const uint8_t*>(mapped);
	if (swizzle_bgra)
	{
		for (VkDeviceSize i = 0; i < pixel_bytes; i += 4)
		{
			pixels[i + 0] = src[i + 2];
			pixels[i + 1] = src[i + 1];
			pixels[i + 2] = src[i + 0];
			pixels[i + 3] = 255;
		}
	}
	else
	{
		std::copy(src, src + pixel_bytes, pixels.begin());
		for (VkDeviceSize i = 3; i < pixel_bytes; i += 4) pixels[i] = 255;
	}
	wrap_vkUnmapMemory(device, resources.staging.memory);

	const std::string filename = mPrefix + std::to_string(frame) + ".png";
	if (stbi_write_png(filename.c_str(), width, height, 4, pixels.data(), 0) == 0)
	{
		ELOG("Failed to write screenshot file %s", filename.c_str());
		return false;
	}
	return true;
}

void replay_screenshot_handler::destroy()
{
	for (auto& entry : mResources)
	{
		destroy_resources(entry.first, entry.second);
	}
	mResources.clear();
}

void replay_screenshot_handler::destroy_device(VkDevice device)
{
	const auto it = mResources.find(device);
	if (it == mResources.end()) return;
	destroy_resources(device, it->second);
	mResources.erase(it);
}
