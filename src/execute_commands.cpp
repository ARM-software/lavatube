#define SPV_ENABLE_UTILITY_CODE 1
#include <spirv/unified1/spirv.h>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include "spirv-simulator/framework/spirv_simulator.hpp"

#include "execute_commands.h"

#include "read_auto.h"
#include "read.h"
#include "util_auto.h"
#include "suballocator.h"

static bool run_spirv(const trackeddevice& device_data, const shader_stage& stage, const std::vector<std::byte>& push_constants,
	const std::unordered_map<uint32_t, std::unordered_map<uint32_t, buffer_access>>& descriptorsets,
	const std::unordered_map<uint32_t, std::unordered_map<uint32_t, image_access>>& imagesets)
{
	SPIRVSimulator::SimulationData inputs;
	SPIRVSimulator::SimulationResults results;
	std::unordered_map<const void*, std::pair<uint32_t, uint32_t>> binding_lookup;
	inputs.push_constants = push_constants.empty() ? nullptr : push_constants.data();
	inputs.entry_point_op_name = stage.name;
	inputs.specialization_constants = stage.specialization_data.empty() ? nullptr : stage.specialization_data.data();
	for (const auto& v : stage.specialization_constants)
	{
		inputs.specialization_constant_offsets[v.constantID] = v.offset;
	}
	for (const auto& set_pair : descriptorsets)
	{
		auto& set_bindings = inputs.bindings[set_pair.first];
		for (const auto& binding_pair : set_pair.second)
		{
			const buffer_access& access = binding_pair.second;
			if (!access.buffer_data) continue;
			const uint32_t buffer_index = access.buffer_data->index;
			suballoc_location loc = device_data.allocator->find_buffer_memory(buffer_index);
			std::byte* base = (std::byte*)loc.memory;
			std::byte* binding_ptr = base + access.offset;
			set_bindings[binding_pair.first] = binding_ptr;
			binding_lookup.emplace(binding_ptr, std::make_pair(set_pair.first, binding_pair.first));
			// Provide real runtime-array length information to the SPIR-V simulator
			const VkDeviceSize binding_size = (access.size != 0) ? access.size : (access.buffer_data->size - access.offset);
			if (binding_size > 0)
			{
				inputs.rt_array_lengths[reinterpret_cast<uint64_t>(binding_ptr)][0] = static_cast<size_t>(binding_size);
			}
		}
	}
	for (const auto& set_pair : imagesets)
	{
		auto& set_bindings = inputs.bindings[set_pair.first];
		for (const auto& binding_pair : set_pair.second)
		{
			if (set_bindings.count(binding_pair.first) > 0) continue;
			const image_access& access = binding_pair.second;
			void* binding_ptr = nullptr;
			if (access.image_data)
			{
				suballoc_location loc = device_data.allocator->find_image_memory(access.image_data->index);
				std::byte* base = (std::byte*)loc.memory;
				binding_ptr = base + loc.offset;
			}
			else
			{
				static uint64_t dummy_descriptor = 0;
				binding_ptr = &dummy_descriptor;
			}
			set_bindings[binding_pair.first] = binding_ptr;
		}
	}
	inputs.shader_id = stage.unique_index;
	SPIRVSimulator::SPIRVSimulator sim(stage.code, &inputs, &results, nullptr, false, ERROR_RAISE_ON_BUFFERS_INCOMPLETE);
	sim.Run();

	for (const auto& candidates : results.output_candidates)
	{
		ILOG("Found set of %u candidates for %p", (unsigned)candidates.second.size(), candidates.first);
		const void* base_ptr = candidates.first;
		const auto binding_it = binding_lookup.find(base_ptr);
		for (const auto& candidate : candidates.second)
		{
			if (binding_it != binding_lookup.end())
			{
				ILOG("SPIRV candidate %s in %s set=%u binding=%u base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED", stage.name.c_str(),
					(unsigned)binding_it->second.first, (unsigned)binding_it->second.second, base_ptr, (unsigned long)candidate.offset,
					(unsigned long long)candidate.address);
			}
			else
			{
				ILOG("SPIRV candidate %s in %s base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED", stage.name.c_str(), base_ptr,
					(unsigned long)candidate.offset, (unsigned long long)candidate.address);
			}
		}
	}

	return true;
}

