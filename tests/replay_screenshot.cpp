#include <assert.h>
#include <initializer_list>
#include <string>
#include <vector>

#include "replay_screenshot.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

static void expect_parse(const char* spec, std::initializer_list<replay_screenshot_range> expected)
{
	std::vector<replay_screenshot_range> parsed;
	std::string error;
	const bool ok = parse_replay_screenshot_ranges(spec, parsed, error);
	assert(ok);
	assert(error.empty());
	assert(parsed.size() == expected.size());
	size_t i = 0;
	for (const replay_screenshot_range& range : expected)
	{
		assert(parsed[i].first == range.first);
		assert(parsed[i].last == range.last);
		i++;
	}
}

static void expect_parse_fail(const char* spec)
{
	std::vector<replay_screenshot_range> parsed;
	std::string error;
	const bool ok = parse_replay_screenshot_ranges(spec, parsed, error);
	assert(!ok);
	assert(!error.empty());
}

int main()
{
	expect_parse("0", { { 0, 0 } });
	expect_parse("0,2-4,8", { { 0, 0 }, { 2, 4 }, { 8, 8 } });
	expect_parse(" 1 - 3 , 7 ", { { 1, 3 }, { 7, 7 } });

	expect_parse_fail("");
	expect_parse_fail(" ");
	expect_parse_fail("1-");
	expect_parse_fail("-3");
	expect_parse_fail("4-2");
	expect_parse_fail("0,0");
	expect_parse_fail("0-2,2-4");
	expect_parse_fail("a");
	expect_parse_fail("1-2-3");

	replay_screenshot_handler handler;
	std::vector<replay_screenshot_range> ranges = { { 1, 1 }, { 4, 6 } };
	handler.set_ranges(std::move(ranges));
	assert(!handler.is_frame_selected(0));
	assert(handler.is_frame_selected(1));
	assert(!handler.is_frame_selected(2));
	assert(!handler.is_frame_selected(3));
	assert(handler.is_frame_selected(4));
	assert(handler.is_frame_selected(5));
	assert(handler.is_frame_selected(6));
	assert(!handler.is_frame_selected(7));

	return 0;
}
