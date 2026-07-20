#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include <string>

#include "tui_app.h"
#include "lavatube.h"

static bool verbose = false;

static bool valid_reasoning_effort(const std::string& value)
{
	return value == "none" || value == "low" || value == "medium" || value == "high";
}

void usage()
{
	printf("lava-tui %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-tui [options] [trace file]\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Verbose output\n");
	printf("-P/--port PORT         Replay service port when no trace file is provided (default %d)\n", (int)p__port);
	printf("-H/--host HOST         Replay service host when no trace file is provided (default localhost)\n");
	printf("\n");
	printf("With a trace file, lava-tui reads packed trace metadata directly. Without a trace file,\n");
	printf("it connects to a running lava-replay --service, like lava-cli.\n");
	printf("Configure models with LAVATUI_LOCAL_* and LAVATUI_CLOUD_* environment variables.\n");
	printf("Use /local or /cloud inside the TUI to switch models.\n");
#ifndef NDEBUG
	printf("-d/--debug level       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Output debug output to the given file\n");
#endif
	exit(-1);
}

int main(int argc, char **argv)
{
	std::string filename;
	std::string hostname = "localhost";
	int port = p__port;
	int remaining = argc - 1; // zeroth is name of program

	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-P", "--port", remaining))
		{
			port = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-H", "--host", remaining))
		{
			hostname = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], "-df", "--debugfile", remaining))
		{
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else
		{
			filename = get_str(argv[i], remaining);
			while (remaining > 0)
			{
				filename += " " + get_str(argv[++i], remaining);
			}
		}
	}

	if (!filename.empty() && access(filename.c_str(), R_OK) != 0)
	{
		fprintf(stderr, "Cannot read trace file \"%s\": %s\n", filename.c_str(), strerror(errno));
		return 1;
	}

	tui_options options;
	options.trace_file = filename;
	options.hostname = hostname;
	options.port = port;
	options.replay_service = filename.empty();
	options.verbose = verbose;
	options.llm = tui_llm_default_options();
	const char* value = getenv("LAVATUI_LOCAL_API_KEY");
	if (value) options.llm.local.api_key = value;
	value = getenv("LAVATUI_LOCAL_MODEL");
	if (value) options.llm.local.model = value;
	value = getenv("LAVATUI_LOCAL_BASE_URL");
	if (value) options.llm.local.base_url = value;

	value = getenv("LAVATUI_CLOUD_API_KEY");
	if (value) options.llm.cloud.api_key = value;
	value = getenv("LAVATUI_CLOUD_MODEL");
	if (value) options.llm.cloud.model = value;
	value = getenv("LAVATUI_CLOUD_BASE_URL");
	if (value) options.llm.cloud.base_url = value;
	value = getenv("LAVATUI_CLOUD_REASONING");
	if (value) options.llm.cloud.reasoning_effort = value;
	value = getenv("LAVATUI_LLM_MODE");
	if (value) options.llm.requested_mode = value;

	if (!valid_reasoning_effort(options.llm.cloud.reasoning_effort))
	{
		fprintf(stderr, "Invalid LAVATUI_CLOUD_REASONING \"%s\". Expected one of: none, low, medium, high\n", options.llm.cloud.reasoning_effort.c_str());
		return 1;
	}
	std::string error;
	if (!tui_llm_resolve_options(options.llm, error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	return run_tui(options);
}