struct mapped_address_range
{
	std::byte* ptr = nullptr;
	VkDeviceSize size = 0;
	trackedbuffer* buffer_data = nullptr;
};

static bool map_device_address_range(const lava_file_reader& reader, const trackeddevice& device_data, VkDeviceAddress address, VkDeviceSize size,
	mapped_address_range& out, const char* label)
{
	if (address == 0 || size == 0) return false;
	trackedobject* obj = reader.parent->device_address_remapping.get_by_address(address);
	if (!obj || obj->object_type != VK_OBJECT_TYPE_BUFFER)
	{
		DLOG("Ray tracing %s address 0x%llx is not a buffer", label, (unsigned long long)address);
		return false;
	}
	trackedbuffer* buffer_data = static_cast<trackedbuffer*>(obj);
	const uint64_t translated = reader.parent->device_address_remapping.translate_address(address);
	if (translated == 0)
	{
		DLOG("Ray tracing %s address 0x%llx could not be remapped", label, (unsigned long long)address);
		return false;
	}
	const VkDeviceSize offset = translated - buffer_data->device_address;
	if (offset + size > buffer_data->size)
	{
		DLOG("Ray tracing %s out of bounds: offset=%llu size=%llu buffer=%llu", label, (unsigned long long)offset,
			(unsigned long long)size, (unsigned long long)buffer_data->size);
		return false;
	}
	suballoc_location loc = device_data.allocator->find_buffer_memory(buffer_data->index);
	out.ptr = (std::byte*)loc.memory + offset;
	out.size = size;
	out.buffer_data = buffer_data;
	return true;
}

static void add_raytracing_group_stages(const trackedpipeline& pipeline_data, uint32_t group_index, std::unordered_set<uint32_t>& stages)
{
	if (group_index >= pipeline_data.raytracing_groups.size()) return;
	const raytracing_group& group = pipeline_data.raytracing_groups[group_index];
	auto add_stage = [&](uint32_t stage_index)
	{
		if (stage_index == VK_SHADER_UNUSED_KHR) return;
		if (stage_index >= pipeline_data.shader_stages.size()) return;
		stages.insert(stage_index);
	};
	if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
	{
		add_stage(group.general_shader);
	}
	else
	{
		add_stage(group.closest_hit_shader);
		add_stage(group.any_hit_shader);
		add_stage(group.intersection_shader);
	}
}

static int find_raytracing_group_index(const trackedpipeline& pipeline_data, const std::byte* handle_ptr)
{
	const uint32_t handle_size = pipeline_data.raytracing_group_handle_size;
	if (handle_size == 0) return -1;
	uint32_t group_count = pipeline_data.raytracing_group_count;
	if (group_count == 0) group_count = (uint32_t)pipeline_data.raytracing_groups.size();
	if (group_count == 0) return -1;
	const size_t handle_capacity = pipeline_data.raytracing_group_handles.size();
	const size_t max_groups = handle_capacity / handle_size;
	if (max_groups == 0) return -1;
	if (group_count > max_groups) group_count = (uint32_t)max_groups;
	const std::byte* base = pipeline_data.raytracing_group_handles.data();
	for (uint32_t i = 0; i < group_count; i++)
	{
		const std::byte* candidate = base + (size_t)i * handle_size;
		if (memcmp(candidate, handle_ptr, handle_size) == 0) return (int)i;
	}
	return -1;
}

