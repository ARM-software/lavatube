#pragma once

#include <deque>

#include "lavatube.h"

struct command_execution_data
{
	const trackeddevice& device_data;
	const trackedcmdbuffer& cmdbuffer_data;
	const address_remapper<trackedobject>& device_address_remapping;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, buffer_access>> descriptorsets; // descriptorset binding : set internal binding point : buffer
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, image_access>> imagesets; // descriptorset binding : set internal binding point : image
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>> opaquesets; // descriptorset binding : set internal binding point : opaque descriptor payload
	std::vector<std::byte> push_constants; // current state of the push constants
	host_write_regions push_constant_sources;
	std::list<address_rewrite>& global_output_rewrite_queue;
	std::deque<descriptor_rewrite>& pending_descriptor_rewrites;
	std::vector<descriptor_buffer_payload>& descriptor_buffer_payloads;
	struct
	{
		int commands = 0;
		int execution_commands = 0;
		uint64_t total_init_time = 0;
		uint64_t total_spirv_run_time = 0;
		struct
		{
			int shader_module_index = -1;
			VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
			uint64_t run_time_ns = 0;
		} slowest;
	} stats;
};

bool execute_commands(command_execution_data& data);
