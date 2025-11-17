#pragma once

#include "vk_wrapper_auto.h"
#include "util.h"
#include "vulkan/vulkan.h"

/// Call to setup window system integration. Pass nullptr to use default WSI. Call before setting up your
/// security sandbox to make sure it does not interfere with the initial WSI connection.
void wsi_initialize(const char* wsi_name);

/// Tear down any connection to the window system.
void wsi_shutdown();

VkSurfaceKHR window_create(VkInstance instance, uint32_t index, int32_t x, int32_t y, int32_t width, int32_t height);
void window_destroy(VkInstance instance, uint32_t index);
void window_preallocate(uint32_t size);
const char* window_winsys();
void window_fullscreen(uint32_t index, bool value);