static void collect_stages_from_sbt_region(const lava_file_reader& reader, const trackeddevice& device_data, const trackedpipeline& pipeline_data,
	const VkStridedDeviceAddressRegionKHR& region, const char* label, std::unordered_set<uint32_t>& stages)
{
	if (region.deviceAddress == 0 || region.size == 0) return;
	if (pipeline_data.raytracing_group_handle_size == 0 || pipeline_data.raytracing_group_handles.empty()) return;
	VkDeviceSize stride = region.stride;
	if (stride == 0) stride = region.size;
	if (stride < pipeline_data.raytracing_group_handle_size || region.size < pipeline_data.raytracing_group_handle_size)
	{
		DLOG("Ray tracing %s region stride/size too small for handle size", label);
		return;
	}
	mapped_address_range mapping;
	if (!map_device_address_range(reader, device_data, region.deviceAddress, region.size, mapping, label)) return;
	const uint32_t record_count = (uint32_t)(region.size / stride);
	for (uint32_t i = 0; i < record_count; i++)
	{
		const std::byte* handle_ptr = mapping.ptr + (size_t)i * stride;
		const int group_index = find_raytracing_group_index(pipeline_data, handle_ptr);
		if (group_index >= 0) add_raytracing_group_stages(pipeline_data, (uint32_t)group_index, stages);
	}
}

