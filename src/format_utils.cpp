#include "format_utils.h"

#include <vulkan/vulkan_core.h>

#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XCB_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#define VULKAN_H_ 1
#include <vulkan/vulkan_format_traits.hpp>

uint32_t lava_plane_compatible_format(uint32_t format, uint32_t plane)
{
	const auto compatible = VULKAN_HPP_NAMESPACE::planeCompatibleFormat(
		static_cast<VULKAN_HPP_NAMESPACE::Format>(format), plane);
	return static_cast<uint32_t>(compatible);
}

lava_format_block_info lava_format_block_info_get(uint32_t format)
{
	const auto vk_format = static_cast<VULKAN_HPP_NAMESPACE::Format>(format);
	const auto extent = VULKAN_HPP_NAMESPACE::blockExtent(vk_format);
	return {
		static_cast<uint32_t>(extent[0] ? extent[0] : 1),
		static_cast<uint32_t>(extent[1] ? extent[1] : 1),
		static_cast<uint32_t>(extent[2] ? extent[2] : 1),
		VULKAN_HPP_NAMESPACE::blockSize(vk_format)
	};
}
