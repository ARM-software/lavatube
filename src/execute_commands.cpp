#include <spirv/unified1/spirv.h>

static bool run_spirv(lava_file_reader& reader, const shader_stage& stage, const std::vector<uint8_t>& push_constants)
{
	const uint32_t shader_index = index_to_VkShaderModule.index(stage.module);
	trackedshadermodule& shader_data = VkShaderModule_index.at(shader_index);
	std::unordered_map<std::string, uint32_t> functions; // function name -> index in spirv
	std::unordered_map<std::string, uint64_t> globals; // store everything as a uint64
	std::vector<std::unordered_map<std::string, uint64_t>> locals; // stack

	assert(shader_data.code[0] == SpvMagicNumber);
	uint32_t id = shader_data.code[3];
	const uint32_t* insn = shader_data.code.data() + 5;
	int count = 0;
	const unsigned code_size = shader_data.code.size() / 4;
	while (insn != shader_data.code.data() + code_size)
	{
		const uint16_t opcode = uint16_t(insn[0]);
		const uint16_t word_count = uint16_t(insn[0] >> 16);

		count++;
		switch (opcode)
		{
		default:
			break;
		}

		assert(insn + word_count <= shader_data.code.data() + code_size);
		insn += word_count;
	}
	return true;
}

static bool execute_commands(lava_file_reader& reader, VkCommandBuffer commandBuffer)
{
	std::vector<uint8_t> push_constants; // current state of the push constants
	uint32_t compute_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t graphics_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t raytracing_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t cmdbuffer_index = index_to_VkCommandBuffer.index(commandBuffer);
	auto& cmdbuffer_data = VkCommandBuffer_index.at(cmdbuffer_index);
	for (const auto& c : cmdbuffer_data.commands)
	{
		switch (c.id)
		{
		case VKCMDCOPYBUFFER:
			// TBD
			free(c.data.copy_buffer.pRegions);
			break;
		case VKCMDUPDATEBUFFER:
			// TBD
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
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) compute_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) raytracing_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			break;
		case VKCMDDISPATCH: // proxy for all compute commands
			{
				const auto& pipeline_data = VkPipeline_index.at(compute_pipeline_bound);
				assert(pipeline_data.shader_stages.size() == 1);
				assert(pipeline_data.shader_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
				run_spirv(reader, pipeline_data.shader_stages[0], push_constants);
			}
			break;
		case VKCMDDRAW: // proxy for all draw commands
			{
				const auto& pipeline_data = VkPipeline_index.at(graphics_pipeline_bound);
				for (const auto& stage : pipeline_data.shader_stages)
				{
					run_spirv(reader, stage, push_constants);
				}
			}
			break;
		case VKCMDTRACERAYSKHR: // proxy for all raytracing commands
			//auto& pipeline_data = VkPipeline_index.at(raytracing_pipeline_bound);
			//TBD
			break;
		default:
			break;
		}
	}
	cmdbuffer_data.commands.clear();
	return true;
}
