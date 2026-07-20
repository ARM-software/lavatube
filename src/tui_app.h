#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/component/app.hpp"
#include "ftxui/component/component.hpp"
#include "tui_llm_router.h"
#include "tui_trace_tools.h"

struct tui_options
{
	std::string trace_file;
	std::string hostname = "localhost";
	tui_llm_options llm;
	int port = 11901;
	bool replay_service = false;
	bool verbose = false;
};

int run_tui(const tui_options& options);
