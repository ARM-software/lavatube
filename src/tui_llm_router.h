#pragma once

#include <string>
#include <vector>

#include "tui_llm_client.h"

enum class tui_llm_mode
{
	local_model,
	cloud_model,
};

enum class tui_llm_command
{
	none,
	local_model,
	cloud_model,
	routed,
};

struct tui_llm_client_options
{
	std::string api_key;
	std::string model;
	std::string base_url;
	std::string reasoning_effort;
};

struct tui_llm_options
{
	tui_llm_client_options local;
	tui_llm_client_options cloud;
	std::string requested_mode;
	tui_llm_mode mode = tui_llm_mode::cloud_model;
};

tui_llm_options tui_llm_default_options();
bool tui_llm_resolve_options(tui_llm_options& options, std::string& error);
tui_llm_command tui_llm_parse_command(const std::string& input);
const char* tui_llm_mode_name(tui_llm_mode mode);

class tui_llm_router
{
public:
	explicit tui_llm_router(const tui_llm_options& options);
	~tui_llm_router();

	bool set_mode(tui_llm_mode mode, std::string& error);
	tui_llm_mode mode() const { return mMode; }
	const tui_llm_client_options& active_options() const;
	tui_assistant_result ask(const std::vector<tui_chat_message>& history, const tui_trace_tools& tools) const;

private:
	tui_llm_client_options mLocalOptions;
	tui_llm_client_options mCloudOptions;
	tui_llm_client mLocalClient;
	tui_llm_client mCloudClient;
	tui_llm_mode mMode;
};
