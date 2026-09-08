#include "suballocator.h"

#include <cassert>

static void test_mapped_memory_range()
{
	suballoc_location loc = {};
	loc.offset = 128;
	loc.size = 19320;
	loc.allocation_size = 32 * 1024 * 1024;
	loc.non_coherent_atom_size = 64;

	VkMappedMemoryRange range = loc.mapped_memory_range();
	assert(range.offset == 128);
	assert(range.size == 19328);

	loc.offset = 19456;
	loc.size = 3450;
	range = loc.mapped_memory_range();
	assert(range.offset == 19456);
	assert(range.size == 3456);

	loc.offset = 130;
	loc.size = 60;
	range = loc.mapped_memory_range();
	assert(range.offset == 128);
	assert(range.size == 64);

	loc.offset = 192;
	loc.size = 8;
	loc.allocation_size = 200;
	range = loc.mapped_memory_range();
	assert(range.offset == 192);
	assert(range.size == VK_WHOLE_SIZE);
}

int main()
{
	test_mapped_memory_range();
	return 0;
}
