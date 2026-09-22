#pragma once

#include <stdint.h>

struct lava_format_block_info
{
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t size;
};

uint32_t lava_plane_compatible_format(uint32_t format, uint32_t plane);
lava_format_block_info lava_format_block_info_get(uint32_t format);
