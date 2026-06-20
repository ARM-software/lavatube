#pragma once

#include <vector>

#include "jsoncpp/json/value.h"
#include "vulkan/vulkan.h"

Json::Value pipeline_executable_statistic_value_json(const VkPipelineExecutableStatisticKHR& statistic);
Json::Value pipeline_executable_statistics_json(const std::vector<VkPipelineExecutablePropertiesKHR>& properties,
	const std::vector<std::vector<VkPipelineExecutableStatisticKHR>>& statistics);
bool append_pipeline_executable_statistics_json(VkDevice device, VkPipeline pipeline, Json::Value& out);
