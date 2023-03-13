#pragma once
#include "jsoncpp/json/value.h"
#include "util.h"

bool readVkPhysicalDeviceVulkan13Features(const Json::Value& root, VkPhysicalDeviceVulkan13Features& in);
bool readVkPhysicalDeviceVulkan12Features(const Json::Value& root, VkPhysicalDeviceVulkan12Features& in);
bool readVkPhysicalDeviceVulkan11Features(const Json::Value& root, VkPhysicalDeviceVulkan11Features& in);
bool readVkPhysicalDeviceFeatures2(const Json::Value& root, VkPhysicalDeviceFeatures2& in);
bool readVkPhysicalDeviceSparseProperties(const Json::Value& root, VkPhysicalDeviceSparseProperties& in);
bool readVkPhysicalDeviceLimits(const Json::Value& root, VkPhysicalDeviceLimits& in);

Json::Value writeVkPhysicalDeviceVulkan13Features(const VkPhysicalDeviceVulkan13Features& in);
Json::Value writeVkPhysicalDeviceVulkan12Features(const VkPhysicalDeviceVulkan12Features& in);
Json::Value writeVkPhysicalDeviceVulkan11Features(const VkPhysicalDeviceVulkan11Features& in);
Json::Value writeVkPhysicalDeviceFeatures2(const VkPhysicalDeviceFeatures2& in);
Json::Value writeVkPhysicalDeviceSparseProperties(const VkPhysicalDeviceSparseProperties& in);
Json::Value writeVkPhysicalDeviceLimits(const VkPhysicalDeviceLimits& in);
