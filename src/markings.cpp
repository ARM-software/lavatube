#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "markings.h"

VkMarkedOffsetsARM* clone_marked_offsets(const VkMarkedOffsetsARM* src)
{
	if (!src) return nullptr;

	VkMarkedOffsetsARM* dst = (VkMarkedOffsetsARM*)malloc(sizeof(VkMarkedOffsetsARM));
	if (!dst) ABORT("Failed to allocate VkMarkedOffsetsARM");
	memset(dst, 0, sizeof(*dst));
	dst->sType = src->sType;
	dst->pNext = nullptr;
	dst->count = src->count;
	if (src->count == 0) return dst;

	const size_t types_bytes = sizeof(VkMarkingTypeARM) * src->count;
	const size_t subs_bytes = sizeof(VkMarkingSubTypeARM) * src->count;
	const size_t offsets_bytes = sizeof(VkDeviceSize) * src->count;

	if (src->pMarkingTypes)
	{
		dst->pMarkingTypes = (VkMarkingTypeARM*)malloc(types_bytes);
		if (!dst->pMarkingTypes) ABORT("Failed to allocate VkMarkedOffsetsARM pMarkingTypes");
		memcpy((void*)dst->pMarkingTypes, src->pMarkingTypes, types_bytes);
	}

	if (src->pSubTypes)
	{
		dst->pSubTypes = (VkMarkingSubTypeARM*)malloc(subs_bytes);
		if (!dst->pSubTypes) ABORT("Failed to allocate VkMarkedOffsetsARM pSubTypes");
		memcpy((void*)dst->pSubTypes, src->pSubTypes, subs_bytes);
	}

	if (src->pOffsets)
	{
		dst->pOffsets = (VkDeviceSize*)malloc(offsets_bytes);
		if (!dst->pOffsets) ABORT("Failed to allocate VkMarkedOffsetsARM pOffsets");
		memcpy((void*)dst->pOffsets, src->pOffsets, offsets_bytes);
	}

	return dst;
}

void sort_marked_offsets(VkMarkedOffsetsARM* markings)
{
	if (!markings || markings->count <= 1) return;
	assert(markings->pOffsets);
	assert(markings->pMarkingTypes);
	assert(markings->pSubTypes);

	std::vector<uint32_t> order(markings->count);
	for (uint32_t i = 0; i < markings->count; i++) order[i] = i;

	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
	{
		if (markings->pOffsets[a] != markings->pOffsets[b]) return markings->pOffsets[a] < markings->pOffsets[b];
		const uint32_t type_a = (uint32_t)markings->pMarkingTypes[a];
		const uint32_t type_b = (uint32_t)markings->pMarkingTypes[b];
		if (type_a != type_b) return type_a < type_b;
		const uint64_t sub_a = markings->pSubTypes[a].reserved;
		const uint64_t sub_b = markings->pSubTypes[b].reserved;
		return sub_a < sub_b;
	});

	std::vector<VkDeviceSize> offsets(markings->count);
	for (uint32_t i = 0; i < markings->count; i++) offsets[i] = markings->pOffsets[order[i]];
	memcpy((void*)markings->pOffsets, offsets.data(), sizeof(VkDeviceSize) * markings->count);

	std::vector<VkMarkingTypeARM> types(markings->count);
	for (uint32_t i = 0; i < markings->count; i++) types[i] = markings->pMarkingTypes[order[i]];
	memcpy((void*)markings->pMarkingTypes, types.data(), sizeof(VkMarkingTypeARM) * markings->count);

	std::vector<VkMarkingSubTypeARM> subs(markings->count);
	for (uint32_t i = 0; i < markings->count; i++) subs[i] = markings->pSubTypes[order[i]];
	memcpy((void*)markings->pSubTypes, subs.data(), sizeof(VkMarkingSubTypeARM) * markings->count);
}

void free_marked_offsets(VkMarkedOffsetsARM* markings)
{
	if (!markings) return;
	free((void*)markings->pMarkingTypes);
	free((void*)markings->pSubTypes);
	free((void*)markings->pOffsets);
	free(markings);
}

marked_offsets_difference compare_marked_offsets(const VkMarkedOffsetsARM* a, const VkMarkedOffsetsARM* b)
{
	if (!a && !b) return marked_offsets_difference::none;
	if (!a) return marked_offsets_difference::missing_left;
	if (!b) return marked_offsets_difference::missing_right;
	if (a->sType != b->sType) return marked_offsets_difference::s_type;
	if (a->count != b->count) return marked_offsets_difference::count;
	if (a->count == 0) return marked_offsets_difference::none;
	if (!a->pMarkingTypes || !b->pMarkingTypes) return marked_offsets_difference::marking_types_missing;
	if (!a->pSubTypes || !b->pSubTypes) return marked_offsets_difference::sub_types_missing;
	if (!a->pOffsets || !b->pOffsets) return marked_offsets_difference::offsets_missing;

	const size_t types_bytes = sizeof(VkMarkingTypeARM) * a->count;
	const size_t subs_bytes = sizeof(VkMarkingSubTypeARM) * a->count;
	const size_t offsets_bytes = sizeof(VkDeviceSize) * a->count;

	if (memcmp(a->pMarkingTypes, b->pMarkingTypes, types_bytes) != 0) return marked_offsets_difference::marking_types;
	if (memcmp(a->pSubTypes, b->pSubTypes, subs_bytes) != 0) return marked_offsets_difference::sub_types;
	if (memcmp(a->pOffsets, b->pOffsets, offsets_bytes) != 0) return marked_offsets_difference::offsets;
	return marked_offsets_difference::none;
}
