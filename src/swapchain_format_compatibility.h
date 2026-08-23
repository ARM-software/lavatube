#pragma once

#include <vector>

#include "vulkan/vulkan.h"

enum class swapchain_format_selection_failure
{
	none,
	no_compatible_format,
	surface_transfer_dst_unsupported,
	source_blit_unsupported,
	destination_blit_unsupported,
};

struct swapchain_surface_format_support
{
	VkSurfaceFormatKHR surface_format = {};
	VkFormatFeatureFlags optimal_tiling_features = 0;
};

struct swapchain_format_selection
{
	VkSurfaceFormatKHR surface_format = {};
	swapchain_format_selection_failure failure = swapchain_format_selection_failure::no_compatible_format;
	bool use_blit = false;
	bool candidate_considered = false;

	bool supported() const
	{
		return failure == swapchain_format_selection_failure::none;
	}
};

bool compatible_swapchain_blit_formats(VkFormat captured_format, VkFormat replay_format);

bool rewrite_mutable_swapchain_format_list(VkSwapchainCreateInfoKHR* create_info, VkFormat* format_storage);

swapchain_format_selection select_swapchain_surface_format(
	VkSurfaceFormatKHR captured_format,
	const std::vector<swapchain_surface_format_support>& replay_formats,
	VkFormatFeatureFlags captured_optimal_tiling_features,
	VkImageUsageFlags supported_surface_usage,
	bool allow_conversion,
	bool require_transfer_destination);
