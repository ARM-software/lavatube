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
	printf("-m/--model MODEL       OpenAI model (default from LAVATUI_MODEL or gpt-5.5)\n");
	printf("-r/--reasoning LEVEL   Reasoning effort: none, low, medium, high (default from LAVATUI_REASONING or low)\n");
	printf("-U/--base-url URL      OpenAI-compatible base URL (default from LAVATUI_OPENAI_BASE_URL or https://api.openai.com/v1)\n");
	printf("                       API key comes from LAVATUI_OPENAI_API_KEY or OPENAI_API_KEY\n");
	printf("\n");
	printf("With a trace file, lava-tui reads packed trace metadata directly. Without a trace file,\n");
	printf("it connects to a running lava-replay --service, like lava-cli.\n");
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
	const char* model_env = getenv("LAVATUI_MODEL");
	std::string model = model_env ? model_env : "gpt-5.5";
	const char* reasoning_env = getenv("LAVATUI_REASONING");
	std::string reasoning_effort = reasoning_env ? reasoning_env : "low";
	const char* base_url_env = getenv("LAVATUI_OPENAI_BASE_URL");
	std::string base_url = base_url_env ? base_url_env : "https://api.openai.com/v1";
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
		else if (match(argv[i], "-m", "--model", remaining))
		{
			model = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], "-r", "--reasoning", remaining))
		{
			reasoning_effort = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], "-U", "--base-url", remaining))
		{
			base_url = get_str(argv[++i], remaining);
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

	if (!valid_reasoning_effort(reasoning_effort))
	{
		fprintf(stderr, "Invalid reasoning effort \"%s\". Expected one of: none, low, medium, high\n", reasoning_effort.c_str());
		return 1;
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
	options.model = model;
	options.base_url = base_url;
	options.reasoning_effort = reasoning_effort;
	options.verbose = verbose;
	const char* api_key = getenv("LAVATUI_OPENAI_API_KEY");
	if (!api_key) api_key = getenv("OPENAI_API_KEY");
	if (api_key) options.api_key = api_key;

	return run_tui(options);
}
