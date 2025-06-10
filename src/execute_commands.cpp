#if 0
static bool run_spirv(lava_file_reader& reader, const shader_stage& stage, const std::vector<uint8_t>& push_constants)
{
	const uint32_t shader_index = index_to_VkShaderModule.index(stage.module);
	trackedshadermodule& shader_data = VkShaderModule_index.at(shader_index);
	std::unordered_map<std::string, uint32_t> functions; // function name -> index in spirv
	std::unordered_map<std::string, uint64_t> globals; // store everything as an uint64
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
#endif

static bool run_spirv(lava_file_reader& reader, const shader_stage& stage, const std::vector<uint8_t>& push_constants)
{
	const uint32_t shader_index = index_to_VkShaderModule.index(stage.module);
	trackedshadermodule& shader_data = VkShaderModule_index.at(shader_index);

	SPIRVSimulator::InputData inputs;
	SPIRVSimulator::SPIRVSimulator sim(shader_data.code, inputs, true);
	sim.Run();

	auto physical_address_data = sim.GetPhysicalAddressData();

	std::cout << "Pointers to pbuffers:" << std::endl;
	for (const auto& pointer_t : physical_address_data)
	{
		std::cout << "  Found pointer with address: 0x" << std::hex << pointer_t.raw_pointer_value << std::dec << " made from input bit components:" << std::endl;
		for (auto bit_component : pointer_t.bit_components)
		{
			if (bit_component.location == SPIRVSimulator::BitLocation::Constant)
			{
				std::cout << "    " << "From Constant in SPIRV input words, at Byte Offset: " << bit_component.byte_offset << std::endl;
			} else {
				if (bit_component.location == SPIRVSimulator::BitLocation::SpecConstant)
				{
					std::cout << "    " << "From SpecId: " << bit_component.binding_id;
				} else {
					std::cout << "    " << "From DescriptorSetID: " << bit_component.set_id << ", Binding: " << bit_component.binding_id;
				}
				if (bit_component.location == SPIRVSimulator::BitLocation::StorageClass)
				{
					std::cout << ", in StorageClass: " << spv::StorageClassToString(bit_component.storage_class);
				}
				std::cout << ", Byte Offset: " << bit_component.byte_offset << ", Bitsize: " << bit_component.bitcount << ", to val Bit Offset: " << bit_component.val_bit_offset << std::endl;
			}
		}
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
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, buffer_access>> descriptorsets; // descriptorset binding : set internal binding point : buffer
	for (const auto& c : cmdbuffer_data.commands)
	{
		switch (c.id)
		{
		case VKCMDBINDDESCRIPTORSETS:
			for (uint32_t i = 0; i < c.data.bind_descriptorsets.descriptorSetCount; i++)
			{
				const uint32_t pipeline_index = c.data.bind_descriptorsets.pipelineBindPoint;
				uint32_t set = c.data.bind_descriptorsets.firstSet + i;
				auto& tds = VkDescriptorSet_index.at(c.data.bind_descriptorsets.pDescriptorSets[i]); // is index now
				for (auto pair : tds.bound_buffers)
				{
					descriptorsets[set][pair.first] = pair.second;
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
				suballoc_location src = suballoc_find_buffer_memory(c.data.copy_buffer.src_buffer_index);
				suballoc_location dst = suballoc_find_buffer_memory(c.data.copy_buffer.dst_buffer_index);
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
				suballoc_location sub = suballoc_find_buffer_memory(c.data.update_buffer.buffer_index);
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
