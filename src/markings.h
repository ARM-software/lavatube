#pragma once

#include "lavatube.h"

enum class marked_offsets_difference
{
	none = 0,
	missing_left,
	missing_right,
	s_type,
	count,
	marking_types_missing,
	sub_types_missing,
	offsets_missing,
	marking_types,
	sub_types,
	offsets,
};

VkMarkedOffsetsARM* clone_marked_offsets(const VkMarkedOffsetsARM* src);
void normalize_marked_offsets(VkMarkedOffsetsARM* markings);
void sort_marked_offsets(VkMarkedOffsetsARM* markings);
VkMarkedOffsetsARM* merge_marked_offsets(const VkMarkedOffsetsARM* a, const VkMarkedOffsetsARM* b);
void free_marked_offsets(VkMarkedOffsetsARM* markings);
marked_offsets_difference compare_marked_offsets(const VkMarkedOffsetsARM* a, const VkMarkedOffsetsARM* b);
const char* marked_offsets_difference_string(marked_offsets_difference diff);
