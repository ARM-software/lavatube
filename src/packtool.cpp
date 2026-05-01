#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include "util.h"
#include "packfile.h"

struct packed_compare_result
{
	bool equal = true;
	uint64_t size_a = 0;
	uint64_t size_b = 0;
	uint64_t first_difference = UINT64_MAX;
};

static packed_compare_result compare_packed_file(const std::string& pack_a, const std::string& pack_b, const std::string& inside)
{
	packed_compare_result result;
	packed a = packed_open(inside, pack_a);
	packed b = packed_open(inside, pack_b);
	result.size_a = a.size();
	result.size_b = b.size();
	if (result.size_a != result.size_b)
	{
		result.equal = false;
		a.close();
		b.close();
		return result;
	}

	const uint32_t chunk_size = 1024 * 1024;
	std::vector<char> buffer_a(chunk_size);
	std::vector<char> buffer_b(chunk_size);
	uint64_t offset = 0;
	while (offset < result.size_a)
	{
		const uint32_t current = std::min<uint64_t>(chunk_size, result.size_a - offset);
		a.read(buffer_a.data(), current);
		b.read(buffer_b.data(), current);
		if (memcmp(buffer_a.data(), buffer_b.data(), current) != 0)
		{
			result.equal = false;
			for (uint32_t i = 0; i < current; i++)
			{
				if (buffer_a[i] != buffer_b[i])
				{
					result.first_difference = offset + i;
					break;
				}
			}
			break;
		}
		offset += current;
	}

	a.close();
	b.close();
	return result;
}

static packed_compare_result compare_regular_file(const std::string& path_a, const std::string& path_b)
{
	packed_compare_result result;
	struct stat stat_a;
	struct stat stat_b;
	int fd_a = open(path_a.c_str(), O_RDONLY | O_NOATIME);
	if (fd_a == -1) FAIL("Cannot open \"%s\": %s", path_a.c_str(), strerror(errno));
	int fd_b = open(path_b.c_str(), O_RDONLY | O_NOATIME);
	if (fd_b == -1) FAIL("Cannot open \"%s\": %s", path_b.c_str(), strerror(errno));
	if (fstat(fd_a, &stat_a) == -1) FAIL("Cannot stat \"%s\": %s", path_a.c_str(), strerror(errno));
	if (fstat(fd_b, &stat_b) == -1) FAIL("Cannot stat \"%s\": %s", path_b.c_str(), strerror(errno));
	result.size_a = stat_a.st_size;
	result.size_b = stat_b.st_size;
	if (result.size_a != result.size_b)
	{
		result.equal = false;
		close(fd_a);
		close(fd_b);
		return result;
	}

	const uint32_t chunk_size = 1024 * 1024;
	std::vector<char> buffer_a(chunk_size);
	std::vector<char> buffer_b(chunk_size);
	uint64_t offset = 0;
	while (offset < result.size_a)
	{
		const uint32_t current = std::min<uint64_t>(chunk_size, result.size_a - offset);
		size_t read_a = 0;
		size_t read_b = 0;
		while (read_a < current)
		{
			const ssize_t res = read(fd_a, buffer_a.data() + read_a, current - read_a);
			if (res == -1 && errno == EINTR) continue;
			if (res <= 0) FAIL("Failed to read \"%s\": %s", path_a.c_str(), strerror(errno));
			read_a += res;
		}
		while (read_b < current)
		{
			const ssize_t res = read(fd_b, buffer_b.data() + read_b, current - read_b);
			if (res == -1 && errno == EINTR) continue;
			if (res <= 0) FAIL("Failed to read \"%s\": %s", path_b.c_str(), strerror(errno));
			read_b += res;
		}
		if (memcmp(buffer_a.data(), buffer_b.data(), current) != 0)
		{
			result.equal = false;
			for (uint32_t i = 0; i < current; i++)
			{
				if (buffer_a[i] != buffer_b[i])
				{
					result.first_difference = offset + i;
					break;
				}
			}
			break;
		}
		offset += current;
	}

	close(fd_a);
	close(fd_b);
	return result;
}

struct semantic_compare_result
{
	bool identical = true;
	bool sane = true;
	unsigned allowed_differences = 0;
	unsigned unexpected_differences = 0;
	std::vector<std::string> messages;
};

