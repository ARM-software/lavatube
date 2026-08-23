#include <assert.h>

#include "swapchain_format_compatibility.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

static swapchain_surface_format_support support(VkFormat format, VkColorSpaceKHR colorSpace, VkFormatFeatureFlags features)
{
	swapchain_surface_format_support value;
	value.surface_format = { format, colorSpace };
	value.optimal_tiling_features = features;
	return value;
}

static swapchain_format_selection select(
	VkFormat captured,
	const std::vector<swapchain_surface_format_support>& formats,
	VkFormatFeatureFlags captured_features = VK_FORMAT_FEATURE_BLIT_SRC_BIT,
	VkImageUsageFlags surface_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT)
{
	const VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	const VkSurfaceFormatKHR captured_surface_format = { captured, color_space };
	return select_swapchain_surface_format(captured_surface_format, formats, captured_features, surface_usage, true, true);
}

int main()
{
	const VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	{
		const VkFormat captured_view_formats[] = {
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_B8G8R8A8_SRGB,
		};
		VkImageFormatListCreateInfo format_list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr };
		format_list.viewFormatCount = 2;
		format_list.pViewFormats = captured_view_formats;
		VkSwapchainCreateInfoKHR create_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, &format_list };
		create_info.flags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
		create_info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
		VkFormat format_storage = VK_FORMAT_UNDEFINED;
		assert(rewrite_mutable_swapchain_format_list(&create_info, &format_storage));
		assert(format_list.viewFormatCount == 1);
		assert(format_list.pViewFormats == &format_storage);
		assert(format_list.pViewFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
	}
	{
		VkSwapchainCreateInfoKHR create_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr };
		create_info.flags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
		create_info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
		VkFormat format_storage = VK_FORMAT_UNDEFINED;
		assert(!rewrite_mutable_swapchain_format_list(&create_info, &format_storage));
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_B8G8R8A8_UNORM, color_space, 0),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats, 0);
		assert(selection.supported());
		assert(!selection.use_blit);
		assert(selection.surface_format.format == VK_FORMAT_B8G8R8A8_UNORM);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_UNORM, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats);
		assert(selection.supported());
		assert(selection.use_blit);
		assert(selection.surface_format.format == VK_FORMAT_R8G8B8A8_UNORM);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_SRGB, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_SRGB, formats);
		assert(selection.supported());
		assert(selection.use_blit);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_SRGB, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		assert(!select(VK_FORMAT_B8G8R8A8_UNORM, formats).supported());
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		assert(!select(VK_FORMAT_B8G8R8A8_UNORM, formats).supported());
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_UNORM, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats, 0);
		assert(selection.failure == swapchain_format_selection_failure::source_blit_unsupported);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_UNORM, color_space, 0),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats);
		assert(selection.failure == swapchain_format_selection_failure::destination_blit_unsupported);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_UNORM, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats,
			VK_FORMAT_FEATURE_BLIT_SRC_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		assert(selection.failure == swapchain_format_selection_failure::surface_transfer_dst_unsupported);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_B8G8R8A8_UNORM, color_space, 0),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats,
			0, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		assert(selection.failure == swapchain_format_selection_failure::surface_transfer_dst_unsupported);
	}
	{
		const std::vector<swapchain_surface_format_support> formats = {
			support(VK_FORMAT_R8G8B8A8_SRGB, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
			support(VK_FORMAT_R8G8B8A8_UNORM, color_space, VK_FORMAT_FEATURE_BLIT_DST_BIT),
			support(VK_FORMAT_B8G8R8A8_UNORM, color_space, 0),
		};
		const swapchain_format_selection selection = select(VK_FORMAT_B8G8R8A8_UNORM, formats);
		assert(selection.supported());
		assert(!selection.use_blit);
		assert(selection.surface_format.format == VK_FORMAT_B8G8R8A8_UNORM);
	}
	return 0;
}
