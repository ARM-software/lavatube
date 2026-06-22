#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/component/app.hpp"
#include "ftxui/component/component.hpp"
#include "tui_openai.h"
#include "tui_trace_tools.h"

struct tui_options
{
	std::string trace_file;
	std::string api_key;
	std::string model;
	std::string base_url;
	std::string reasoning_effort;
	bool verbose = false;
};

int run_tui(const tui_options& options);