static int usage(const char* argv0)
{
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, "\t%s pack <output file> <input directory>\n", argv0);
	fprintf(stdout, "\t%s unpack <output directory> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s extract <output file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s add <input file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s list <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s print <target file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s check <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s diff [--semantic] [--assert-sane] <input packaged file A> <input packaged file B>\n", argv0);
	return 0;
}

static bool starts_with(const std::string& value, const char* prefix)
{
	return value.rfind(prefix, 0) == 0;
}

static bool ends_with(const std::string& value, const char* suffix)
{
	const size_t len = strlen(suffix);
	return value.size() >= len && value.compare(value.size() - len, len, suffix) == 0;
}

static bool is_thread_binary(const std::string& value)
{
	return starts_with(value, "thread_") && ends_with(value, ".bin");
}

static bool is_frames_json(const std::string& value)
{
	return starts_with(value, "frames_") && ends_with(value, ".json");
}

static std::string json_value_string(const Json::Value& value)
{
	if (value.isString()) return "\"" + value.asString() + "\"";
	if (value.isBool()) return value.asBool() ? "true" : "false";
	if (value.isUInt64()) return _to_string(value.asUInt64());
	if (value.isInt64()) return _to_string(value.asInt64());
	if (value.isDouble())
	{
		std::ostringstream ss;
		ss << value.asDouble();
		return ss.str();
	}
	std::string out = value.toStyledString();
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
	return out;
}

static void json_compare_recursive(const Json::Value& a, const Json::Value& b, const std::string& path, std::vector<std::string>& diffs, size_t max_diffs = 12)
{
	if (diffs.size() >= max_diffs) return;
	if (a.type() != b.type())
	{
		diffs.push_back(path + ": type differs (" + _to_string(a.type()) + " vs " + _to_string(b.type()) + ")");
		return;
	}

	switch (a.type())
	{
	case Json::nullValue:
		return;
	case Json::intValue:
	case Json::uintValue:
	case Json::realValue:
	case Json::stringValue:
	case Json::booleanValue:
		if (a != b) diffs.push_back(path + ": " + json_value_string(a) + " vs " + json_value_string(b));
		return;
	case Json::arrayValue:
		if (a.size() != b.size())
		{
			diffs.push_back(path + ": array length differs (" + _to_string(a.size()) + " vs " + _to_string(b.size()) + ")");
			if (diffs.size() >= max_diffs) return;
		}
		for (Json::ArrayIndex i = 0; i < std::min(a.size(), b.size()) && diffs.size() < max_diffs; i++)
		{
			json_compare_recursive(a[i], b[i], path + "[" + _to_string(i) + "]", diffs, max_diffs);
		}
		return;
	case Json::objectValue:
	{
		std::vector<std::string> names_a = a.getMemberNames();
		std::vector<std::string> names_b = b.getMemberNames();
		std::sort(names_a.begin(), names_a.end());
		std::sort(names_b.begin(), names_b.end());
		for (const std::string& name : names_a)
		{
			if (!b.isMember(name))
			{
				diffs.push_back(path + "." + name + ": only in A");
				if (diffs.size() >= max_diffs) return;
			}
		}
		for (const std::string& name : names_b)
		{
			if (!a.isMember(name))
			{
				diffs.push_back(path + "." + name + ": only in B");
				if (diffs.size() >= max_diffs) return;
			}
		}
		for (const std::string& name : names_a)
		{
			if (!b.isMember(name) || diffs.size() >= max_diffs) continue;
			json_compare_recursive(a[name], b[name], path + "." + name, diffs, max_diffs);
		}
		return;
	}
	}
}

static std::map<unsigned, std::string> reverse_dictionary(const Json::Value& dict)
{
	std::map<unsigned, std::string> result;
	for (const std::string& name : dict.getMemberNames())
	{
		result[dict[name].asUInt()] = name;
	}
	return result;
}

static std::string map_api_id(const std::map<unsigned, std::string>& dict, unsigned id)
{
	auto it = dict.find(id);
	if (it != dict.end()) return it->second;
	return "<unknown api " + _to_string(id) + ">";
}

static void normalize_metadata_json(Json::Value& value)
{
	value.removeMember("lavatube_version_major");
	value.removeMember("lavatube_version_minor");
	value.removeMember("lavatube_version_patch");
	value.removeMember("vulkan_header_version");
}

