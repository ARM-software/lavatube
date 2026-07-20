#include <assert.h>

#include <string>

#include "tui_llm_router.h"

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
	assert(router.set_mode(tui_llm_mode::local_model, error));
	assert(router.mode() == tui_llm_mode::local_model);
	assert(router.active_options().model == "gemma4:latest");

	options = tui_llm_default_options();
	options.cloud.api_key = "cloud-key";
	assert(tui_llm_resolve_options(options, error));
	tui_llm_router cloud_only(options);
	assert(!cloud_only.set_mode(tui_llm_mode::local_model, error));
	assert(cloud_only.mode() == tui_llm_mode::cloud_model);
}

int main()
{
	test_defaults();
	test_mode_resolution();
	test_commands_and_switching();
	return 0;
}
