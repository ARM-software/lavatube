#include "tui_llm_router.h"

#include <curl/curl.h>

tui_llm_options tui_llm_default_options()
{
	tui_llm_options options;
	options.local.model = "gemma4:latest";
	options.local.base_url = "http://localhost:11434/v1";
	options.cloud.model = "gpt-5.5";
	options.cloud.base_url = "https://api.openai.com/v1";
	options.cloud.reasoning_effort = "low";
	return options;
}

bool tui_llm_resolve_options(tui_llm_options& options, std::string& error)
{
	if (!options.local.api_key.empty() && options.local.api_key != "ollama")
	{
		error = "LAVATUI_LOCAL_API_KEY must be set to ollama";
		return false;
	}

	const bool have_local = !options.local.api_key.empty();
	const bool have_cloud = !options.cloud.api_key.empty();
	if (options.requested_mode.empty())
	{
		if (!have_local && !have_cloud)
		{
			error = "Neither LAVATUI_LOCAL_API_KEY nor LAVATUI_CLOUD_API_KEY is set";
			return false;
		}
		options.mode = have_cloud ? tui_llm_mode::cloud_model : tui_llm_mode::local_model;
		return true;
	}

	if (options.requested_mode == "routed")
	{
		error = "LAVATUI_LLM_MODE=routed is not implemented yet";
		return false;
	}
	if (options.requested_mode == "local")
	{
		if (!have_local)
		{
			error = "LAVATUI_LLM_MODE=local requires LAVATUI_LOCAL_API_KEY=ollama";
			return false;
		}
		options.mode = tui_llm_mode::local_model;
		return true;
	}
	if (options.requested_mode == "cloud")
	{
		if (!have_cloud)
		{
			error = "LAVATUI_LLM_MODE=cloud requires LAVATUI_CLOUD_API_KEY";
			return false;
		}
		options.mode = tui_llm_mode::cloud_model;
		return true;
	}

	error = "Invalid LAVATUI_LLM_MODE \"" + options.requested_mode + "\". Expected local, cloud, or routed";
	return false;
}

tui_llm_command tui_llm_parse_command(const std::string& input)
{
	if (input == "/local") return tui_llm_command::local_model;
	if (input == "/cloud") return tui_llm_command::cloud_model;
	if (input == "/routed") return tui_llm_command::routed;
	return tui_llm_command::none;
}

const char* tui_llm_mode_name(tui_llm_mode mode)
{
	return mode == tui_llm_mode::local_model ? "local" : "cloud";
}

tui_llm_router::tui_llm_router(const tui_llm_options& options)
	: mLocalOptions(options.local)
	, mCloudOptions(options.cloud)
	, mLocalClient(options.local.api_key, options.local.model, options.local.base_url, options.local.reasoning_effort)
	, mCloudClient(options.cloud.api_key, options.cloud.model, options.cloud.base_url, options.cloud.reasoning_effort)
	, mMode(options.mode)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

tui_llm_router::~tui_llm_router()
{
	curl_global_cleanup();
}

bool tui_llm_router::set_mode(tui_llm_mode mode, std::string& error)
{
	const tui_llm_client& client = mode == tui_llm_mode::local_model ? mLocalClient : mCloudClient;
	if (!client.configured())
	{
		error = std::string(tui_llm_mode_name(mode)) + " model is not configured";
		return false;
	}
	mMode = mode;
	return true;
}

const tui_llm_client_options& tui_llm_router::active_options() const
{
	return mMode == tui_llm_mode::local_model ? mLocalOptions : mCloudOptions;
}

tui_assistant_result tui_llm_router::ask(const std::vector<tui_chat_message>& history, const tui_trace_tools& tools) const
{
	const tui_llm_client& client = mMode == tui_llm_mode::local_model ? mLocalClient : mCloudClient;
	return client.ask(history, tools);
}