static void normalize_tracking_json(Json::Value& value, const std::map<unsigned, std::string>& dict)
{
	if (value.isArray())
	{
		for (Json::ArrayIndex i = 0; i < value.size(); i++) normalize_tracking_json(value[i], dict);
		return;
	}
	if (!value.isObject()) return;

	if (value.isMember("api_created") && value["api_created"].isUInt())
	{
		value["api_created"] = map_api_id(dict, value["api_created"].asUInt());
	}
	if (value.isMember("api_destroyed") && value["api_destroyed"].isUInt())
	{
		value["api_destroyed"] = map_api_id(dict, value["api_destroyed"].asUInt());
	}

	for (const std::string& name : value.getMemberNames())
	{
		normalize_tracking_json(value[name], dict);
	}
}

static void compare_json_file(semantic_compare_result& result, const std::string& file, Json::Value a, Json::Value b)
{
	if (a == b) return;
	result.identical = false;
	result.sane = false;
	result.unexpected_differences++;
	std::vector<std::string> diffs;
	json_compare_recursive(a, b, file, diffs);
	if (diffs.empty()) result.messages.push_back("Unexpected semantic difference in " + file);
	for (const std::string& diff : diffs) result.messages.push_back(diff);
}

static void compare_metadata_file(semantic_compare_result& result, const std::string& pack_a, const std::string& pack_b)
{
	Json::Value raw_a = packed_json("metadata.json", pack_a);
	Json::Value raw_b = packed_json("metadata.json", pack_b);

	const char* allowed[] = { "lavatube_version_major", "lavatube_version_minor", "lavatube_version_patch", "vulkan_header_version" };
	for (const char* name : allowed)
	{
		if (raw_a[name] != raw_b[name])
		{
			result.identical = false;
			result.allowed_differences++;
			result.messages.push_back(std::string("Allowed metadata difference: metadata.json.") + name + " changed from "
				+ json_value_string(raw_a[name]) + " to " + json_value_string(raw_b[name]));
		}
	}

	normalize_metadata_json(raw_a);
	normalize_metadata_json(raw_b);
	compare_json_file(result, "metadata.json", raw_a, raw_b);
}

static void compare_dictionary_file(semantic_compare_result& result, const std::string& pack_a, const std::string& pack_b)
{
	Json::Value a = packed_json("dictionary.json", pack_a);
	Json::Value b = packed_json("dictionary.json", pack_b);
	std::vector<std::string> members_a = a.getMemberNames();
	std::vector<std::string> members_b = b.getMemberNames();
	std::set<std::string> names_a(members_a.begin(), members_a.end());
	std::set<std::string> names_b(members_b.begin(), members_b.end());
	if (names_a != names_b)
	{
		result.identical = false;
		result.sane = false;
		result.unexpected_differences++;
		for (const std::string& name : names_a) if (names_b.count(name) == 0) result.messages.push_back("dictionary.json: only in A: " + name);
		for (const std::string& name : names_b) if (names_a.count(name) == 0) result.messages.push_back("dictionary.json: only in B: " + name);
		return;
	}

	unsigned remapped = 0;
	for (const std::string& name : names_a)
	{
		if (a[name] != b[name]) remapped++;
	}
	if (remapped)
	{
		result.identical = false;
		result.allowed_differences++;
		result.messages.push_back("Allowed semantic difference: dictionary.json remapped " + _to_string(remapped) + " function ids");
	}
}