bool execute_commands(lava_file_reader& reader, const trackeddevice& device_data, VkCommandBuffer commandBuffer)
{
	std::vector<std::byte> push_constants; // current state of the push constants
	uint32_t compute_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	std::unordered_map<VkShaderStageFlagBits, uint32_t> shader_objects;
	uint32_t graphics_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t raytracing_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, buffer_access>> descriptorsets; // descriptorset binding : set internal binding point : buffer
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, image_access>> imagesets; // descriptorset binding : set internal binding point : image
	for (const auto& c : cmdbuffer_data.commands)
	{
		switch (c.id)
		{
		case VKCMDBINDDESCRIPTORSETS:
			for (uint32_t i = 0; i < c.data.bind_descriptorsets.descriptorSetCount; i++)
			{
				uint32_t set = c.data.bind_descriptorsets.firstSet + i;
				auto& tds = VkDescriptorSet_index.at(c.data.bind_descriptorsets.pDescriptorSets[i]); // is index now
				for (auto pair : tds.bound_buffers)
				{
					descriptorsets[set][pair.first] = pair.second;
				}
				for (auto pair : tds.bound_images)
				{
					imagesets[set][pair.first] = pair.second;
				}
				uint32_t binding = 0;
				for (auto pair : tds.dynamic_buffers)
				{
					buffer_access access;
					const uint32_t buffer_index = index_to_VkBuffer.index(pair.second.buffer);
					auto& buffer_data = VkBuffer_index.at(buffer_index);
					access.buffer_data = &buffer_data;
					access.offset = pair.second.offset;
					if (c.data.bind_descriptorsets.pDynamicOffsets) access.offset += c.data.bind_descriptorsets.pDynamicOffsets[binding];
					access.size = pair.second.range;
					if (access.size == VK_WHOLE_SIZE) access.size = buffer_data.size - access.offset;
					descriptorsets[set][pair.first] = access;
					binding++;
					assert(!c.data.bind_descriptorsets.pDynamicOffsets || c.data.bind_descriptorsets.dynamicOffsetCount >= binding);
				}
			}
			free((void*)c.data.bind_descriptorsets.pDescriptorSets);
			free((void*)c.data.bind_descriptorsets.pDynamicOffsets);
			break;
		case VKCMDCOPYBUFFER:
			{
				suballoc_location src = device_data.allocator->find_buffer_memory(c.data.copy_buffer.src_buffer_index);
				suballoc_location dst = device_data.allocator->find_buffer_memory(c.data.copy_buffer.dst_buffer_index);
				trackedbuffer& src_buffer = VkBuffer_index.at(c.data.copy_buffer.src_buffer_index);
				trackedbuffer& dst_buffer = VkBuffer_index.at(c.data.copy_buffer.dst_buffer_index);
				for (uint32_t i = 0; i < c.data.copy_buffer.regionCount; i++)
				{
					VkBufferCopy& r = c.data.copy_buffer.pRegions[i];
					memcpy((char*)dst.memory + r.dstOffset, (char*)src.memory + r.srcOffset, r.size);
					dst_buffer.source.copy_sources(src_buffer.source, r.dstOffset, r.srcOffset, r.size);
				}
			}
			free(c.data.copy_buffer.pRegions);
			break;
		case VKCMDUPDATEBUFFER:
			{
				suballoc_location sub = device_data.allocator->find_buffer_memory(c.data.update_buffer.buffer_index);
				trackedbuffer& dst_buffer = VkBuffer_index.at(c.data.update_buffer.buffer_index);
				memcpy((char*)sub.memory + c.data.update_buffer.offset, c.data.update_buffer.values, c.data.update_buffer.size);
				dst_buffer.source.register_source(c.data.update_buffer.offset, c.data.update_buffer.size, c.source);
			}
			free(c.data.update_buffer.values);
			break;
		case VKCMDPUSHCONSTANTS:
		case VKCMDPUSHCONSTANTS2KHR:
			if (push_constants.size() < c.data.push_constants.offset + c.data.push_constants.size) push_constants.resize(c.data.push_constants.offset + c.data.push_constants.size);
			memcpy(push_constants.data() + c.data.push_constants.offset, c.data.push_constants.values, c.data.push_constants.size);
			free(c.data.push_constants.values);
			break;
		case VKCMDBINDPIPELINE:
			if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) graphics_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
			{
				compute_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			}
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) raytracing_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			break;
		case VKCMDPUSHDESCRIPTORSETKHR:
			assert(false); // TBD
			break;
		case VKCMDBINDSHADERSEXT:
			for (uint32_t i = 0; i < c.data.bind_shaders_ext.stageCount; i++)
			{
				if (c.data.bind_shaders_ext.shader_objects[i] != CONTAINER_NULL_VALUE) shader_objects[c.data.bind_shaders_ext.shader_types[i]] = c.data.bind_shaders_ext.shader_objects[i];
				else shader_objects.erase(c.data.bind_shaders_ext.shader_types[i]); // explicit unbind
			}
			free(c.data.bind_shaders_ext.shader_types);
			free(c.data.bind_shaders_ext.shader_objects);
			break;
		case VKCMDDISPATCH: // proxy for all compute commands
			if (compute_pipeline_bound != CONTAINER_INVALID_INDEX) // old style pipeline
			{
				auto& pipeline_data = VkPipeline_index.at(compute_pipeline_bound);
				assert(pipeline_data.shader_stages.size() == 1);
				assert(pipeline_data.shader_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
				pipeline_data.shader_stages[0].calls++;
				run_spirv(device_data, pipeline_data.shader_stages[0], push_constants, descriptorsets, imagesets);
			}
			else // shader objects
			{
				auto& shader_object_data = VkShaderEXT_index.at(shader_objects.at(VK_SHADER_STAGE_COMPUTE_BIT));
				shader_object_data.stage.calls++;
				run_spirv(device_data, shader_object_data.stage, push_constants, descriptorsets, imagesets);
			}
			break;
		case VKCMDDRAW: // proxy for all draw commands
			if (graphics_pipeline_bound != CONTAINER_INVALID_INDEX) // old style pipeline
			{
				auto& pipeline_data = VkPipeline_index.at(graphics_pipeline_bound);
				for (auto& stage : pipeline_data.shader_stages)
				{
					if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT) continue;
					stage.calls++;
					run_spirv(device_data, stage, push_constants, descriptorsets, imagesets);
				}
			}
			else for (auto& pair : shader_objects) // shader objects
			{
				if (pair.first == VK_SHADER_STAGE_COMPUTE_BIT) continue;
				auto& shader_object_data = VkShaderEXT_index.at(pair.second);
				shader_stage& stage = shader_object_data.stage;
				assert(pair.first == stage.stage);
				stage.calls++;
				run_spirv(device_data, stage, push_constants, descriptorsets, imagesets);
			}
			break;
		case VKCMDTRACERAYSKHR: // proxy for all raytracing commands
			{
				if (raytracing_pipeline_bound == CONTAINER_INVALID_INDEX) break;
				auto& pipeline_data = VkPipeline_index.at(raytracing_pipeline_bound);
				if (pipeline_data.shader_stages.empty()) break;
				if (!c.trace_rays_valid)
				{
					for (auto& stage : pipeline_data.shader_stages)
					{
						stage.calls++;
						run_spirv(device_data, stage, push_constants, descriptorsets, imagesets);
					}
					break;
				}

				VkStridedDeviceAddressRegionKHR raygen = c.data.trace_rays.raygen;
				VkStridedDeviceAddressRegionKHR miss = c.data.trace_rays.miss;
				VkStridedDeviceAddressRegionKHR hit = c.data.trace_rays.hit;
				VkStridedDeviceAddressRegionKHR callable = c.data.trace_rays.callable;
				uint32_t width = c.data.trace_rays.width;
				uint32_t height = c.data.trace_rays.height;
				uint32_t depth = c.data.trace_rays.depth;

				if (c.data.trace_rays.mode == trackedcommand::TRACE_RAYS_INDIRECT)
				{
					mapped_address_range mapping;
					if (map_device_address_range(reader, device_data, c.data.trace_rays.indirect_device_address, sizeof(VkTraceRaysIndirectCommandKHR), mapping, "indirect"))
					{
						const VkTraceRaysIndirectCommandKHR* indirect = reinterpret_cast<const VkTraceRaysIndirectCommandKHR*>(mapping.ptr);
						width = indirect->width;
						height = indirect->height;
						depth = indirect->depth;
					}
					else
					{
						DLOG("Ray tracing indirect command could not be read");
					}
				}
				else if (c.data.trace_rays.mode == trackedcommand::TRACE_RAYS_INDIRECT2)
				{
					mapped_address_range mapping;
					if (map_device_address_range(reader, device_data, c.data.trace_rays.indirect_device_address, sizeof(VkTraceRaysIndirectCommand2KHR), mapping, "indirect2"))
					{
						const VkTraceRaysIndirectCommand2KHR* indirect = reinterpret_cast<const VkTraceRaysIndirectCommand2KHR*>(mapping.ptr);
						raygen.deviceAddress = indirect->raygenShaderRecordAddress;
						raygen.size = indirect->raygenShaderRecordSize;
						raygen.stride = indirect->raygenShaderRecordSize;
						miss.deviceAddress = indirect->missShaderBindingTableAddress;
						miss.size = indirect->missShaderBindingTableSize;
						miss.stride = indirect->missShaderBindingTableStride;
						hit.deviceAddress = indirect->hitShaderBindingTableAddress;
						hit.size = indirect->hitShaderBindingTableSize;
						hit.stride = indirect->hitShaderBindingTableStride;
						callable.deviceAddress = indirect->callableShaderBindingTableAddress;
						callable.size = indirect->callableShaderBindingTableSize;
						callable.stride = indirect->callableShaderBindingTableStride;
						width = indirect->width;
						height = indirect->height;
						depth = indirect->depth;
					}
					else
					{
						DLOG("Ray tracing indirect2 command could not be read");
					}
				}

				std::unordered_set<uint32_t> stages_to_run;
				collect_stages_from_sbt_region(reader, device_data, pipeline_data, raygen, "raygen", stages_to_run);
				collect_stages_from_sbt_region(reader, device_data, pipeline_data, miss, "miss", stages_to_run);
				collect_stages_from_sbt_region(reader, device_data, pipeline_data, hit, "hit", stages_to_run);
				collect_stages_from_sbt_region(reader, device_data, pipeline_data, callable, "callable", stages_to_run);

				if (stages_to_run.empty())
				{
					for (auto& stage : pipeline_data.shader_stages)
					{
						stage.calls++;
						run_spirv(device_data, stage, push_constants, descriptorsets, imagesets);
					}
				}
				else
				{
					for (uint32_t stage_index : stages_to_run)
					{
						pipeline_data.shader_stages[stage_index].calls++;
						run_spirv(device_data, pipeline_data.shader_stages[stage_index], push_constants, descriptorsets, imagesets);
					}
				}
				(void)width;
				(void)height;
				(void)depth;
			}
			break;
		default:
			break;
		}
	}
	cmdbuffer_data.commands.clear();
	return true;
}
