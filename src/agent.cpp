#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <string>

#include "agent_runtime.h"
#include "agent_tools.h"
#include "lavatube.h"
#include "sandbox.h"
#include "util.h"

#define DEFAULT_SANDBOX_LEVEL 3

static void agent_usage()
{
	printf("lava-agent %d.%d.%d-" RELTYPE " command line options\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-agent --service HOST:PORT [options] <trace file> ask <prompt>\n");
	printf("-h/--help              This help\n");
	printf("-v/--verbose           Write progress diagnostics to standard error\n");
	printf("-d/--debug LEVEL       Set debug level [0,1,2,3]\n");
	printf("-df/--debugfile FILE   Write the complete model/tool transcript as JSONL\n");
	printf("-s/--sandbox LEVEL     Set security sandbox level [1,2,3] (default %d)\n", (int)DEFAULT_SANDBOX_LEVEL);
	printf("--service HOST:PORT    Required replay service endpoint\n");
	printf("--timeout SECONDS      Maximum wall-clock duration (default 300)\n");
	printf("--max-rounds N         Maximum model rounds (default 8)\n");
	printf("--max-tool-calls N     Maximum tool calls (default 32)\n");
	printf("--max-output-bytes N   Maximum result size (default 32768, minimum 1024)\n");
	printf("--base-url URL         OpenAI-compatible API base (or LAVA_AGENT_BASE_URL)\n");
	printf("--model MODEL          Local model name (or LAVA_AGENT_MODEL)\n");
	printf("--reasoning-effort E   Model reasoning effort (or LAVA_AGENT_REASONING_EFFORT)\n");
	printf("--api-key KEY          API key (or LAVA_AGENT_API_KEY, default ollama)\n");
}

static std::string agent_environment(const char* name, const char* fallback)
{
	const char* value = getenv(name);
	return value && value[0] != '\0' ? value : fallback;
}

static bool agent_parse_u64(const std::string& text, uint64_t& value)
{
	if (text.empty() || text[0] == '-') return false;
	char* end = nullptr;
	errno = 0;
	const unsigned long long parsed = strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0') return false;
	value = parsed;
	return true;
}

static bool agent_parse_service(const std::string& endpoint, std::string& hostname, int& port)
{
	std::string port_text;
	if (!endpoint.empty() && endpoint[0] == '[')
	{
		const size_t close = endpoint.find(']');
		if (close == std::string::npos || close + 2 > endpoint.size() || endpoint[close + 1] != ':') return false;
		hostname = endpoint.substr(1, close - 1);
		port_text = endpoint.substr(close + 2);
	}
	else
	{
		const size_t colon = endpoint.rfind(':');
		if (colon == std::string::npos) return false;
		hostname = endpoint.substr(0, colon);
		port_text = endpoint.substr(colon + 1);
	}
	uint64_t parsed = 0;
	if (hostname.empty() || !agent_parse_u64(port_text, parsed) || parsed == 0 || parsed > 65535) return false;
	port = (int)parsed;
	return true;
}

static bool agent_parse_model_port(const std::string& url, uint16_t& port)
{
	const size_t separator = url.find("://");
	if (separator == std::string::npos) return false;
	const std::string scheme = url.substr(0, separator);
	if (scheme != "http" && scheme != "https") return false;
	const size_t authority_start = separator + 3;
	const size_t authority_end = url.find_first_of("/?#", authority_start);
	const size_t end = authority_end == std::string::npos ? url.size() : authority_end;
	if (authority_start == end) return false;
	std::string port_text;
	if (url[authority_start] == '[')
	{
		const size_t close = url.find(']', authority_start + 1);
		if (close == std::string::npos || close >= end) return false;
		if (close + 1 < end)
		{
			if (url[close + 1] != ':') return false;
			port_text = url.substr(close + 2, end - close - 2);
		}
	}
	else
	{
		const size_t colon = url.rfind(':', end - 1);
		if (colon != std::string::npos && colon >= authority_start)
		{
			if (url.find(':', authority_start) != colon) return false;
			port_text = url.substr(colon + 1, end - colon - 1);
		}
	}
	if (port_text.empty())
	{
		port = scheme == "https" ? 443 : 80;
		return true;
	}
	uint64_t parsed = 0;
	if (!agent_parse_u64(port_text, parsed) || parsed == 0 || parsed > UINT16_MAX) return false;
	port = (uint16_t)parsed;
	return true;
}

