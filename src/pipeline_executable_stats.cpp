#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "pipeline_executable_stats.h"

#include "util.h"
#include "vk_wrapper_auto.h"

static std::string pipeline_executable_name(const char* name)
{
	return std::string(name, strnlen(name, VK_MAX_DESCRIPTION_SIZE));
}

Json::Value pipeline_executable_statistic_value_json(const VkPipelineExecutableStatisticKHR& statistic)
{
	switch (statistic.format)
	{
	case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
		return Json::Value(statistic.value.b32 == VK_TRUE);
	case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
		return Json::Value((Json::Int64)statistic.value.i64);
	case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
		return Json::Value((Json::UInt64)statistic.value.u64);
	case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
		return Json::Value(statistic.value.f64);
	default:
		return Json::Value();
	}
}

Json::Value pipeline_executable_statistics_json(const std::vector<VkPipelineExecutablePropertiesKHR>& properties,
	const std::vector<std::vector<VkPipelineExecutableStatisticKHR>>& statistics)
{
	Json::Value executables(Json::objectValue);
	const size_t executable_count = std::min(properties.size(), statistics.size());
	for (size_t executable_index = 0; executable_index < executable_count; executable_index++)
	{
		Json::Value executable_statistics(Json::objectValue);
		for (const VkPipelineExecutableStatisticKHR& statistic : statistics[executable_index])
		{
			executable_statistics[pipeline_executable_name(statistic.name)] = pipeline_executable_statistic_value_json(statistic);
		}
		executables[pipeline_executable_name(properties[executable_index].name)] = executable_statistics;
	}
	return executables;
}

static bool query_pipeline_executable_properties(VkDevice device, VkPipeline pipeline, std::vector<VkPipelineExecutablePropertiesKHR>& properties)
{
	if (!wrap_vkGetPipelineExecutablePropertiesKHR) return false;

	VkPipelineInfoKHR pipeline_info = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline };
	uint32_t executable_count = 0;
	VkResult result = wrap_vkGetPipelineExecutablePropertiesKHR(device, &pipeline_info, &executable_count, nullptr);
	if (result != VK_SUCCESS)
	{
		DLOG("vkGetPipelineExecutablePropertiesKHR count query failed: %s", errorString(result));
		return false;
	}

	properties.resize(executable_count);
	for (VkPipelineExecutablePropertiesKHR& property : properties)
	{
		property = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR, nullptr };
	}

	result = wrap_vkGetPipelineExecutablePropertiesKHR(device, &pipeline_info, &executable_count, properties.data());
	if (result != VK_SUCCESS)
	{
		DLOG("vkGetPipelineExecutablePropertiesKHR data query failed: %s", errorString(result));
		return false;
	}
	properties.resize(executable_count);
	return true;
}

static bool query_pipeline_executable_statistics(VkDevice device, VkPipeline pipeline, uint32_t executable_index,
	std::vector<VkPipelineExecutableStatisticKHR>& statistics)
{
	if (!wrap_vkGetPipelineExecutableStatisticsKHR) return false;

	VkPipelineExecutableInfoKHR executable_info = {
		VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
		nullptr,
		pipeline,
		executable_index
	};
	uint32_t statistic_count = 0;
	VkResult result = wrap_vkGetPipelineExecutableStatisticsKHR(device, &executable_info, &statistic_count, nullptr);
	if (result != VK_SUCCESS)
	{
		DLOG("vkGetPipelineExecutableStatisticsKHR count query failed for executable %u: %s", executable_index, errorString(result));
		return false;
	}

	statistics.resize(statistic_count);
	for (VkPipelineExecutableStatisticKHR& statistic : statistics)
	{
		statistic = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR, nullptr };
	}

	result = wrap_vkGetPipelineExecutableStatisticsKHR(device, &executable_info, &statistic_count, statistics.data());
	if (result != VK_SUCCESS)
	{
		DLOG("vkGetPipelineExecutableStatisticsKHR data query failed for executable %u: %s", executable_index, errorString(result));
		return false;
	}
	statistics.resize(statistic_count);
	return true;
}

bool append_pipeline_executable_statistics_json(VkDevice device, VkPipeline pipeline, Json::Value& out)
{
	std::vector<VkPipelineExecutablePropertiesKHR> properties;
	if (!query_pipeline_executable_properties(device, pipeline, properties)) return false;

	std::vector<std::vector<VkPipelineExecutableStatisticKHR>> statistics(properties.size());
	for (uint32_t executable_index = 0; executable_index < properties.size(); executable_index++)
	{
		if (!query_pipeline_executable_statistics(device, pipeline, executable_index, statistics[executable_index])) return false;
	}

	out["pipeline_executables"] = pipeline_executable_statistics_json(properties, statistics);
	return true;
}
