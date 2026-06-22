#include "trace_metadata.h"

#include <errno.h>
#include <stdlib.h>

#include <set>
#include <vector>

#include "datatable.h"
#include "jsoncpp/json/writer.h"
#include "packfile.h"
#include "util.h"

static bool string_has_json_suffix(const std::string& name)
{
	return name.size() >= 5 && name.compare(name.size() - 5, 5, ".json") == 0;
}

static uint32_t frame_thread_from_file(const std::string& name)
{
	if (name.size() <= 12 || name.rfind("frames_", 0) != 0 || !string_has_json_suffix(name)) return UINT32_MAX;
	const std::string number = name.substr(7, name.size() - 12);
	char* end = nullptr;
	errno = 0;
	const unsigned long value = strtoul(number.c_str(), &end, 10);
	if (errno != 0 || end == number.c_str() || *end != '\0' || value > UINT32_MAX) return UINT32_MAX;
	return (uint32_t)value;
}

static bool trace_metadata_has_thread(const std::string& trace_file, uint32_t thread)
{
	const std::vector<std::string> files = packed_files(trace_file, "frames_");
	for (const std::string& name : files)
	{
		if (frame_thread_from_file(name) == thread) return true;
	}
	return false;
}

static data_table trace_metadata_objects_table(const std::string& trace_file)
{
	Json::Value limits = packed_json("limits.json", trace_file);
	data_table table;
	table.set_headers({ "object_type", "count" });
	for (const std::string& name : limits.getMemberNames())
	{
		const Json::Value& value = limits[name];
		if (!value.isNumeric()) continue;
		const Json::UInt64 count = value.asUInt64();
		if (count == 0) continue;
		table.add_row({ name, std::to_string(count) });
	}
	return table;
}

bool trace_metadata_validate(const std::string& trace_file, std::string& error)
{
	std::set<std::string> files;
	const std::vector<std::string> packed = packed_files(trace_file);
	for (const std::string& file : packed)
	{
		files.insert(file);
	}

	const char* required[] = { "limits.json", "tracking.json", "metadata.json", "frames_0.json" };
	for (const char* file : required)
	{
		if (files.find(file) == files.end())
		{
			error = "Missing " + std::string(file) + " in trace container";
			return false;
		}
	}

	return true;
}

std::string trace_metadata_objects_tsv(const std::string& trace_file)
{
	return trace_metadata_objects_table(trace_file).to_tsv();
}

std::string trace_metadata_objects_markdown(const std::string& trace_file)
{
	return trace_metadata_objects_table(trace_file).to_markdown();
}

bool trace_metadata_thread_json(const std::string& trace_file, uint32_t thread, Json::Value& out, std::string& error)
{
	if (!trace_metadata_has_thread(trace_file, thread))
	{
		error = "No metadata file for thread " + _to_string(thread);
		return false;
	}

	out = packed_json("frames_" + _to_string(thread) + ".json", trace_file);
	out.removeMember("frames");
	return true;
}

bool trace_metadata_frame_json(const std::string& trace_file, uint32_t thread, uint32_t frame, Json::Value& out, std::string& error)
{
	if (!trace_metadata_has_thread(trace_file, thread))
	{
		error = "No metadata file for thread " + _to_string(thread);
		return false;
	}

	const std::string file = "frames_" + _to_string(thread) + ".json";
	Json::Value frameinfo = packed_json(file, trace_file);
	if (!frameinfo.isMember("frames") || !frameinfo["frames"].isArray())
	{
		error = file + " does not contain a frames array";
		return false;
	}
	if (frame >= frameinfo["frames"].size())
	{
		error = "Frame " + _to_string(frame) + " is out of range for thread " + _to_string(thread);
		return false;
	}

	out = frameinfo["frames"][frame];
	return true;
}

std::string trace_metadata_json_compact(const Json::Value& value)
{
	Json::FastWriter writer;
	std::string out = writer.write(value);
	if (!out.empty() && out.back() == '\n') out.pop_back();
	return out;
}

std::string trace_metadata_json_pretty(const Json::Value& value)
{
	Json::StyledWriter writer;
	return writer.write(value);
}