static Json::Value agent_error_envelope(const std::string& message)
{
	Json::Value output;
	output["schema_version"] = 1;
	output["status"] = "error";
	output["conclusion"] = message;
	output["confidence"] = 0.0;
	output["evidence"] = Json::Value(Json::arrayValue);
	output["unresolved"] = Json::Value(Json::arrayValue);
	output["unresolved"].append(message);
	output["usage"]["rounds"] = 0;
	output["usage"]["calls"] = 0;
	return output;
}

static void agent_mark_unstable(Json::Value& output, const std::string& reason, size_t maximum)
{
	output["status"] = "unstable";
	output["unresolved"].append(reason);
	while (!output["evidence"].empty() && agent_json_compact(output).size() > maximum)
	{
		output["evidence"].resize(output["evidence"].size() - 1);
	}
	if (agent_json_compact(output).size() > maximum)
	{
		output["conclusion"] = "Replay stability could not be confirmed.";
		output["unresolved"] = Json::Value(Json::arrayValue);
		output["unresolved"].append(reason);
	}
}

int main(int argc, char** argv)
{
	if (p__sandbox_level == -1) p__sandbox_level = DEFAULT_SANDBOX_LEVEL;
	if (p__sandbox_level >= 1) sandbox_level_one();
	if (p__debug_destination == stdout) p__debug_destination = stderr;
	if (argc <= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
	{
		agent_usage();
		return 0;
	}

	agent_runtime_options runtime_options;
	runtime_options.base_url = agent_environment("LAVA_AGENT_BASE_URL", "http://localhost:11434/v1");
	runtime_options.api_key = agent_environment("LAVA_AGENT_API_KEY", "ollama");
	runtime_options.model = agent_environment("LAVA_AGENT_MODEL", "gemma4:latest");
	runtime_options.reasoning_effort = agent_environment("LAVA_AGENT_REASONING_EFFORT", "");
	agent_tools_options tool_options;
	std::string debug_filename;
	std::string error;
	uint16_t model_port = 0;
	int index = 1;
	while (index < argc && argv[index][0] == '-')
	{
		const std::string option = argv[index++];
		if (option == "-h" || option == "--help")
		{
			agent_usage();
			return 0;
		}
		if (option == "-v" || option == "--verbose") runtime_options.verbose = true;
		else if (option == "--service" && index < argc)
		{
			if (!agent_parse_service(argv[index++], tool_options.hostname, tool_options.port)) error = "Invalid --service HOST:PORT";
		}
		else if ((option == "-df" || option == "--debugfile") && index < argc) debug_filename = argv[index++];
		else if ((option == "-d" || option == "--debug") && index < argc)
		{
			uint64_t level = 0;
			if (!agent_parse_u64(argv[index++], level) || level > 3) error = "Invalid debug level";
			else p__debug_level = (uint_fast8_t)level;
		}
		else if ((option == "-s" || option == "--sandbox") && index < argc)
		{
			uint64_t level = 0;
			if (!agent_parse_u64(argv[index++], level) || level == 0 || level > 3) error = "Invalid --sandbox level";
			else p__sandbox_level = (int_fast8_t)level;
		}
		else if (option == "--timeout" && index < argc)
		{
			uint64_t seconds = 0;
			if (!agent_parse_u64(argv[index++], seconds) || seconds == 0 || seconds > UINT32_MAX) error = "Invalid --timeout value";
			else runtime_options.timeout_seconds = (uint32_t)seconds;
		}
		else if (option == "--max-rounds" && index < argc)
		{
			uint64_t rounds = 0;
			if (!agent_parse_u64(argv[index++], rounds) || rounds == 0 || rounds > UINT32_MAX) error = "Invalid --max-rounds value";
			else runtime_options.max_rounds = (uint32_t)rounds;
		}
		else if (option == "--max-tool-calls" && index < argc)
		{
			uint64_t calls = 0;
			if (!agent_parse_u64(argv[index++], calls) || calls == 0 || calls > UINT32_MAX) error = "Invalid --max-tool-calls value";
			else runtime_options.max_tool_calls = (uint32_t)calls;
		}
		else if (option == "--max-output-bytes" && index < argc)
		{
			uint64_t bytes = 0;
			if (!agent_parse_u64(argv[index++], bytes) || bytes < 1024 || bytes > SIZE_MAX) error = "Invalid --max-output-bytes value";
			else runtime_options.max_output_bytes = (size_t)bytes;
		}
		else if (option == "--base-url" && index < argc) runtime_options.base_url = argv[index++];
		else if (option == "--model" && index < argc) runtime_options.model = argv[index++];
		else if (option == "--reasoning-effort" && index < argc) runtime_options.reasoning_effort = argv[index++];
		else if (option == "--api-key" && index < argc) runtime_options.api_key = argv[index++];
		else error = "Unknown or incomplete option: " + option;
		if (!error.empty()) break;
	}

	std::string prompt;
	if (error.empty() && tool_options.port == 0) error = "--service HOST:PORT is required";
	if (error.empty() && index >= argc) error = "Trace file is required";
	if (error.empty()) tool_options.trace_file = argv[index++];
	if (error.empty() && (index >= argc || strcmp(argv[index++], "ask") != 0)) error = "Expected ask after the trace file";
	if (error.empty() && index >= argc) error = "Investigation prompt is required";
	while (error.empty() && index < argc)
	{
		if (!prompt.empty()) prompt += " ";
		prompt += argv[index++];
	}
	if (error.empty() && (runtime_options.base_url.empty() || runtime_options.model.empty() || runtime_options.api_key.empty()))
	{
		error = "Model base URL, model, and API key must be configured";
	}
	if (error.empty() && p__sandbox_level >= 2 && !agent_parse_model_port(runtime_options.base_url, model_port))
	{
		error = "Model base URL must be an HTTP or HTTPS URL when sandbox level 2 or 3 is enabled";
	}
	if (!error.empty())
	{
		fprintf(stderr, "lava-agent: %s\n", error.c_str());
		std::cout << agent_json_compact(agent_error_envelope(error)) << std::endl;
		return 2;
	}
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(runtime_options.timeout_seconds);
	tool_options.deadline = deadline;

	if (!debug_filename.empty())
	{
		runtime_options.debug_file = fopen(debug_filename.c_str(), "w");
		if (!runtime_options.debug_file)
		{
			error = "Failed to open debug file " + debug_filename + ": " + strerror(errno);
			fprintf(stderr, "lava-agent: %s\n", error.c_str());
			std::cout << agent_json_compact(agent_error_envelope(error)) << std::endl;
			return 2;
		}
	}
	const uint16_t connect_ports[2] = { (uint16_t)tool_options.port, model_port };
	const size_t connect_port_count = connect_ports[0] == connect_ports[1] ? 1 : 2;
	if (p__sandbox_level >= 2) sandbox_level_two(connect_port_count, connect_ports);
	if (p__sandbox_level >= 3) sandbox_level_three();

	agent_tools tools(tool_options);
	std::string initial_position;
	if (!tools.validate(initial_position, error))
	{
		Json::Value output = agent_error_envelope(error);
		int result = 1;
		if (error.rfind("Replay service is not paused:", 0) == 0)
		{
			output["status"] = "unstable";
			result = 0;
		}
		fprintf(stderr, "lava-agent: %s\n", error.c_str());
		std::cout << agent_json_compact(output) << std::endl;
		if (runtime_options.debug_file) fclose(runtime_options.debug_file);
		return result;
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);
	agent_runtime runtime(runtime_options, tools);
	Json::Value output = runtime.ask(prompt, deadline);
	curl_global_cleanup();
	std::string stability_error;
	if (!tools.stable_at(initial_position, stability_error))
	{
		agent_mark_unstable(output, stability_error, runtime_options.max_output_bytes);
	}
	std::cout << agent_json_compact(output) << std::endl;
	if (runtime_options.debug_file) fclose(runtime_options.debug_file);
	return output["status"].asString() == "error" ? 1 : 0;
}
