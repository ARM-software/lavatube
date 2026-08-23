#include "swapchain_format_compatibility.h"

#include <limits>

bool compatible_swapchain_blit_formats(VkFormat captured_format, VkFormat replay_format)
{
	if (captured_format == VK_FORMAT_B8G8R8A8_UNORM && replay_format == VK_FORMAT_R8G8B8A8_UNORM) return true;
	if (captured_format == VK_FORMAT_R8G8B8A8_UNORM && replay_format == VK_FORMAT_B8G8R8A8_UNORM) return true;
	if (captured_format == VK_FORMAT_B8G8R8A8_SRGB && replay_format == VK_FORMAT_R8G8B8A8_SRGB) return true;
	if (captured_format == VK_FORMAT_R8G8B8A8_SRGB && replay_format == VK_FORMAT_B8G8R8A8_SRGB) return true;
	return false;
}

bool rewrite_mutable_swapchain_format_list(VkSwapchainCreateInfoKHR* create_info, VkFormat* format_storage)
{
	if (!(create_info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)) return true;
	if (!format_storage) return false;
	VkBaseOutStructure* extension = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(create_info->pNext));
	while (extension && extension->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
	{
		extension = extension->pNext;
	}
	if (!extension) return false;
	VkImageFormatListCreateInfo* format_list = reinterpret_cast<VkImageFormatListCreateInfo*>(extension);
	*format_storage = create_info->imageFormat;
	format_list->viewFormatCount = 1;
	format_list->pViewFormats = format_storage;
	return true;
}

static uint32_t swapchain_format_rank(VkFormat captured_format, VkFormat replay_format)
{
	if (captured_format == replay_format) return 0;
	if (compatible_swapchain_blit_formats(captured_format, replay_format)) return 1;
	return std::numeric_limits<uint32_t>::max();
}

swapchain_format_selection select_swapchain_surface_format(
	VkSurfaceFormatKHR captured_format,
	const std::vector<swapchain_surface_format_support>& replay_formats,
	VkFormatFeatureFlags captured_optimal_tiling_features,
	VkImageUsageFlags supported_surface_usage,
	bool allow_conversion,
	bool require_transfer_destination)
{
	swapchain_format_selection selection;
	for (const swapchain_surface_format_support& replay_format : replay_formats)
	{
		if (replay_format.surface_format.colorSpace != captured_format.colorSpace) continue;
		if (replay_format.surface_format.format != captured_format.format && replay_format.surface_format.format != VK_FORMAT_UNDEFINED) continue;
		selection.surface_format = captured_format;
		selection.failure = swapchain_format_selection_failure::none;
		if (require_transfer_destination && !(supported_surface_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		{
			selection.failure = swapchain_format_selection_failure::surface_transfer_dst_unsupported;
		}
		return selection;
	}

	if (!allow_conversion) return selection;

	const swapchain_surface_format_support* best_candidate = nullptr;
	const swapchain_surface_format_support* best_supported_candidate = nullptr;
	uint32_t best_rank = std::numeric_limits<uint32_t>::max();
	uint32_t best_supported_rank = std::numeric_limits<uint32_t>::max();
	for (const swapchain_surface_format_support& replay_format : replay_formats)
	{
		if (replay_format.surface_format.colorSpace != captured_format.colorSpace) continue;
		const uint32_t rank = swapchain_format_rank(captured_format.format, replay_format.surface_format.format);
		if (rank == std::numeric_limits<uint32_t>::max()) continue;
		if (!best_candidate || rank < best_rank
			|| (rank == best_rank && replay_format.surface_format.format < best_candidate->surface_format.format))
		{
			best_candidate = &replay_format;
			best_rank = rank;
		}
		if ((replay_format.optimal_tiling_features & VK_FORMAT_FEATURE_BLIT_DST_BIT)
			&& (!best_supported_candidate || rank < best_supported_rank
				|| (rank == best_supported_rank && replay_format.surface_format.format < best_supported_candidate->surface_format.format)))
		{
			best_supported_candidate = &replay_format;
			best_supported_rank = rank;
		}
	}

	if (!best_candidate) return selection;
	selection.surface_format = best_candidate->surface_format;
	selection.candidate_considered = true;
	selection.use_blit = true;
	if (require_transfer_destination && !(supported_surface_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
	{
		selection.failure = swapchain_format_selection_failure::surface_transfer_dst_unsupported;
	}
	else if (!(captured_optimal_tiling_features & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
	{
		selection.failure = swapchain_format_selection_failure::source_blit_unsupported;
	}
	else if (!best_supported_candidate)
	{
		selection.failure = swapchain_format_selection_failure::destination_blit_unsupported;
	}
	else
	{
		selection.surface_format = best_supported_candidate->surface_format;
		selection.failure = swapchain_format_selection_failure::none;
	}
	return selection;
}