static semantic_compare_result compare_packed_file_semantic(const std::string& pack_a, const std::string& pack_b)
{
	semantic_compare_result result;
	std::vector<std::string> files_a = packed_files(pack_a);
	std::vector<std::string> files_b = packed_files(pack_b);
	std::set<std::string> set_a(files_a.begin(), files_a.end());
	std::set<std::string> set_b(files_b.begin(), files_b.end());

	for (const std::string& name : set_a)
	{
		if (set_b.count(name) == 0)
		{
			result.identical = false;
			result.sane = false;
			result.unexpected_differences++;
			result.messages.push_back("Only in " + pack_a + ": " + name);
		}
	}
	for (const std::string& name : set_b)
	{
		if (set_a.count(name) == 0)
		{
			result.identical = false;
			result.sane = false;
			result.unexpected_differences++;
			result.messages.push_back("Only in " + pack_b + ": " + name);
		}
	}
	if (!set_a.count("dictionary.json") || !set_b.count("dictionary.json"))
	{
		result.identical = false;
		result.sane = false;
		result.unexpected_differences++;
		result.messages.push_back("Semantic diff requires dictionary.json in both packs");
		return result;
	}

	compare_dictionary_file(result, pack_a, pack_b);
	const std::map<unsigned, std::string> reverse_a = reverse_dictionary(packed_json("dictionary.json", pack_a));
	const std::map<unsigned, std::string> reverse_b = reverse_dictionary(packed_json("dictionary.json", pack_b));

	for (const std::string& name : set_a)
	{
		if (set_b.count(name) == 0) continue;
		const packed_compare_result raw = compare_packed_file(pack_a, pack_b, name);
		if (raw.equal) continue;
		result.identical = false;

		if (name == "dictionary.json")
		{
			continue; // already handled semantically
		}
		else if (name == "metadata.json")
		{
			compare_metadata_file(result, pack_a, pack_b);
		}
		else if (name == "tracking.json")
		{
			Json::Value a = packed_json(name, pack_a);
			Json::Value b = packed_json(name, pack_b);
			normalize_tracking_json(a, reverse_a);
			normalize_tracking_json(b, reverse_b);
			compare_json_file(result, name, a, b);
		}
		else if (name == "limits.json" || is_frames_json(name) || ends_with(name, ".json"))
		{
			compare_json_file(result, name, packed_json(name, pack_a), packed_json(name, pack_b));
		}
		else if (is_thread_binary(name))
		{
			result.allowed_differences++;
			if (raw.size_a != raw.size_b)
			{
				result.messages.push_back("Unchecked binary difference allowed in " + name + ": size "
					+ _to_string(raw.size_a) + " vs " + _to_string(raw.size_b));
			}
			else
			{
				result.messages.push_back("Unchecked binary difference allowed in " + name + ": first differing byte at offset "
					+ _to_string(raw.first_difference));
			}
		}
		else
		{
			result.sane = false;
			result.unexpected_differences++;
			if (raw.size_a != raw.size_b)
			{
				result.messages.push_back("Unexpected binary difference in " + name + ": size "
					+ _to_string(raw.size_a) + " vs " + _to_string(raw.size_b));
			}
			else
			{
				result.messages.push_back("Unexpected binary difference in " + name + ": first differing byte at offset "
					+ _to_string(raw.first_difference));
			}
		}
	}
	return result;
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		return usage(argv[0]);
	}
	else if (strcmp(argv[1], "diff") == 0)
	{
		bool semantic = false;
		bool assert_sane = false;
		std::vector<const char*> positional;
		for (int i = 2; i < argc; i++)
		{
			if (strcmp(argv[i], "--semantic") == 0) semantic = true;
			else if (strcmp(argv[i], "--assert-sane") == 0)
			{
				assert_sane = true;
				semantic = true;
			}
			else positional.push_back(argv[i]);
		}
		if (positional.size() != 2) return usage(argv[0]);

		if (semantic)
		{
			semantic_compare_result result = compare_packed_file_semantic(positional[0], positional[1]);
			if (result.identical && result.sane)
			{
				printf("Semantic identical: %s and %s\n", positional[0], positional[1]);
				return 0;
			}
			for (const std::string& message : result.messages)
			{
				printf("%s\n", message.c_str());
			}
			if (result.sane)
			{
				printf("Semantic diff is sane: %u allowed differences\n", result.allowed_differences);
				return assert_sane ? 0 : 1;
			}
			printf("Semantic diff is NOT sane: %u allowed differences, %u unexpected differences\n",
				result.allowed_differences, result.unexpected_differences);
			return 1;
		}

		std::vector<std::string> files_a = packed_files(positional[0]);
		std::vector<std::string> files_b = packed_files(positional[1]);
		std::set<std::string> set_a(files_a.begin(), files_a.end());
		std::set<std::string> set_b(files_b.begin(), files_b.end());
		bool different = false;
		unsigned identical_files = 0;
		unsigned differing_files = 0;

		for (const std::string& name : set_a)
		{
			if (set_b.count(name) == 0)
			{
				printf("Only in %s: %s\n", positional[0], name.c_str());
				different = true;
			}
		}
		for (const std::string& name : set_b)
		{
			if (set_a.count(name) == 0)
			{
				printf("Only in %s: %s\n", positional[1], name.c_str());
				different = true;
			}
		}

		for (const std::string& name : set_a)
		{
			if (set_b.count(name) == 0) continue;
			packed_compare_result result = compare_packed_file(positional[0], positional[1], name);
			if (result.equal)
			{
				identical_files++;
				continue;
			}

			different = true;
			differing_files++;
			if (result.size_a != result.size_b)
			{
				printf("Different: %s size %lu vs %lu\n", name.c_str(), (unsigned long)result.size_a, (unsigned long)result.size_b);
			}
			else
			{
				printf("Different: %s first differing byte at offset %lu (size %lu)\n",
					name.c_str(), (unsigned long)result.first_difference, (unsigned long)result.size_a);
			}
		}

		if (!different)
		{
			packed_compare_result raw = compare_regular_file(positional[0], positional[1]);
			if (raw.equal)
			{
				printf("Identical: %s and %s (%u files)\n", positional[0], positional[1], identical_files);
				return 0;
			}

			if (raw.size_a != raw.size_b)
			{
				printf("Contained files identical, but packaged file size differs: %lu vs %lu\n",
					(unsigned long)raw.size_a, (unsigned long)raw.size_b);
			}
			else
			{
				printf("Contained files identical, but packaged file bytes differ at offset %lu (size %lu)\n",
					(unsigned long)raw.first_difference, (unsigned long)raw.size_a);
			}
			return 1;
		}

		printf("Summary: %u identical, %u different\n", identical_files, differing_files);
		return 1;
	}
	else if (strcmp(argv[1], "check") == 0)
	{
		if (argc != 3) return usage(argv[0]);
		std::map<std::string, bool> map;
		std::vector<std::string> files = packed_files(argv[2]);
		int threads_bin = 0;
		int threads_json = 0;
		for (const std::string& s : files)
		{
			map[s] = true;
			if (strncmp(s.c_str(), "thread_", 6) == 0) threads_bin++;
			if (strncmp(s.c_str(), "frames_", 6) == 0) threads_json++;
		}
		if (threads_bin == 0) DIE("No threads in trace file");
		if (threads_bin != threads_json) DIE("Mismatched number of binaries and JSON files");
		if (!map.at("limits.json")) DIE("No limits.json file in container");
		if (!map.at("dictionary.json")) DIE("No dictionary.json in container");
		if (!map.at("metadata.json")) DIE("No metadata.json in container");
		if (!map.at("tracking.json")) DIE("No tracking.json in container");
		if (!map.at("frames_0.json")) DIE("No frames_0.json in container");
		printf("Success\n");
		return 0;
	}
	else if (strcmp(argv[1], "pack") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		if (!pack_directory(argv[2], argv[3], false))
		{
			FAIL("Failed to pack directory \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "add") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		if (!pack_add(argv[2], argv[3]))
		{
			FAIL("Failed to add \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "unpack") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		if (!unpack_directory(argv[3], argv[2]))
		{
			FAIL("Failed to unpack file \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "print") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		Json::Value js = packed_json(argv[2], argv[3]);
		printf("%s", js.toStyledString().c_str());
		return 0;
	}
	else if (strcmp(argv[1], "extract") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		packed pf = packed_open(argv[2], argv[3]);
		int fd = open(argv[2], O_CREAT | O_TRUNC | O_WRONLY | O_NOATIME, 0664);
		if (fd == -1) FAIL("Failed to create target %s: %s", argv[2], strerror(errno));
		int res = sendfile(fd, pf.fd, nullptr, pf.filesize);
		if (res == -1) FAIL("Failed to write out the file: %s\n", strerror(errno));
		pf.close();
		close(fd);
		return 0;
	}
	else if (strcmp(argv[1], "list") == 0)
	{
		if (argc != 3) return usage(argv[0]);
		packed_list(argv[2]);
		return 0;
	}
	fprintf(stderr, "Unrecognized command: %s\n", argv[1]);
	return -1;
}
