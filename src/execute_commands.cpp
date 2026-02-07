#define SPV_ENABLE_UTILITY_CODE 1
#include <spirv/unified1/spirv.h>
#include "spirv-simulator/framework/spirv_simulator.hpp"

#include "execute_commands.h"

#include "read_auto.h"
#include "read.h"
#include "util_auto.h"
#include "suballocator.h"

static bool run_spirv(const trackeddevice& device_data, const shader_stage& stage, const std::vector<std::byte>& push_constants,
	const std::unordered_map<uint32_t, std::unordered_map<uint32_t, buffer_access>>& descriptorsets,
	const std::unordered_map<uint32_t, std::unordered_map<uint32_t, image_access>>& imagesets,
	const std::vector<uint32_t>* code_override = nullptr)
{
	const std::vector<uint32_t>* code_ptr = code_override;
	trackedshadermodule* shader_data_ptr = nullptr;
	if (!code_ptr)
	{
		assert(stage.module != VK_NULL_HANDLE);
		const uint32_t shader_index = index_to_VkShaderModule.index(stage.module);
		shader_data_ptr = &VkShaderModule_index.at(shader_index);
		code_ptr = &shader_data_ptr->code;
	}
	assert(code_ptr);
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
			if (!access.buffer_data->candidates.empty() && access.size != 0)
			{
				const VkDeviceSize binding_start = access.offset;
				const VkDeviceSize binding_end = access.offset + access.size;
				std::vector<SPIRVSimulator::PhysicalAddressCandidate>* candidate_list = nullptr;
				for (const auto& candidate : access.buffer_data->candidates)
				{
					if (candidate.offset < binding_start || candidate.offset >= binding_end) continue;
					if (!candidate_list) candidate_list = &inputs.candidates[static_cast<const void*>(binding_ptr)];
					SPIRVSimulator::PhysicalAddressCandidate sim_candidate;
					sim_candidate.address = candidate.address;
					sim_candidate.offset = candidate.offset - binding_start;
					sim_candidate.payload = access.buffer_data;
					candidate_list->push_back(sim_candidate);
				}
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
	if (shader_data_ptr) shader_data_ptr->calls++;
	SPIRVSimulator::SPIRVSimulator sim(*code_ptr, &inputs, &results, nullptr, false, ERROR_RAISE_ON_BUFFERS_INCOMPLETE);
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

bool execute_commands(lava_file_reader& reader, const trackeddevice& device_data, VkCommandBuffer commandBuffer)
{
	std::vector<std::byte> push_constants; // current state of the push constants
	uint32_t compute_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t compute_shader_object_bound = CONTAINER_INVALID_INDEX; // index into compute_shader_objects array
	trackedshaderobject* compute_shader_objects = nullptr;
	uint32_t compute_shader_objects_count = 0;
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
				for (uint32_t i = 0; i < c.data.copy_buffer.regionCount; i++)
				{
					VkBufferCopy& r = c.data.copy_buffer.pRegions[i];
					memcpy((char*)dst.memory + r.dstOffset, (char*)src.memory + r.srcOffset, r.size);
				}
			}
			free(c.data.copy_buffer.pRegions);
			break;
		case VKCMDUPDATEBUFFER:
			{
				suballoc_location sub = device_data.allocator->find_buffer_memory(c.data.update_buffer.buffer_index);
				memcpy((char*)sub.memory + c.data.update_buffer.offset, c.data.update_buffer.values, c.data.update_buffer.size);
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
				compute_shader_object_bound = CONTAINER_INVALID_INDEX;
			}
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) raytracing_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			break;
		case VKCMDPUSHDESCRIPTORSETKHR:
			assert(false); // TBD
			break;
		case VKCMDBINDSHADERSEXT:
			if (compute_shader_objects) delete[] compute_shader_objects;
			compute_shader_objects = c.data.bind_shaders_ext.shader_objects;
			compute_shader_objects_count = c.data.bind_shaders_ext.stageCount;
			compute_shader_object_bound = CONTAINER_INVALID_INDEX;
			for (uint32_t i = 0; i < compute_shader_objects_count; i++)
			{
				if (compute_shader_objects[i].stage == VK_SHADER_STAGE_COMPUTE_BIT)
				{
					compute_shader_object_bound = i; // index into current array
				}
			}
			break;
		case VKCMDDISPATCH: // proxy for all compute commands
			{
				if (compute_pipeline_bound != CONTAINER_INVALID_INDEX)
				{
					const auto& pipeline_data = VkPipeline_index.at(compute_pipeline_bound);
					assert(pipeline_data.shader_stages.size() == 1);
					assert(pipeline_data.shader_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
					run_spirv(device_data, pipeline_data.shader_stages[0], push_constants, descriptorsets, imagesets);
				}
				else
				{
					assert(compute_shader_object_bound != CONTAINER_INVALID_INDEX);
					const auto& so = compute_shader_objects[compute_shader_object_bound];
					shader_stage stage;
					stage.index = 0;
					stage.flags = so.flags;
					stage.stage = so.stage;
					stage.module = VK_NULL_HANDLE;
					stage.name = so.name;
					stage.specialization_constants = so.specialization_constants;
					stage.specialization_data = so.specialization_data;
					run_spirv(device_data, stage, push_constants, descriptorsets, imagesets, &so.code);
				}
			}
			break;
		case VKCMDDRAW: // proxy for all draw commands
			{
				const auto& pipeline_data = VkPipeline_index.at(graphics_pipeline_bound);
				for (const auto& stage : pipeline_data.shader_stages)
				{
					run_spirv(device_data, stage, push_constants, descriptorsets, imagesets);
				}
			}
			break;
		case VKCMDTRACERAYSKHR: // proxy for all raytracing commands
			//auto& pipeline_data = VkPipeline_index.at(raytracing_pipeline_bound);
			//TBD
			(void)raytracing_pipeline_bound;
			break;
		default:
			break;
		}
	}
	cmdbuffer_data.commands.clear();
	if (compute_shader_objects) delete[] compute_shader_objects;
	return true;
}
