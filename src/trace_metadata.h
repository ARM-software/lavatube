#pragma once

#include <stdint.h>

#include <string>

#include "jsoncpp/json/value.h"

bool trace_metadata_validate(const std::string& trace_file, std::string& error);
std::string trace_metadata_objects_tsv(const std::string& trace_file);
std::string trace_metadata_objects_markdown(const std::string& trace_file);
bool trace_metadata_thread_json(const std::string& trace_file, uint32_t thread, Json::Value& out, std::string& error);
bool trace_metadata_frame_json(const std::string& trace_file, uint32_t thread, uint32_t frame, Json::Value& out, std::string& error);
std::string trace_metadata_json_compact(const Json::Value& value);
std::string trace_metadata_json_pretty(const Json::Value& value);
