#pragma once

#include <string>
#include <vector>

bool parse_android_replay_arguments(const std::string& command_line, std::vector<std::string>& arguments, std::string& error);
