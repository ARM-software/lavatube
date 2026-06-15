#define SPV_ENABLE_UTILITY_CODE 1
#include <spirv/unified1/spirv.h>
#include <algorithm>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include "spirv-simulator/framework/spirv_simulator.hpp"

#include "execute_commands.h"

#include "read_auto.h"
#include "markings.h"
#include "suballocator.h"

struct simulator_buffer_range
{
	const void* base_ptr = nullptr;
	size_t size = 0;
	trackedbuffer* buffer_data = nullptr;
	VkDeviceSize buffer_offset = 0;
	const host_write_regions* source_regions = nullptr;
	uint32_t set = UINT32_MAX;
	uint32_t binding = UINT32_MAX;
	bool physical_address_backing = false;
	VkObjectType source_object_type = VK_OBJECT_TYPE_UNKNOWN;
	uint32_t source_object_index = CONTAINER_NULL_VALUE;
	uint32_t source_stage_index = CONTAINER_NULL_VALUE;
};

struct discovered_buffer_marking
{
	trackedbuffer* buffer_data = nullptr;
	VkObjectType output_object_type = VK_OBJECT_TYPE_UNKNOWN;
	uint32_t output_object_index = CONTAINER_NULL_VALUE;
	uint32_t output_stage_index = CONTAINER_NULL_VALUE;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	VkMarkingTypeARM type = (VkMarkingTypeARM)0;
	VkMarkingSubTypeARM subtype{};
	bool has_explicit_source = false;
	change_source source;
};

static void merge_discovered_markings(command_execution_data& data, const std::vector<discovered_buffer_marking>& discovered);

static SPIRVSimulator::InternalPersistentData simulator_persistent_data;

