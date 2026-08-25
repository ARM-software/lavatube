#pragma once

#include <string>

#include "read.h"

void cli_process_instrument_request(callback_context& cb, VkCommandBuffer command_buffer);
void replay_instrumentation_pre_shader_command(lava_file_reader& reader, VkCommandBuffer command_buffer, trackedcmdbuffer& commandbuffer_data);
void replay_instrumentation_post_shader_command(lava_file_reader& reader, VkCommandBuffer command_buffer, trackedcmdbuffer& commandbuffer_data);
void replay_instrumentation_end_command_buffer(lava_file_reader& reader, VkCommandBuffer command_buffer);
void replay_nvidia_marker_as_build(lava_file_reader& reader, VkCommandBuffer command_buffer, uint32_t info_count,
	const VkAccelerationStructureBuildGeometryInfoKHR* infos,
	const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, bool before);
std::string replay_as_build_diagnostic(const lava_file_reader& reader, uint32_t commandbuffer_index, uint32_t info_count,
	const VkAccelerationStructureBuildGeometryInfoKHR* infos,
	const VkAccelerationStructureBuildRangeInfoKHR* const* ranges);
void replay_instrumentation_mark_submitted(trackedcmdbuffer& commandbuffer_data);
void replay_instrumentation_cleanup_command_buffer(trackedcmdbuffer& commandbuffer_data);
void replay_instrumentation_cleanup_all();
std::string replay_instrumentation_show(uint32_t commandbuffer_index);
std::string replay_as_build_show(uint32_t commandbuffer_index);
