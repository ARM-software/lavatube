#include <assert.h>

#include <string>

#include "tui_llm_router.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

static void test_defaults()
{
	const tui_llm_options options = tui_llm_default_options();
	assert(options.local.model == "gemma4:latest");
	assert(options.local.base_url == "http://localhost:11434/v1");
	assert(options.local.reasoning_effort.empty());
	assert(options.cloud.model == "gpt-5.5");
	assert(options.cloud.base_url == "https://api.openai.com/v1");
	assert(options.cloud.reasoning_effort == "low");
}

static void test_mode_resolution()
{
	std::string error;
	tui_llm_options options = tui_llm_default_options();
	assert(!tui_llm_resolve_options(options, error));
	assert(error.find("Neither") != std::string::npos);

	options = tui_llm_default_options();
	options.local.api_key = "ollama";
	assert(tui_llm_resolve_options(options, error));
	assert(options.mode == tui_llm_mode::local_model);

	options = tui_llm_default_options();
	options.cloud.api_key = "cloud-key";
	assert(tui_llm_resolve_options(options, error));
	assert(options.mode == tui_llm_mode::cloud_model);

	options = tui_llm_default_options();
	options.local.api_key = "ollama";
	options.cloud.api_key = "cloud-key";
	assert(tui_llm_resolve_options(options, error));
	assert(options.mode == tui_llm_mode::cloud_model);

	options.requested_mode = "local";
	assert(tui_llm_resolve_options(options, error));
	assert(options.mode == tui_llm_mode::local_model);
	options.requested_mode = "cloud";
	assert(tui_llm_resolve_options(options, error));
	assert(options.mode == tui_llm_mode::cloud_model);

	options.requested_mode = "routed";
	assert(!tui_llm_resolve_options(options, error));
	assert(error.find("not implemented") != std::string::npos);
	options.requested_mode = "invalid";
	assert(!tui_llm_resolve_options(options, error));
	assert(error.find("Invalid") != std::string::npos);

	options = tui_llm_default_options();
	options.local.api_key = "wrong";
	assert(!tui_llm_resolve_options(options, error));
	assert(error.find("must be set to ollama") != std::string::npos);

	options = tui_llm_default_options();
	options.requested_mode = "local";
	assert(!tui_llm_resolve_options(options, error));
	options.requested_mode = "cloud";
	assert(!tui_llm_resolve_options(options, error));
}

static void test_commands_and_switching()
{
	assert(tui_llm_parse_command("/local") == tui_llm_command::local_model);
	assert(tui_llm_parse_command("/cloud") == tui_llm_command::cloud_model);
	assert(tui_llm_parse_command("/routed") == tui_llm_command::routed);
	assert(tui_llm_parse_command(" /local") == tui_llm_command::none);
	assert(tui_llm_parse_command("/local now") == tui_llm_command::none);

	std::string error;
	tui_llm_options options = tui_llm_default_options();
	options.local.api_key = "ollama";
	options.cloud.api_key = "cloud-key";
	assert(tui_llm_resolve_options(options, error));
	tui_llm_router router(options);
	assert(router.mode() == tui_llm_mode::cloud_model);
	assert(router.active_options().model == "gpt-5.5");
	const bool switched_to_local = router.set_mode(tui_llm_mode::local_model, error);
	assert(switched_to_local);
	assert(router.mode() == tui_llm_mode::local_model);
	assert(router.active_options().model == "gemma4:latest");

	options = tui_llm_default_options();
	options.cloud.api_key = "cloud-key";
	assert(tui_llm_resolve_options(options, error));
	tui_llm_router cloud_only(options);
	const bool switched_cloud_only = cloud_only.set_mode(tui_llm_mode::local_model, error);
	assert(!switched_cloud_only);
	assert(cloud_only.mode() == tui_llm_mode::cloud_model);
}

static void test_connect_ports()
{
	std::string error;
	std::vector<uint16_t> ports;
	tui_llm_options options = tui_llm_default_options();
	assert(tui_llm_append_connect_ports(options, ports, error));
	assert(ports.empty());

	options.local.api_key = "ollama";
	assert(tui_llm_append_connect_ports(options, ports, error));
	assert(ports.size() == 1);
	assert(ports[0] == 11434);

	options.cloud.api_key = "cloud-key";
	assert(tui_llm_append_connect_ports(options, ports, error));
	assert(ports.size() == 2);
	assert(ports[1] == 443);

	options.local.base_url = "http://localhost:8443/v1";
	ports.clear();
	ports.push_back(8443);
	assert(tui_llm_append_connect_ports(options, ports, error));
	assert(ports.size() == 2);
	assert(ports[0] == 8443);
	assert(ports[1] == 443);

	options.local.base_url = "ftp://localhost:21";
	ports.clear();
	assert(!tui_llm_append_connect_ports(options, ports, error));
	assert(error.find("Invalid HTTP or HTTPS") != std::string::npos);
}

int main()
{
	test_defaults();
	test_mode_resolution();
	test_commands_and_switching();
	test_connect_ports();
	return 0;
}
