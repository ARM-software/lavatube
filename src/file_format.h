#pragma once

#include <stdint.h>

struct packet_checkpoint
{
	uint32_t packet = 0;
	uint64_t position = 0;
};
