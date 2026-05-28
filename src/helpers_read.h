#pragma once

#include "read.h"

// FIXME get rid of most of these globals
extern VkInstance stored_instance;
extern VkPhysicalDevice selected_physical_device;
extern uint32_t selected_queue_family_index;
extern bool has_pipeline_feedback;
extern bool has_pipeline_control;
extern bool has_debug_report;
extern bool has_debug_utils;
extern bool host_has_frame_boundary;

uint64_t debug_object_lookup(VkDebugReportObjectTypeEXT type, uint32_t index);
uint64_t debug_object_lookup_output(VkDebugReportObjectTypeEXT type, uint32_t index);
uint64_t object_lookup(VkObjectType type, uint32_t index);
uint64_t object_lookup_output(VkObjectType type, uint32_t index);
trackable& object_trackable(VkObjectType type, uint64_t handle);
uint64_t queue_lookup_fake_handle(uint32_t index);
VkQueue queue_lookup_output_handle(uint32_t index, VkQueue fallback);

void memory_report_callback(const VkDeviceMemoryReportCallbackDataEXT* pCallbackData, void* pUserData);
VkBool32 VKAPI_PTR messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);
VkBool32 VKAPI_PTR debug_report_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData);

const char* const* device_layers(lava_file_reader& reader, uint32_t& len);
const char* const* instance_layers(lava_file_reader& reader, uint32_t& len);
const char* const* instance_extensions(lava_file_reader& reader, uint32_t& len);
const char* const* device_extensions(VkDeviceCreateInfo* sptr, lava_file_reader& reader, uint32_t& len);
