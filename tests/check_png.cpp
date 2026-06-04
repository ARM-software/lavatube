#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "external/tracetooltests/external/stb_image.h"

struct rgba8
{
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 0;
};

static void print_usage(const char* argv0)
{
	fprintf(stderr, "Usage: %s <image.png> [--expect-solid R,G,B,A]\n", argv0);
}

static bool parse_byte(const char* first, const char* last, uint8_t& out)
{
	if (first == last) return false;
	char tmp[4] = {};
	const size_t len = (size_t)(last - first);
	if (len > 3) return false;
	memcpy(tmp, first, len);
	char* end = nullptr;
	const unsigned long value = strtoul(tmp, &end, 10);
	if (end != tmp + len || value > 255) return false;
	out = (uint8_t)value;
	return true;
}

static bool parse_rgba(const char* text, rgba8& out)
{
	const char* first = text;
	uint8_t values[4] = {};
	for (unsigned i = 0; i < 4; i++)
	{
		const char* last = strchr(first, i == 3 ? '\0' : ',');
		if (!last || !parse_byte(first, last, values[i])) return false;
		first = last + 1;
	}
	out = { values[0], values[1], values[2], values[3] };
	return true;
}

int main(int argc, char** argv)
{
	if (argc != 2 && argc != 4)
	{
		print_usage(argv[0]);
		return 1;
	}

	bool expect_solid = false;
	rgba8 expected = {};
	if (argc == 4)
	{
		if (strcmp(argv[2], "--expect-solid") != 0 || !parse_rgba(argv[3], expected))
		{
			print_usage(argv[0]);
			return 1;
		}
		expect_solid = true;
	}

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
	if (!expect_solid) return 0;

	unsigned char* pixels = stbi_load(argv[1], &width, &height, &channels, 4);
	if (!pixels)
	{
		fprintf(stderr, "Failed to load PNG %s: %s\n", argv[1], stbi_failure_reason());
		return 1;
	}

	const int total = width * height;
	unsigned mismatches = 0;
	for (int i = 0; i < total; i++)
	{
		const unsigned char* p = pixels + (i * 4);
		if (p[0] == expected.r && p[1] == expected.g && p[2] == expected.b && p[3] == expected.a) continue;
		if (mismatches < 8)
		{
			const int x = i % width;
			const int y = i / width;
			fprintf(stderr, "%s: pixel %d,%d was %u,%u,%u,%u, expected %u,%u,%u,%u\n",
			        argv[1], x, y, p[0], p[1], p[2], p[3], expected.r, expected.g, expected.b, expected.a);
		}
		mismatches++;
	}
	stbi_image_free(pixels);

	if (mismatches > 0)
	{
		fprintf(stderr, "%s: %u of %d pixels did not match expected solid color %u,%u,%u,%u\n",
		        argv[1], mismatches, total, expected.r, expected.g, expected.b, expected.a);
		return 1;
	}
	return 0;
}
