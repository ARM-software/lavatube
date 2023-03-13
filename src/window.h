#pragma once

#include "vk_wrapper_auto.h"
#include "util.h"
#include "vulkan/vulkan.h"

VkSurfaceKHR window_create(VkInstance instance, uint32_t index, int32_t x, int32_t y, int32_t width, int32_t height);
void window_destroy(VkInstance instance, uint32_t index);
void window_preallocate(uint32_t size);
const char* window_winsys();
void window_set_winsys(const std::string& name);
void window_fullscreen(uint32_t index, bool value);
