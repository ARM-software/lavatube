#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lavatube.h"

struct replay_screenshot_range
{
	uint32_t first = 0;
	uint32_t last = 0;
};

bool parse_replay_screenshot_ranges(const std::string& spec, std::vector<replay_screenshot_range>& out, std::string& error);

class replay_screenshot_handler
{
public:
	void set_prefix(std::string&& prefix);
	void set_ranges(std::vector<replay_screenshot_range>&& ranges);
	bool enabled() const { return !mRanges.empty(); }
	bool is_frame_selected(uint32_t frame);
	void destroy();
	void destroy_device(VkDevice device);
	bool capture(uint32_t frame, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family,
		VkImage image, VkImageLayout image_layout, VkFormat format, uint32_t width, uint32_t height);

private:
	struct device_resources
	{
		internal_buffer staging;
		VkCommandPool command_pool = VK_NULL_HANDLE;
		VkCommandBuffer command_buffer = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		uint32_t queue_family = UINT32_MAX;
	};

	bool ensure_resources(VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, VkDeviceSize size, device_resources& out);
	void destroy_resources(VkDevice device, device_resources& resources);

	std::string mPrefix = "screenshot_frame_";
	std::vector<replay_screenshot_range> mRanges;
	size_t mCurrentRange = 0;
	std::map<VkDevice, device_resources> mResources;
};
