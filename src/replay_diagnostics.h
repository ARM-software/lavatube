#pragma once

#include <string>

#include "read.h"

const char* replay_diagnostics_thread_state_name(cli_thread_state state);
std::string replay_diagnostics_thread_wait_description(lava_reader& replayer, lava_file_reader& reader, cli_thread_state state);
std::string replay_diagnostics_threads_response(lava_reader& replayer);
std::string replay_diagnostics_deadlock_response(lava_reader& replayer);