static uint64_t simulator_pointer_bits(const void* ptr)
{
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

static void register_simulator_buffer_range(std::vector<simulator_buffer_range>& ranges, const void* base_ptr, size_t size,
	trackedbuffer* buffer_data, VkDeviceSize buffer_offset, uint32_t set = UINT32_MAX, uint32_t binding = UINT32_MAX, bool physical_address_backing = false,
	const host_write_regions* source_regions = nullptr, VkObjectType source_object_type = VK_OBJECT_TYPE_UNKNOWN, uint32_t source_object_index = CONTAINER_NULL_VALUE,
	uint32_t source_stage_index = CONTAINER_NULL_VALUE)
{
	if (!base_ptr || (!buffer_data && !source_regions) || size == 0) return;
	for (const simulator_buffer_range& existing : ranges)
	{
		if (existing.base_ptr == base_ptr && existing.size == size && existing.buffer_data == buffer_data
			&& existing.buffer_offset == buffer_offset && existing.source_regions == source_regions && existing.physical_address_backing == physical_address_backing
			&& existing.source_object_type == source_object_type && existing.source_object_index == source_object_index && existing.source_stage_index == source_stage_index)
		{
			return;
		}
	}
	ranges.push_back({
		.base_ptr = base_ptr,
		.size = size,
		.buffer_data = buffer_data,
		.buffer_offset = buffer_offset,
		.source_regions = source_regions,
		.set = set,
		.binding = binding,
		.physical_address_backing = physical_address_backing,
		.source_object_type = source_object_type,
		.source_object_index = source_object_index,
		.source_stage_index = source_stage_index,
	});
}

static const simulator_buffer_range* find_simulator_range(const std::vector<simulator_buffer_range>& ranges, const void* source_ptr,
	VkDeviceSize byte_offset, VkDeviceSize size, VkDeviceSize& out_offset)
{
	if (!source_ptr || size == 0) return nullptr;
	const std::byte* source = static_cast<const std::byte*>(source_ptr);
	for (const simulator_buffer_range& range : ranges)
	{
		const std::byte* base = static_cast<const std::byte*>(range.base_ptr);
		const std::byte* end = base + range.size;
		if (source < base || source > end) continue;
		const VkDeviceSize local = (VkDeviceSize)(source - base) + byte_offset;
		if (local + size > range.size) continue;
		out_offset = local;
		return &range;
	}
	return nullptr;
}

static const simulator_buffer_range* find_simulator_storage_class_range(const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::DataSourceBits& source, VkDeviceSize size, VkDeviceSize& out_offset)
{
	if (size == 0) return nullptr;
	if (source.storage_class == spv::StorageClassPushConstant)
	{
		for (const simulator_buffer_range& range : ranges)
		{
			if (!range.source_regions) continue;
			if (source.byte_offset > range.size || size > range.size - source.byte_offset) continue;
			out_offset = source.byte_offset;
			return &range;
		}
	}
	else if (source.storage_class == spv::StorageClassUniform
		|| source.storage_class == spv::StorageClassStorageBuffer
		|| source.storage_class == spv::StorageClassUniformConstant)
	{
		for (const simulator_buffer_range& range : ranges)
		{
			if (!range.buffer_data) continue;
			if (range.set != source.set_id || range.binding != source.binding_id) continue;
			if (source.byte_offset > range.size || size > range.size - source.byte_offset) continue;
			out_offset = source.byte_offset;
			return &range;
		}
	}
	return nullptr;
}

static const simulator_buffer_range* find_simulator_source_range(const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::DataSourceBits& source, VkDeviceSize size, VkDeviceSize& out_offset)
{
	if (source.location == SPIRVSimulator::SpecConstant)
	{
		return find_simulator_range(ranges, source.source_ptr, source.byte_offset, size, out_offset);
	}
	if (source.location == SPIRVSimulator::StorageClass)
	{
		const simulator_buffer_range* range = find_simulator_range(ranges, source.source_ptr, source.byte_offset, size, out_offset);
		if (!range) range = find_simulator_storage_class_range(ranges, source, size, out_offset);
		return range;
	}
	return nullptr;
}

static bool get_range_source_reference(const simulator_buffer_range& range, VkDeviceSize local_offset, VkDeviceSize size, host_write_reference& out)
{
	if (range.source_regions)
	{
		return range.source_regions->try_get_reference(range.buffer_offset + local_offset, size, out);
	}
	if (range.buffer_data)
	{
		return range.buffer_data->source.try_get_reference(range.buffer_offset + local_offset, size, out);
	}
	return false;
}

static void abort_missing_range_source(const simulator_buffer_range& range, VkDeviceSize local_offset, VkDeviceSize size, const char* context)
{
	if (range.buffer_data)
	{
		ABORT("%s needs host-write provenance for %s[%u] offset=%llu size=%llu, but coverage is missing or spans multiple sources",
			context, pretty_print_VkObjectType(range.buffer_data->object_type), range.buffer_data->index,
			(unsigned long long)(range.buffer_offset + local_offset), (unsigned long long)size);
	}
	ABORT("%s needs host-write provenance for simulator range offset=%llu size=%llu, but coverage is missing or spans multiple sources",
		context, (unsigned long long)(range.buffer_offset + local_offset), (unsigned long long)size);
}

static bool collect_contiguous_device_address_marking(const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::PhysicalAddressData& pointer_data, std::vector<discovered_buffer_marking>& discovered)
{
	struct source_component
	{
		const simulator_buffer_range* range = nullptr;
		VkDeviceSize local_offset = 0;
		uint64_t bitcount = 0;
		uint64_t val_bit_offset = 0;
	};

	std::vector<source_component> components;
	for (const SPIRVSimulator::DataSourceBits& source : pointer_data.bit_components)
	{
		if (source.location != SPIRVSimulator::StorageClass && source.location != SPIRVSimulator::SpecConstant) continue;
		if (source.bit_offset != 0 || source.bitcount == 0 || source.bitcount % 8 != 0 || source.val_bit_offset % 8 != 0) continue;
		if (source.val_bit_offset >= sizeof(VkDeviceAddress) * 8) continue;
		if (source.val_bit_offset + source.bitcount > sizeof(VkDeviceAddress) * 8) continue;
		VkDeviceSize local_offset = 0;
		const simulator_buffer_range* range = find_simulator_source_range(ranges, source, source.bitcount / 8, local_offset);
		if (!range) continue;
		components.push_back({
			.range = range,
			.local_offset = local_offset,
			.bitcount = source.bitcount,
			.val_bit_offset = source.val_bit_offset,
		});
	}
	if (components.empty()) return false;
	std::sort(components.begin(), components.end(), [](const source_component& a, const source_component& b)
	{
		return a.val_bit_offset < b.val_bit_offset;
	});

	const simulator_buffer_range* range = components.front().range;
	const VkDeviceSize base_offset = components.front().local_offset;
	uint64_t covered_bits = 0;
	for (const source_component& component : components)
	{
		if (component.range != range) return false;
		if (component.val_bit_offset != covered_bits) return false;
		const VkDeviceSize expected_offset = base_offset + component.val_bit_offset / 8;
		if (component.local_offset != expected_offset) return false;
		covered_bits += component.bitcount;
	}
	if (covered_bits != sizeof(VkDeviceAddress) * 8) return false;

	host_write_reference source_ref;
	if (!get_range_source_reference(*range, base_offset, sizeof(VkDeviceAddress), source_ref))
	{
		abort_missing_range_source(*range, base_offset, sizeof(VkDeviceAddress), "SPIR-V physical-address source marking");
	}
	const VkObjectType output_object_type = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.object_type : range->source_object_type;
	const uint32_t output_object_index = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.object_index : range->source_object_index;
	const uint32_t output_stage_index = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.stage_index : range->source_stage_index;
	VkMarkingSubTypeARM subtype{};
	subtype.deviceAddressType = VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM;
	discovered.push_back({
		.buffer_data = range->buffer_data,
		.output_object_type = output_object_type,
		.output_object_index = output_object_index,
		.output_stage_index = output_stage_index,
		.offset = range->buffer_offset + base_offset,
		.size = sizeof(VkDeviceAddress),
		.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
		.subtype = subtype,
		.has_explicit_source = true,
		.source = source_ref.source,
	});
	return true;
}

static void merge_simulator_output_candidates(const shader_stage& stage, const change_source& source,
	const std::unordered_map<const void*, simulator_buffer_range>& range_lookup, const SPIRVSimulator::SimulationResults& results)
{
	for (const auto& candidates : results.output_candidates)
	{
		ILOG("Found set of %u candidates for %p", (unsigned)candidates.second.size(), candidates.first);
		const auto range_it = range_lookup.find(candidates.first);
		for (const auto& candidate : candidates.second)
		{
			if (range_it != range_lookup.end())
			{
				const simulator_buffer_range& range = range_it->second;
				if (range.set != UINT32_MAX && range.binding != UINT32_MAX)
				{
					ILOG("SPIRV candidate %s in %s set=%u binding=%u base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED", stage.name.c_str(),
						(unsigned)range.set, (unsigned)range.binding, candidates.first, (unsigned long)candidate.offset,
						(unsigned long long)candidate.address);
				}
				else if (range.physical_address_backing)
				{
					ILOG("SPIRV candidate %s in %s physical-address buffer=%u base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED",
						stage.name.c_str(), (unsigned)range.buffer_data->index, candidates.first, (unsigned long)candidate.offset,
						(unsigned long long)candidate.address);
				}
				else
				{
					ILOG("SPIRV candidate %s in %s buffer=%u base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED", stage.name.c_str(),
						(unsigned)range.buffer_data->index, candidates.first, (unsigned long)candidate.offset,
						(unsigned long long)candidate.address);
				}
				if (candidate.verified)
				{
					range.buffer_data->source.register_source(range.buffer_offset + candidate.offset, sizeof(uint64_t), source,
						1, 0, range.buffer_data->object_type, range.buffer_data->index);
				}
			}
			else
			{
				ILOG("SPIRV candidate %s in %s base=%p offset=%lu address=0x%llx", candidate.verified ? "verified" : "UNVERIFIED", stage.name.c_str(), candidates.first,
					(unsigned long)candidate.offset, (unsigned long long)candidate.address);
			}
		}
	}
}

static void copy_simulator_source_span(const simulator_buffer_range& dst_range, const simulator_buffer_range& src_range,
	VkDeviceSize dst_offset, VkDeviceSize src_offset, VkDeviceSize size)
{
	if (!dst_range.buffer_data || size == 0) return;
	const host_write_regions* src_sources = src_range.source_regions;
	if (!src_sources && src_range.buffer_data) src_sources = &src_range.buffer_data->source;
	if (!src_sources) return;
	dst_range.buffer_data->source.copy_sources(*src_sources, dst_range.buffer_offset + dst_offset, src_range.buffer_offset + src_offset, size);
}

static void merge_simulator_source_copies(const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::MemoryFlagTracker& memory_flag_tracker)
{
	for (const simulator_buffer_range& dst_range : ranges)
	{
		if (!dst_range.buffer_data) continue;
		const uint64_t dst_base = simulator_pointer_bits(dst_range.base_ptr);
		const auto spans = memory_flag_tracker.queryRangeDetailed(dst_base, dst_range.size);
		for (const auto& span : spans)
		{
			if (span.origin != SPIRVSimulator::MemoryFlagTracker::MappingOrigin::Copy) continue;
			const uint64_t dst_start = std::max<uint64_t>(span.start, dst_base);
			const uint64_t dst_end = std::min<uint64_t>(span.end, dst_base + dst_range.size);
			if (dst_start >= dst_end) continue;

			uint64_t copied = 0;
			while (copied < dst_end - dst_start)
			{
				const uint64_t src_address = span.root_address + (dst_start - span.start) + copied;
				const VkDeviceSize remaining = (VkDeviceSize)(dst_end - dst_start - copied);
				VkDeviceSize src_offset = 0;
				const simulator_buffer_range* src_range = find_simulator_range(ranges,
					reinterpret_cast<const void*>(static_cast<uintptr_t>(src_address)), 0, 1, src_offset);
				if (!src_range) break;
				const VkDeviceSize copy_size = std::min<VkDeviceSize>(remaining, src_range->size - src_offset);
				copy_simulator_source_span(dst_range, *src_range, (VkDeviceSize)(dst_start - dst_base + copied), src_offset, copy_size);
				copied += copy_size;
			}
		}
	}
}

static void merge_simulator_memory_metadata(const change_source& source, const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::MemoryFlagTracker& memory_flag_tracker)
{
	for (const simulator_buffer_range& range : ranges)
	{
		if (!range.buffer_data) continue;
		const uint64_t base = simulator_pointer_bits(range.base_ptr);
		const auto spans = memory_flag_tracker.queryRangeDetailed(base, range.size);
		for (const auto& span : spans)
		{
			if ((span.flags & SPS_FLAG_IS_PBUFFER_PTR) == 0) continue;
			const uint64_t start = std::max<uint64_t>(span.start, base);
			const uint64_t end = std::min<uint64_t>(span.end, base + range.size);
			if (start >= end) continue;
			range.buffer_data->source.register_source(range.buffer_offset + (start - base), end - start, source,
				1, 0, range.buffer_data->object_type, range.buffer_data->index);
		}
	}
}

static void collect_simulator_physical_address_markings(const std::vector<simulator_buffer_range>& ranges,
	const SPIRVSimulator::SimulationResults& results, std::vector<discovered_buffer_marking>& discovered)
{
	VkMarkingSubTypeARM subtype{};
	subtype.deviceAddressType = VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM;
	for (const SPIRVSimulator::PhysicalAddressData& pointer_data : results.physical_address_data)
	{
		bool found_source = false;
		for (const SPIRVSimulator::DataSourceBits& source : pointer_data.bit_components)
		{
			if (source.location != SPIRVSimulator::StorageClass && source.location != SPIRVSimulator::SpecConstant) continue;
			if (source.bit_offset != 0 || source.val_bit_offset != 0 || source.bitcount != sizeof(VkDeviceAddress) * 8) continue;
			VkDeviceSize local_offset = 0;
			const simulator_buffer_range* range = find_simulator_source_range(ranges, source, sizeof(VkDeviceAddress), local_offset);
			if (!range) continue;
			host_write_reference source_ref;
			if (!get_range_source_reference(*range, local_offset, sizeof(VkDeviceAddress), source_ref))
			{
				abort_missing_range_source(*range, local_offset, sizeof(VkDeviceAddress), "SPIR-V physical-address source marking");
			}
			const VkObjectType output_object_type = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.object_type : range->source_object_type;
			const uint32_t output_object_index = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.object_index : range->source_object_index;
			const uint32_t output_stage_index = (source_ref.object_type != VK_OBJECT_TYPE_UNKNOWN) ? source_ref.stage_index : range->source_stage_index;
			discovered.push_back({
				.buffer_data = range->buffer_data,
				.output_object_type = output_object_type,
				.output_object_index = output_object_index,
				.output_stage_index = output_stage_index,
				.offset = range->buffer_offset + local_offset,
				.size = sizeof(VkDeviceAddress),
				.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
				.subtype = subtype,
				.has_explicit_source = true,
				.source = source_ref.source,
			});
			found_source = true;
		}
		if (!found_source) found_source = collect_contiguous_device_address_marking(ranges, pointer_data, discovered);
	}
}

static bool run_spirv(command_execution_data& data, const shader_stage& stage, const change_source& source)
{
	SPIRVSimulator::SimulationData inputs;
	SPIRVSimulator::SimulationResults results;
	std::deque<uint64_t> opaque_storage;
	std::vector<simulator_buffer_range> simulator_ranges;
	std::unordered_map<const void*, simulator_buffer_range> range_lookup;
	inputs.push_constants = data.push_constants.empty() ? nullptr : data.push_constants.data();
	if (!data.push_constants.empty())
	{
		register_simulator_buffer_range(simulator_ranges, data.push_constants.data(), data.push_constants.size(), nullptr, 0, UINT32_MAX, UINT32_MAX, false, &data.push_constant_sources);
		inputs.rt_array_lengths[simulator_pointer_bits(data.push_constants.data())][0] = data.push_constants.size();
	}
	inputs.entry_point_op_name = stage.name;
	inputs.specialization_constants = stage.specialization_data.empty() ? nullptr : stage.specialization_data.data();
	if (!stage.specialization_data.empty() && stage.specialization_sources_valid)
	{
		register_simulator_buffer_range(simulator_ranges, stage.specialization_data.data(), stage.specialization_data.size(), nullptr, 0,
			UINT32_MAX, UINT32_MAX, false, &stage.specialization_sources, stage.specialization_source_object_type, stage.specialization_source_object_index,
			stage.specialization_source_stage_index);
	}
	for (const auto& v : stage.specialization_constants)
	{
		inputs.specialization_constant_offsets[v.constantID] = v.offset;
	}
	for (const auto& set_pair : data.descriptorsets)
	{
		auto& set_bindings = inputs.bindings[set_pair.first];
		for (const auto& binding_pair : set_pair.second)
		{
			const buffer_access& access = binding_pair.second;
			if (!access.buffer_data) continue;
			const uint32_t buffer_index = access.buffer_data->index;
			suballoc_location loc = data.device_data.allocator->find_buffer_memory(buffer_index);
			assert(loc.mapped);
			std::byte* base = (std::byte*)loc.mapped;
			std::byte* binding_ptr = base + access.offset;
			set_bindings[binding_pair.first] = binding_ptr;
			const VkDeviceSize binding_size = (access.size != 0) ? access.size : (access.buffer_data->size - access.offset);
			register_simulator_buffer_range(simulator_ranges, binding_ptr, binding_size, access.buffer_data, access.offset, set_pair.first, binding_pair.first);
			// Provide real runtime-array length information to the SPIR-V simulator
			if (binding_size > 0)
			{
				inputs.rt_array_lengths[simulator_pointer_bits(binding_ptr)][0] = static_cast<size_t>(binding_size);
			}
			if ((access.buffer_data->usage2 & VK_BUFFER_USAGE_2_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
			{
				inputs.descriptor_candidates[binding_ptr];
			}
		}
	}
	for (const auto& set_pair : data.imagesets)
	{
		auto& set_bindings = inputs.bindings[set_pair.first];
		for (const auto& binding_pair : set_pair.second)
		{
			if (set_bindings.count(binding_pair.first) > 0) continue;
			const image_access& access = binding_pair.second;
			void* binding_ptr = nullptr;
			if (access.image_data)
			{
				suballoc_location loc = data.device_data.allocator->find_image_memory(access.image_data->index);
				assert(loc.mapped);
				std::byte* base = (std::byte*)loc.mapped;
				binding_ptr = base;
			}
			else
			{
				static uint64_t dummy_descriptor = 0;
				binding_ptr = &dummy_descriptor;
			}
			set_bindings[binding_pair.first] = binding_ptr;
		}
	}
	for (const auto& set_pair : data.opaquesets)
	{
		auto& set_bindings = inputs.bindings[set_pair.first];
		for (const auto& binding_pair : set_pair.second)
		{
			if (set_bindings.count(binding_pair.first) > 0) continue;
			opaque_storage.push_back(binding_pair.second ? binding_pair.second : 1);
			set_bindings[binding_pair.first] = &opaque_storage.back();
		}
	}

	for (trackedbuffer& buffer_data : VkBuffer_index)
	{
		if (buffer_data.parent_device_index != data.device_data.index) continue;
		if (buffer_data.size == 0) continue;
		const uint64_t visible_address = buffer_data.capture_device_address ? buffer_data.capture_device_address : buffer_data.device_address;
		if (visible_address == 0) continue;
		if (!buffer_data.is_state(trackedobject::states::bound) && data.device_address_remapping.get_by_address(visible_address) != &buffer_data) continue;
		suballoc_location loc = data.device_data.allocator->find_buffer_memory(buffer_data.index);
		if (!loc.mapped) continue;
		std::byte* base = (std::byte*)loc.mapped;
		inputs.physical_address_buffers[visible_address] = std::make_pair(static_cast<size_t>(buffer_data.size), base);
		inputs.rt_array_lengths[simulator_pointer_bits(base)][0] = static_cast<size_t>(buffer_data.size);
		register_simulator_buffer_range(simulator_ranges, base, buffer_data.size, &buffer_data, 0, UINT32_MAX, UINT32_MAX, true);
		if ((buffer_data.usage2 & VK_BUFFER_USAGE_2_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		{
			inputs.descriptor_candidates[base];
		}
	}

	for (const simulator_buffer_range& range : simulator_ranges)
	{
		range_lookup.emplace(range.base_ptr, range);
	}

	inputs.shader_id = stage.unique_index;
	SPIRVSimulator::MemoryFlagTracker memory_flag_tracker;
	const uint64_t simulator_init_start = gettime();
	SPIRVSimulator::SPIRVSimulator sim(stage.code, &memory_flag_tracker, &inputs, &results, &simulator_persistent_data, false, ERROR_RAISE_ON_BUFFERS_INCOMPLETE);
	const uint64_t simulator_run_start = gettime();
	sim.Run();
	const uint64_t simulator_run_time_ns = gettime() - simulator_run_start;
	data.stats.total_spirv_run_time += simulator_run_time_ns;
	data.stats.total_init_time += simulator_run_start - simulator_init_start;
	if (simulator_run_time_ns > data.stats.slowest.run_time_ns)
	{
		data.stats.slowest.run_time_ns = simulator_run_time_ns;
		data.stats.slowest.stage = stage.stage;
		data.stats.slowest.shader_module_index = stage.shader_module_index;
	}

	if (results.full_dispatch_needed)
	{
		DLOG("SPIRV simulator requested full dispatch for shader %s", stage.name.c_str());
	}
	if (results.aborted_long_loop)
	{
		DLOG("SPIRV simulator aborted long loop while executing shader %s", stage.name.c_str());
	}
	if (!results.physical_address_data.empty())
	{
		DLOG("SPIRV simulator found %u physical-address value chains in shader %s", (unsigned)results.physical_address_data.size(), stage.name.c_str());
	}

	std::vector<discovered_buffer_marking> discovered_markings;
	merge_simulator_source_copies(simulator_ranges, memory_flag_tracker);
	collect_simulator_physical_address_markings(simulator_ranges, results, discovered_markings);
	merge_discovered_markings(data, discovered_markings);
	merge_simulator_output_candidates(stage, source, range_lookup, results);
	merge_simulator_memory_metadata(source, simulator_ranges, memory_flag_tracker);

	return true;
}

struct mapped_address_range
{
	std::byte* ptr = nullptr;
	VkDeviceSize size = 0;
	VkDeviceSize buffer_offset = 0;
	trackedbuffer* buffer_data = nullptr;
};

static bool map_device_address_range(const command_execution_data& data, VkDeviceAddress address, VkDeviceSize size, mapped_address_range& out, const char* label)
{
	if (address == 0 || size == 0) return false;
	trackedobject* obj = data.device_address_remapping.get_by_address(address);
	if (!obj || obj->object_type != VK_OBJECT_TYPE_BUFFER)
	{
		DLOG("Ray tracing %s address 0x%llx is not a buffer", label, (unsigned long long)address);
		return false;
	}
	trackedbuffer* buffer_data = static_cast<trackedbuffer*>(obj);
	const uint64_t translated = data.device_address_remapping.translate_address(address);
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
	suballoc_location loc = data.device_data.allocator->find_buffer_memory(buffer_data->index);
	assert(loc.mapped);
	std::byte* base = (std::byte*)loc.mapped;
	out.ptr = base + offset;
	out.size = size;
	out.buffer_offset = offset;
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

struct discovered_output_markings_bucket
{
	change_source source;
	VkObjectType object_type = VK_OBJECT_TYPE_UNKNOWN;
	uint32_t object_index = CONTAINER_NULL_VALUE;
	uint32_t stage_index = CONTAINER_NULL_VALUE;
	std::vector<discovered_buffer_marking> entries;
};

static VkShaderGroupShaderKHR get_shader_group_handle_marking_subtype(const trackedpipeline& pipeline_data, uint32_t group_index)
{
	if (group_index >= pipeline_data.raytracing_groups.size()) return VK_SHADER_GROUP_SHADER_GENERAL_KHR;
	const raytracing_group& group = pipeline_data.raytracing_groups[group_index];
	switch (group.type)
	{
	case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
		return VK_SHADER_GROUP_SHADER_GENERAL_KHR;
	case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
		if (group.closest_hit_shader != VK_SHADER_UNUSED_KHR) return VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR;
		if (group.any_hit_shader != VK_SHADER_UNUSED_KHR) return VK_SHADER_GROUP_SHADER_ANY_HIT_KHR;
		break;
	case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
		if (group.intersection_shader != VK_SHADER_UNUSED_KHR) return VK_SHADER_GROUP_SHADER_INTERSECTION_KHR;
		if (group.closest_hit_shader != VK_SHADER_UNUSED_KHR) return VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR;
		if (group.any_hit_shader != VK_SHADER_UNUSED_KHR) return VK_SHADER_GROUP_SHADER_ANY_HIT_KHR;
		break;
	default:
		break;
	}
	return VK_SHADER_GROUP_SHADER_GENERAL_KHR;
}

static VkMarkedOffsetsARM* build_marked_offsets(const std::vector<discovered_buffer_marking>& entries)
{
	assert(!entries.empty());
	VkMarkedOffsetsARM* markings = (VkMarkedOffsetsARM*)malloc(sizeof(VkMarkedOffsetsARM));
	if (!markings) ABORT("Failed to allocate discovered VkMarkedOffsetsARM");
	memset(markings, 0, sizeof(*markings));
	markings->sType = VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM;
	markings->count = entries.size();
	VkMarkingTypeARM* types = (VkMarkingTypeARM*)malloc(sizeof(VkMarkingTypeARM) * markings->count);
	VkMarkingSubTypeARM* subtypes = (VkMarkingSubTypeARM*)malloc(sizeof(VkMarkingSubTypeARM) * markings->count);
	VkDeviceSize* offsets = (VkDeviceSize*)malloc(sizeof(VkDeviceSize) * markings->count);
	if (!types || !subtypes || !offsets) ABORT("Failed to allocate discovered VkMarkedOffsetsARM arrays");
	markings->pMarkingTypes = types;
	markings->pSubTypes = subtypes;
	markings->pOffsets = offsets;

	for (uint32_t i = 0; i < markings->count; i++)
	{
		types[i] = entries[i].type;
		subtypes[i] = entries[i].subtype;
		offsets[i] = entries[i].offset;
	}
	normalize_marked_offsets(markings);
	return markings;
}

static void merge_discovered_markings(command_execution_data& data, const std::vector<discovered_buffer_marking>& discovered)
{
	std::vector<discovered_output_markings_bucket> output_buckets;
	for (const discovered_buffer_marking& marking : discovered)
	{
		assert(marking.size > 0);
		assert(marking.buffer_data || marking.has_explicit_source);
		change_source source = marking.source;
		if (!marking.has_explicit_source && !marking.buffer_data->source.try_get_source(marking.offset, marking.size, source))
		{
			DLOG("Skipping discovered marking for %s[%u] offset=%llu because no source update packet covers it",
				pretty_print_VkObjectType(marking.buffer_data->object_type), marking.buffer_data->index,
				(unsigned long long)marking.offset);
			continue;
		}

		auto output_it = std::find_if(output_buckets.begin(), output_buckets.end(), [&](const discovered_output_markings_bucket& bucket)
		{
			const VkObjectType object_type = (marking.output_object_type != VK_OBJECT_TYPE_UNKNOWN)
				? marking.output_object_type : (marking.buffer_data ? marking.buffer_data->object_type : VK_OBJECT_TYPE_UNKNOWN);
			const uint32_t object_index = (marking.output_object_type != VK_OBJECT_TYPE_UNKNOWN)
				? marking.output_object_index : (marking.buffer_data ? marking.buffer_data->index : CONTAINER_NULL_VALUE);
			return same_change_source(bucket.source, source)
				&& bucket.object_type == object_type
				&& bucket.object_index == object_index
				&& bucket.stage_index == marking.output_stage_index;
		});
		if (output_it == output_buckets.end())
		{
			discovered_output_markings_bucket bucket;
			bucket.source = source;
			bucket.object_type = (marking.output_object_type != VK_OBJECT_TYPE_UNKNOWN)
				? marking.output_object_type : (marking.buffer_data ? marking.buffer_data->object_type : VK_OBJECT_TYPE_UNKNOWN);
			bucket.object_index = (marking.output_object_type != VK_OBJECT_TYPE_UNKNOWN)
				? marking.output_object_index : (marking.buffer_data ? marking.buffer_data->index : CONTAINER_NULL_VALUE);
			bucket.stage_index = marking.output_stage_index;
			bucket.entries.push_back(marking);
			output_buckets.push_back(std::move(bucket));
		}
		else
		{
			output_it->entries.push_back(marking);
		}
	}

	for (const discovered_output_markings_bucket& bucket : output_buckets)
	{
		VkMarkedOffsetsARM* markings = build_marked_offsets(bucket.entries);
		merge_rewrite_markings(data.global_output_rewrite_queue, bucket.source, markings, bucket.object_type, bucket.object_index, bucket.stage_index);
		free_marked_offsets(markings);
	}
}

static void collect_stages_from_sbt_region(const command_execution_data& data, const trackedpipeline& pipeline_data, const VkStridedDeviceAddressRegionKHR& region,
	const char* label, std::unordered_set<uint32_t>& stages, std::vector<discovered_buffer_marking>* discovered = nullptr)
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
	if (!map_device_address_range(data, region.deviceAddress, region.size, mapping, label)) return;
	const uint32_t record_count = (uint32_t)(region.size / stride);
	for (uint32_t i = 0; i < record_count; i++)
	{
		const std::byte* handle_ptr = mapping.ptr + (size_t)i * stride;
		const int group_index = find_raytracing_group_index(pipeline_data, handle_ptr);
		if (group_index < 0) continue;
		add_raytracing_group_stages(pipeline_data, (uint32_t)group_index, stages);
		if (!discovered) continue;
		VkMarkingSubTypeARM subtype{};
		subtype.shaderGroupType = get_shader_group_handle_marking_subtype(pipeline_data, (uint32_t)group_index);
		discovered->push_back({
			.buffer_data = mapping.buffer_data,
			.offset = mapping.buffer_offset + (VkDeviceSize)i * stride,
			.size = pipeline_data.raytracing_group_handle_size,
			.type = VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM,
			.subtype = subtype,
		});
	}
}

static void collect_acceleration_structure_instance_markings(const command_execution_data& data, VkDeviceAddress address,
	VkDeviceSize primitive_offset, uint32_t primitive_count, std::vector<discovered_buffer_marking>& discovered)
{
	if (address == 0 || primitive_count == 0) return;
	address += primitive_offset;
	const VkDeviceSize size = (VkDeviceSize)primitive_count * sizeof(VkAccelerationStructureInstanceKHR);
	mapped_address_range mapping;
	if (!map_device_address_range(data, address, size, mapping, "acceleration structure instances")) return;

	VkMarkingSubTypeARM subtype{};
	subtype.deviceAddressType = VK_DEVICE_ADDRESS_TYPE_ACCELERATION_STRUCTURE_ARM;
	for (uint32_t i = 0; i < primitive_count; i++)
	{
		const VkDeviceSize instance_offset = (VkDeviceSize)i * sizeof(VkAccelerationStructureInstanceKHR);
		const std::byte* instance_ptr = mapping.ptr + instance_offset;
		uint64_t reference = 0;
		memcpy(&reference, instance_ptr + offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference), sizeof(reference));
		if (reference == 0) continue;
		discovered.push_back({
			.buffer_data = mapping.buffer_data,
			.offset = mapping.buffer_offset + instance_offset + offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference),
			.size = sizeof(VkDeviceAddress),
			.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
			.subtype = subtype,
		});
	}
}

static bool read_acceleration_structure_build_range(const command_execution_data& data, VkDeviceAddress address, VkAccelerationStructureBuildRangeInfoKHR& range)
{
	if (address == 0) return false;
	mapped_address_range mapping;
	if (!map_device_address_range(data, address, sizeof(VkAccelerationStructureBuildRangeInfoKHR), mapping, "indirect acceleration structure build range"))
	{
		return false;
	}
	memcpy(&range, mapping.ptr, sizeof(range));
	return true;
}

struct descriptor_buffer_binding_state
{
	trackedbuffer* buffer_data = nullptr;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	VkBufferUsageFlags usage = 0;
};

static void record_descriptor_buffer_payload(const command_execution_data& data, uint32_t buffer_index, VkDeviceSize offset, VkDescriptorType type)
{
	trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);
	suballoc_location loc = data.device_data.allocator->find_buffer_memory(buffer_index);
	if (!loc.mapped) return;

	for (const descriptor_rewrite& payload : data.pending_descriptor_rewrites)
	{
		if (payload.type != type || payload.capture_bytes.empty()) continue;
		if (offset > buffer_data.size || payload.capture_bytes.size() > buffer_data.size - offset) continue;
		if (memcmp(loc.mapped + offset, payload.capture_bytes.data(), payload.capture_bytes.size()) != 0) continue;

		auto existing = std::find_if(data.descriptor_buffer_payloads.begin(), data.descriptor_buffer_payloads.end(),
			[&](const descriptor_buffer_payload& entry)
			{
				return entry.buffer_index == buffer_index && entry.offset == offset && entry.type == type && entry.bytes == payload.capture_bytes;
			});
		if (existing != data.descriptor_buffer_payloads.end()) return;

		data.descriptor_buffer_payloads.push_back({
			.buffer_index = buffer_index,
			.offset = offset,
			.type = type,
			.bytes = payload.capture_bytes,
		});
		return;
	}
}

bool execute_commands(command_execution_data& data)
{
	std::vector<std::byte> push_constants; // current state of the push constants
	host_write_regions push_constant_sources;
	uint32_t compute_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t data_graph_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	std::unordered_map<VkShaderStageFlagBits, uint32_t> shader_objects;
	uint32_t graphics_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	uint32_t raytracing_pipeline_bound = CONTAINER_INVALID_INDEX; // currently bound pipeline
	std::vector<descriptor_buffer_binding_state> descriptor_buffers;
	for (const auto& c : data.cmdbuffer_data.commands)
	{
		data.stats.commands++;
		switch (c.id)
		{
		case VKCMDBINDDESCRIPTORSETS:
			for (uint32_t i = 0; i < c.data.bind_descriptorsets.descriptorSetCount; i++)
			{
				uint32_t set = c.data.bind_descriptorsets.firstSet + i;
				auto& tds = VkDescriptorSet_index.at(c.data.bind_descriptorsets.pDescriptorSets[i]); // is index now
				for (auto pair : tds.bound_buffers)
				{
					data.descriptorsets[set][pair.first] = pair.second;
				}
				for (auto pair : tds.bound_images)
				{
					data.imagesets[set][pair.first] = pair.second;
				}
				for (auto pair : tds.bound_opaque_descriptors)
				{
					data.opaquesets[set][pair.first] = pair.second;
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
					data.descriptorsets[set][pair.first] = access;
					binding++;
					assert(!c.data.bind_descriptorsets.pDynamicOffsets || c.data.bind_descriptorsets.dynamicOffsetCount >= binding);
				}
			}
			free((void*)c.data.bind_descriptorsets.pDescriptorSets);
			free((void*)c.data.bind_descriptorsets.pDynamicOffsets);
			break;
		case VKCMDBINDDESCRIPTORBUFFERSEXT:
			descriptor_buffers.clear();
			descriptor_buffers.resize(c.data.bind_descriptor_buffers_ext.bufferCount);
			for (uint32_t i = 0; i < c.data.bind_descriptor_buffers_ext.bufferCount; i++)
			{
				mapped_address_range mapping;
				if (map_device_address_range(data, c.data.bind_descriptor_buffers_ext.addresses[i], 1, mapping, "descriptor buffer binding"))
				{
					descriptor_buffers[i].buffer_data = mapping.buffer_data;
					descriptor_buffers[i].offset = mapping.buffer_offset;
					descriptor_buffers[i].size = mapping.buffer_data->size - mapping.buffer_offset;
					descriptor_buffers[i].usage = c.data.bind_descriptor_buffers_ext.usages[i];
				}
			}
			free(c.data.bind_descriptor_buffers_ext.addresses);
			free(c.data.bind_descriptor_buffers_ext.usages);
			break;
		case VKCMDSETDESCRIPTORBUFFEROFFSETSEXT:
			{
				const uint32_t layout_index = index_to_VkPipelineLayout.index(c.data.set_descriptor_buffer_offsets_ext.layout);
				if (layout_index == CONTAINER_INVALID_INDEX) break;
				const trackedpipelinelayout& layout_data = VkPipelineLayout_index.at(layout_index);
				for (uint32_t i = 0; i < c.data.set_descriptor_buffer_offsets_ext.setCount; i++)
				{
					const uint32_t set = c.data.set_descriptor_buffer_offsets_ext.firstSet + i;
					if (set >= layout_data.descriptor_set_layout_count()) continue;
					const uint32_t binding_index = c.data.set_descriptor_buffer_offsets_ext.pBufferIndices ? c.data.set_descriptor_buffer_offsets_ext.pBufferIndices[i] : 0;
					if (binding_index >= descriptor_buffers.size()) continue;
					const descriptor_buffer_binding_state& descriptor_buffer = descriptor_buffers[binding_index];
					if (!descriptor_buffer.buffer_data) continue;
					const VkDeviceSize set_offset = c.data.set_descriptor_buffer_offsets_ext.pOffsets ? c.data.set_descriptor_buffer_offsets_ext.pOffsets[i] : 0;
					uint32_t set_layout_index = CONTAINER_INVALID_INDEX;
					if (set < layout_data.layout_indices.size()) set_layout_index = layout_data.layout_indices[set];
					if (set_layout_index == CONTAINER_INVALID_INDEX && set < layout_data.layouts.size())
					{
						set_layout_index = index_to_VkDescriptorSetLayout.index(layout_data.layouts[set]);
					}
					if (set_layout_index == CONTAINER_INVALID_INDEX) continue;
					const trackeddescriptorsetlayout& set_layout_data = VkDescriptorSetLayout_index.at(set_layout_index);
					for (const auto& binding_pair : set_layout_data.binding_types)
					{
						const uint32_t binding = binding_pair.first;
						const auto offset_it = set_layout_data.offsets.find(binding);
						const VkDeviceSize binding_offset = offset_it != set_layout_data.offsets.end() ? offset_it->second : 0;
						const VkDeviceSize descriptor_offset = descriptor_buffer.offset + set_offset + binding_offset;
						if (descriptor_offset >= descriptor_buffer.buffer_data->size) continue;
						data.descriptorsets[set][binding] = {
							.buffer_data = descriptor_buffer.buffer_data,
							.offset = descriptor_offset,
							.size = descriptor_buffer.buffer_data->size - descriptor_offset,
						};
						VkMarkingSubTypeARM subtype{};
						subtype.descriptorType = binding_pair.second;
						std::vector<discovered_buffer_marking> discovered_markings;
						discovered_markings.push_back({
							.buffer_data = descriptor_buffer.buffer_data,
							.offset = descriptor_offset,
							.size = 1,
							.type = VK_MARKING_TYPE_DESCRIPTOR_ARM,
							.subtype = subtype,
						});
						merge_discovered_markings(data, discovered_markings);
						record_descriptor_buffer_payload(data, descriptor_buffer.buffer_data->index, descriptor_offset, binding_pair.second);
					}
				}
			}
			free(c.data.set_descriptor_buffer_offsets_ext.pBufferIndices);
			free(c.data.set_descriptor_buffer_offsets_ext.pOffsets);
			break;
		case VKCMDCOPYBUFFER:
			{
				suballoc_location src = data.device_data.allocator->find_buffer_memory(c.data.copy_buffer.src_buffer_index);
				suballoc_location dst = data.device_data.allocator->find_buffer_memory(c.data.copy_buffer.dst_buffer_index);
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
				suballoc_location sub = data.device_data.allocator->find_buffer_memory(c.data.update_buffer.buffer_index);
				trackedbuffer& dst_buffer = VkBuffer_index.at(c.data.update_buffer.buffer_index);
				memcpy((char*)sub.memory + c.data.update_buffer.offset, c.data.update_buffer.values, c.data.update_buffer.size);
				dst_buffer.source.register_source(c.data.update_buffer.offset, c.data.update_buffer.size, c.source,
					1, 0, dst_buffer.object_type, dst_buffer.index);
			}
			free(c.data.update_buffer.values);
			break;
		case VKCMDPUSHCONSTANTS:
		case VKCMDPUSHCONSTANTS2KHR:
			if (data.push_constants.size() < c.data.push_constants.offset + c.data.push_constants.size) data.push_constants.resize(c.data.push_constants.offset + c.data.push_constants.size);
			memcpy(data.push_constants.data() + c.data.push_constants.offset, c.data.push_constants.values, c.data.push_constants.size);
			data.push_constant_sources.register_source(c.data.push_constants.offset, c.data.push_constants.size, c.source);
			free(c.data.push_constants.values);
			break;
		case VKCMDBINDPIPELINE:
			if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) graphics_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
			{
				compute_pipeline_bound = c.data.bind_pipeline.pipeline_index;
			}
			else if (c.data.bind_pipeline.pipelineBindPoint == VK_PIPELINE_BIND_POINT_DATA_GRAPH_ARM) data_graph_pipeline_bound = c.data.bind_pipeline.pipeline_index;
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
			data.stats.execution_commands++;
			if (compute_pipeline_bound != CONTAINER_INVALID_INDEX) // old style pipeline
			{
				auto& pipeline_data = VkPipeline_index.at(compute_pipeline_bound);
				assert(pipeline_data.shader_stages.size() == 1);
				assert(pipeline_data.shader_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
				pipeline_data.shader_stages[0].calls++;
				run_spirv(data, pipeline_data.shader_stages[0], c.source);
			}
			else // shader objects
			{
				auto& shader_object_data = VkShaderEXT_index.at(shader_objects.at(VK_SHADER_STAGE_COMPUTE_BIT));
				shader_object_data.stage.calls++;
				run_spirv(data, shader_object_data.stage, c.source);
			}
			break;
		case VKCMDDISPATCHDATAGRAPHARM:
			data.stats.execution_commands++;
			{
				const uint32_t session_index = c.data.dispatch_data_graph.session_index;
				if (session_index == CONTAINER_INVALID_INDEX) break;
				auto& session_data = VkDataGraphPipelineSessionARM_index.at(session_index);
				if (session_data.pipeline_index == CONTAINER_INVALID_INDEX) break;
				if (data_graph_pipeline_bound != CONTAINER_INVALID_INDEX && data_graph_pipeline_bound != session_data.pipeline_index)
				{
					DLOG("Data graph session %u dispatch uses pipeline %u while %u is currently bound",
						(unsigned)session_index, (unsigned)session_data.pipeline_index, (unsigned)data_graph_pipeline_bound);
				}
				auto& pipeline_data = VkPipeline_index.at(session_data.pipeline_index);
				// Count invocations here; GraphARM SPIR-V execution is still handled separately.
				for (auto& stage : pipeline_data.shader_stages)
				{
					stage.calls++;
				}
			}
			break;
		case VKCMDDRAW: // proxy for all draw commands
			data.stats.execution_commands++;
			if (graphics_pipeline_bound != CONTAINER_INVALID_INDEX) // old style pipeline
			{
				auto& pipeline_data = VkPipeline_index.at(graphics_pipeline_bound);
				for (auto& stage : pipeline_data.shader_stages)
				{
					if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT) continue;
					stage.calls++;
					run_spirv(data, stage, c.source);
				}
			}
			else for (auto& pair : shader_objects) // shader objects
			{
				if (pair.first == VK_SHADER_STAGE_COMPUTE_BIT) continue;
				auto& shader_object_data = VkShaderEXT_index.at(pair.second);
				shader_stage& stage = shader_object_data.stage;
				assert(pair.first == stage.stage);
				stage.calls++;
				run_spirv(data, stage, c.source);
			}
			break;
		case VKCMDBUILDACCELERATIONSTRUCTURESKHR:
			{
				std::vector<discovered_buffer_marking> discovered_markings;
				for (uint32_t i = 0; i < c.data.build_acceleration_structures.instance_count; i++)
				{
					VkDeviceSize primitive_offset = c.data.build_acceleration_structures.primitive_offsets[i];
					uint32_t primitive_count = c.data.build_acceleration_structures.primitive_counts[i];
					const VkDeviceAddress indirect_range_address = c.data.build_acceleration_structures.indirect_range_addresses[i];
					if (indirect_range_address != 0)
					{
						VkAccelerationStructureBuildRangeInfoKHR range = {};
						if (!read_acceleration_structure_build_range(data, indirect_range_address, range)) continue;
						primitive_offset = range.primitiveOffset;
						primitive_count = range.primitiveCount;
					}
					collect_acceleration_structure_instance_markings(data,
						c.data.build_acceleration_structures.instance_addresses[i],
						primitive_offset,
						primitive_count,
						discovered_markings);
				}
				merge_discovered_markings(data, discovered_markings);
				free(c.data.build_acceleration_structures.instance_addresses);
				free(c.data.build_acceleration_structures.primitive_offsets);
				free(c.data.build_acceleration_structures.primitive_counts);
				free(c.data.build_acceleration_structures.indirect_range_addresses);
			}
			break;
		case VKCMDTRACERAYSKHR: // proxy for all raytracing commands
			data.stats.execution_commands++;
			{
				if (raytracing_pipeline_bound == CONTAINER_INVALID_INDEX) break;
				auto& pipeline_data = VkPipeline_index.at(raytracing_pipeline_bound);
				if (pipeline_data.shader_stages.empty()) break;
				if (!c.trace_rays_valid)
				{
					for (auto& stage : pipeline_data.shader_stages)
					{
						stage.calls++;
						run_spirv(data, stage, c.source);
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
				std::vector<discovered_buffer_marking> discovered_markings;

				if (c.data.trace_rays.mode == trackedcommand::TRACE_RAYS_INDIRECT)
				{
					mapped_address_range mapping;
					if (map_device_address_range(data, c.data.trace_rays.indirect_device_address, sizeof(VkTraceRaysIndirectCommandKHR), mapping, "indirect"))
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
					if (map_device_address_range(data, c.data.trace_rays.indirect_device_address, sizeof(VkTraceRaysIndirectCommand2KHR), mapping, "indirect2"))
					{
						const VkTraceRaysIndirectCommand2KHR* indirect = reinterpret_cast<const VkTraceRaysIndirectCommand2KHR*>(mapping.ptr);
						VkMarkingSubTypeARM address_subtype{};
						address_subtype.deviceAddressType = VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM;
						raygen.deviceAddress = indirect->raygenShaderRecordAddress;
						raygen.size = indirect->raygenShaderRecordSize;
						raygen.stride = indirect->raygenShaderRecordSize;
						if (indirect->raygenShaderRecordAddress != 0)
						{
							discovered_markings.push_back({
								.buffer_data = mapping.buffer_data,
								.offset = mapping.buffer_offset + offsetof(VkTraceRaysIndirectCommand2KHR, raygenShaderRecordAddress),
								.size = sizeof(VkDeviceAddress),
								.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
								.subtype = address_subtype,
							});
						}
						miss.deviceAddress = indirect->missShaderBindingTableAddress;
						miss.size = indirect->missShaderBindingTableSize;
						miss.stride = indirect->missShaderBindingTableStride;
						if (indirect->missShaderBindingTableAddress != 0)
						{
							discovered_markings.push_back({
								.buffer_data = mapping.buffer_data,
								.offset = mapping.buffer_offset + offsetof(VkTraceRaysIndirectCommand2KHR, missShaderBindingTableAddress),
								.size = sizeof(VkDeviceAddress),
								.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
								.subtype = address_subtype,
							});
						}
						hit.deviceAddress = indirect->hitShaderBindingTableAddress;
						hit.size = indirect->hitShaderBindingTableSize;
						hit.stride = indirect->hitShaderBindingTableStride;
						if (indirect->hitShaderBindingTableAddress != 0)
						{
							discovered_markings.push_back({
								.buffer_data = mapping.buffer_data,
								.offset = mapping.buffer_offset + offsetof(VkTraceRaysIndirectCommand2KHR, hitShaderBindingTableAddress),
								.size = sizeof(VkDeviceAddress),
								.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
								.subtype = address_subtype,
							});
						}
						callable.deviceAddress = indirect->callableShaderBindingTableAddress;
						callable.size = indirect->callableShaderBindingTableSize;
						callable.stride = indirect->callableShaderBindingTableStride;
						if (indirect->callableShaderBindingTableAddress != 0)
						{
							discovered_markings.push_back({
								.buffer_data = mapping.buffer_data,
								.offset = mapping.buffer_offset + offsetof(VkTraceRaysIndirectCommand2KHR, callableShaderBindingTableAddress),
								.size = sizeof(VkDeviceAddress),
								.type = VK_MARKING_TYPE_DEVICE_ADDRESS_ARM,
								.subtype = address_subtype,
							});
						}
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
				collect_stages_from_sbt_region(data, pipeline_data, raygen, "raygen", stages_to_run, &discovered_markings);
				collect_stages_from_sbt_region(data, pipeline_data, miss, "miss", stages_to_run, &discovered_markings);
				collect_stages_from_sbt_region(data, pipeline_data, hit, "hit", stages_to_run, &discovered_markings);
				collect_stages_from_sbt_region(data, pipeline_data, callable, "callable", stages_to_run, &discovered_markings);
				merge_discovered_markings(data, discovered_markings);

				if (stages_to_run.empty())
				{
					for (auto& stage : pipeline_data.shader_stages)
					{
						stage.calls++;
						run_spirv(data, stage, c.source);
					}
				}
				else
				{
					for (uint32_t stage_index : stages_to_run)
					{
						pipeline_data.shader_stages[stage_index].calls++;
						run_spirv(data, pipeline_data.shader_stages[stage_index], c.source);
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
	return true;
}
