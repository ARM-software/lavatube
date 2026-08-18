#include "android_replay_args.h"

#include <stdio.h>

static bool expect_arguments(const char* command_line, const std::vector<std::string>& expected)
{
	std::vector<std::string> actual;
	std::string error;
	if (!parse_android_replay_arguments(command_line, actual, error))
	{
		fprintf(stderr, "Failed to parse '%s': %s\n", command_line, error.c_str());
		return false;
	}
	if (actual == expected) return true;
	fprintf(stderr, "Unexpected arguments for '%s'\n", command_line);
	return false;
}

static bool expect_error(const char* command_line)
{
	std::vector<std::string> actual;
	std::string error;
	if (!parse_android_replay_arguments(command_line, actual, error)) return true;
	fprintf(stderr, "Expected parse failure for '%s'\n", command_line);
	return false;
}

int main()
{
	bool success = true;
	success &= expect_arguments("--service -H 127.0.0.1 trace.api", { "--service", "-H", "127.0.0.1", "trace.api" });
	success &= expect_arguments("--screenshot-prefix 'screen shots/frame ' \"trace file.api\"", { "--screenshot-prefix", "screen shots/frame ", "trace file.api" });
	success &= expect_arguments("one\\ two pre\"middle value\"post '' \"\"", { "one two", "premiddle valuepost", "", "" });
	success &= expect_arguments("  tabs\tand\nlines  ", { "tabs", "and", "lines" });
	success &= expect_error("unterminated 'quote");
	success &= expect_error("trailing\\");
	return success ? 0 : 1;
}
