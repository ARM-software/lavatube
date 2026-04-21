#include <assert.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "external/tracetooltests/external/stb_image.h"

int main(int argc, char** argv)
{
	assert(argc == 2);
	int width = 0;
	int height = 0;
	int channels = 0;
	const int ok = stbi_info(argv[1], &width, &height, &channels);
	if (!ok)
	{
		fprintf(stderr, "Failed to parse PNG %s: %s\n", argv[1], stbi_failure_reason());
		return 1;
	}
	assert(width > 0);
	assert(height > 0);
	assert(channels > 0);
	return 0;
}
