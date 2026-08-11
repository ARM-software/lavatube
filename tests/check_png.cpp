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
	fprintf(stderr, "Usage: %s <image.png> [--expect-solid R,G,B,A | --compare image.png]\n", argv0);
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
	const char* compare_path = nullptr;
	rgba8 expected = {};
	if (argc == 4)
	{
		if (strcmp(argv[2], "--expect-solid") == 0 && parse_rgba(argv[3], expected))
		{
			expect_solid = true;
		}
		else if (strcmp(argv[2], "--compare") == 0)
		{
			compare_path = argv[3];
		}
		else
		{
			print_usage(argv[0]);
			return 1;
		}
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
	if (!expect_solid && !compare_path) return 0;

	unsigned char* pixels = stbi_load(argv[1], &width, &height, &channels, 4);
	if (!pixels)
	{
		fprintf(stderr, "Failed to load PNG %s: %s\n", argv[1], stbi_failure_reason());
		return 1;
	}
	unsigned char* comparison = nullptr;
	if (compare_path)
	{
		int comparison_width = 0;
		int comparison_height = 0;
		int comparison_channels = 0;
		comparison = stbi_load(compare_path, &comparison_width, &comparison_height, &comparison_channels, 4);
		if (!comparison)
		{
			fprintf(stderr, "Failed to load PNG %s: %s\n", compare_path, stbi_failure_reason());
			stbi_image_free(pixels);
			return 1;
		}
		if (comparison_width != width || comparison_height != height)
		{
			fprintf(stderr, "%s is %dx%d, but %s is %dx%d\n", argv[1], width, height,
			        compare_path, comparison_width, comparison_height);
			stbi_image_free(comparison);
			stbi_image_free(pixels);
			return 1;
		}
	}

	const int total = width * height;
	unsigned mismatches = 0;
	for (int i = 0; i < total; i++)
	{
		const unsigned char* p = pixels + (i * 4);
		const unsigned char* q = comparison ? comparison + (i * 4) : nullptr;
		if (q && memcmp(p, q, 4) == 0) continue;
		if (!q && p[0] == expected.r && p[1] == expected.g && p[2] == expected.b && p[3] == expected.a) continue;
		if (mismatches < 8)
		{
			const int x = i % width;
			const int y = i / width;
			if (q)
			{
				fprintf(stderr, "%s: pixel %d,%d was %u,%u,%u,%u, but %s was %u,%u,%u,%u\n",
				        argv[1], x, y, p[0], p[1], p[2], p[3], compare_path, q[0], q[1], q[2], q[3]);
			}
			else
			{
				fprintf(stderr, "%s: pixel %d,%d was %u,%u,%u,%u, expected %u,%u,%u,%u\n",
				        argv[1], x, y, p[0], p[1], p[2], p[3], expected.r, expected.g, expected.b, expected.a);
			}
		}
		mismatches++;
	}
	stbi_image_free(comparison);
	stbi_image_free(pixels);

	if (mismatches > 0)
	{
		if (compare_path)
		{
			fprintf(stderr, "%s: %u of %d pixels did not match %s\n", argv[1], mismatches, total, compare_path);
		}
		else
		{
			fprintf(stderr, "%s: %u of %d pixels did not match expected solid color %u,%u,%u,%u\n",
			        argv[1], mismatches, total, expected.r, expected.g, expected.b, expected.a);
		}
		return 1;
	}
	return 0;
}
