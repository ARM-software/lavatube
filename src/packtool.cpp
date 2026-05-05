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
#include "zipc.h"
#include "read.h"
#include "read_auto.h"
#include "markings.h"

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
	int fd_a = open(path_a.c_str(), O_RDONLY | default_file_flags);
	if (fd_a == -1) FAIL("Cannot open \"%s\": %s", path_a.c_str(), strerror(errno));
	int fd_b = open(path_b.c_str(), O_RDONLY | default_file_flags);
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

static void convert_packfile_to_zip(const std::string& input, const std::string& output)
{
	if (input == output)
	{
		FAIL("Input and output files must differ");
	}

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* archive = zipc_open(output.c_str(), "w", &status);
	if (!archive)
	{
		FAIL("Failed to create output zip file \"%s\": %s", output.c_str(), zipc_strerror(status));
	}

	const std::vector<std::string> files = packed_files(input);
	std::vector<char> buffer(1024 * 1024);
	for (const std::string& name : files)
	{
		packed pf = packed_open(name, input);
		zipcstream* stream = zipc_stream_open(archive, name.c_str(), "", &status);
		if (!stream)
		{
			pf.close();
			zipc_close(archive);
			FAIL("Failed to open zip stream for \"%s\": %s", name.c_str(), zipc_strerror(status));
		}

		uint64_t remaining = pf.size();
		while (remaining > 0)
		{
			const size_t chunk = std::min<uint64_t>(buffer.size(), remaining);
			pf.read(buffer.data(), chunk);
			status = zipc_stream_write(archive, stream, chunk, buffer.data());
			if (status != ZIPC_SUCCESS)
			{
				pf.close();
				zipc_close(archive);
				FAIL("Failed while writing \"%s\" to \"%s\": %s", name.c_str(), output.c_str(), zipc_strerror(status));
			}
			remaining -= chunk;
		}

		status = zipc_stream_close(archive, stream);
		pf.close();
		if (status != ZIPC_SUCCESS)
		{
			zipc_close(archive);
			FAIL("Failed to finalize \"%s\" in \"%s\": %s", name.c_str(), output.c_str(), zipc_strerror(status));
		}
	}

	status = zipc_validate(archive);
	if (status != ZIPC_SUCCESS)
	{
		zipc_close(archive);
		FAIL("Converted zip file \"%s\" failed validation: %s", output.c_str(), zipc_strerror(status));
	}

	status = zipc_close(archive);
	if (status != ZIPC_SUCCESS)
	{
		FAIL("Failed to close output zip file \"%s\": %s", output.c_str(), zipc_strerror(status));
	}
}

struct semantic_compare_result
{
	bool identical = true;
	bool sane = true;
	unsigned allowed_differences = 0;
	unsigned unexpected_differences = 0;
	std::vector<std::string> messages;
};

struct markings_compare_result
{
	bool identical = true;
	std::vector<std::string> messages;
};

struct collected_markings_entry
{
	change_source source;
	uint64_t packet = 0;
	uint8_t instrtype = 0;
	uint32_t occurrence = 0;
	VkMarkedOffsetsARM* markings = nullptr;
};

struct markings_collect_state
{
	std::vector<collected_markings_entry>* out = nullptr;
	uint64_t packet = 0;
	uint8_t instrtype = 0;
	uint32_t occurrence = 0;
};

static int usage(const char* argv0)
{
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, "\t%s pack <output file> <input directory>\n", argv0);
	fprintf(stdout, "\t%s convert <input packaged file> <output zip file>\n", argv0);
	fprintf(stdout, "\t%s unpack <output directory> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s extract <output file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s add <input file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s list <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s print <target file> <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s check <input packaged file>\n", argv0);
	fprintf(stdout, "\t%s diff [--semantic] [--assert-sane] [--assert-markings] <input packaged file A> <input packaged file B>\n", argv0);
	fprintf(stdout, "\t%s list-markings <input packaged file>\n", argv0);
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

static const char* packet_type_name(uint8_t instrtype)
{
	switch (instrtype)
	{
	case PACKET_VULKAN_API_CALL: return "PACKET_VULKAN_API_CALL";
	case PACKET_THREAD_BARRIER: return "PACKET_THREAD_BARRIER";
	case PACKET_IMAGE_UPDATE: return "PACKET_IMAGE_UPDATE";
	case PACKET_BUFFER_UPDATE: return "PACKET_BUFFER_UPDATE";
	case PACKET_VULKANSC_API_CALL: return "PACKET_VULKANSC_API_CALL";
	case PACKET_TENSOR_UPDATE: return "PACKET_TENSOR_UPDATE";
	case PACKET_IMAGE_UPDATE2: return "PACKET_IMAGE_UPDATE2";
	case PACKET_BUFFER_UPDATE2: return "PACKET_BUFFER_UPDATE2";
	default: return "PACKET_UNKNOWN";
	}
}

static std::string packtool_marked_offsets_difference_string(marked_offsets_difference diff)
{
	switch (diff)
	{
	case marked_offsets_difference::none: return "none";
	case marked_offsets_difference::missing_left: return "missing in A";
	case marked_offsets_difference::missing_right: return "missing in B";
	case marked_offsets_difference::s_type: return "sType mismatch";
	case marked_offsets_difference::count: return "count mismatch";
	case marked_offsets_difference::marking_types_missing: return "marking types missing";
	case marked_offsets_difference::sub_types_missing: return "subtypes missing";
	case marked_offsets_difference::offsets_missing: return "offsets missing";
	case marked_offsets_difference::marking_types: return "marking type mismatch";
	case marked_offsets_difference::sub_types: return "marking subtype mismatch";
	case marked_offsets_difference::offsets: return "offset mismatch";
	}
	return "unknown";
}

static std::string markings_location_string(const collected_markings_entry& entry)
{
	std::string where = "thread " + _to_string(entry.source.thread)
		+ " packet " + _to_string(entry.packet)
		+ " marking " + _to_string(entry.occurrence);
	if (entry.instrtype == PACKET_VULKAN_API_CALL && entry.source.call_id != UINT16_MAX)
	{
		where += " (" + std::string(get_function_name(entry.source.call_id))
			+ ", call " + _to_string(entry.source.call) + ")";
	}
	else
	{
		where += " (" + std::string(packet_type_name(entry.instrtype))
			+ ", api call " + _to_string(entry.source.call) + ")";
	}
	return where;
}

static bool is_capture_flush_leftover_markings(const collected_markings_entry& entry)
{
	return entry.instrtype == PACKET_VULKAN_API_CALL
		&& entry.source.call_id != UINT16_MAX
		&& strcmp(get_function_name(entry.source.call_id), "vkFlushMappedMemoryRanges") == 0;
}

static std::string marking_type_string(VkMarkingTypeARM type)
{
	switch (type)
	{
	case VK_MARKING_TYPE_DEVICE_ADDRESS_ARM: return "device_address";
	case VK_MARKING_TYPE_DESCRIPTOR_SIZE_ARM: return "descriptor_size";
	case VK_MARKING_TYPE_DESCRIPTOR_OFFSET_ARM: return "descriptor_offset";
	case VK_MARKING_TYPE_DESCRIPTOR_ARM: return "descriptor";
	case VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM: return "shader_group_handle";
	default: return "unknown(" + _to_string((int)type) + ")";
	}
}

static std::string marking_subtype_string(VkMarkingTypeARM type, const VkMarkingSubTypeARM& subtype)
{
	switch (type)
	{
	case VK_MARKING_TYPE_DEVICE_ADDRESS_ARM:
		switch (subtype.deviceAddressType)
		{
		case VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM: return "buffer";
		case VK_DEVICE_ADDRESS_TYPE_ACCELERATION_STRUCTURE_ARM: return "acceleration_structure";
		default: return "unknown_device_address_type(" + _to_string((int)subtype.deviceAddressType) + ")";
		}
	case VK_MARKING_TYPE_DESCRIPTOR_SIZE_ARM:
	case VK_MARKING_TYPE_DESCRIPTOR_OFFSET_ARM:
	case VK_MARKING_TYPE_DESCRIPTOR_ARM:
		return "descriptor_type=" + _to_string((int)subtype.descriptorType);
	case VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM:
		switch (subtype.shaderGroupType)
		{
		case VK_SHADER_GROUP_SHADER_GENERAL_KHR: return "general";
		case VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR: return "closest_hit";
		case VK_SHADER_GROUP_SHADER_ANY_HIT_KHR: return "any_hit";
		case VK_SHADER_GROUP_SHADER_INTERSECTION_KHR: return "intersection";
		default: return "unknown_shader_group_type(" + _to_string((int)subtype.shaderGroupType) + ")";
		}
	default:
		return "reserved=" + _to_string((unsigned long long)subtype.reserved);
	}
}

static void print_markings_entry(const collected_markings_entry& entry)
{
	const bool ignored = is_capture_flush_leftover_markings(entry);
	auto print_line = [&](const std::string& line)
	{
		if (ignored) printf(MAKEGRAY("%s\n"), line.c_str());
		else printf("%s\n", line.c_str());
	};

	std::string header = markings_location_string(entry);
	if (ignored) header += " [ignored capture leftover]";
	print_line(header);
	if (entry.source.call_id != UINT16_MAX)
	{
		print_line("  source: " + describe_change_source(entry.source));
	}
	else
	{
		print_line("  source: frame " + _to_string((unsigned)entry.source.frame)
			+ " call " + _to_string((unsigned)entry.source.call)
			+ " thread " + _to_string((unsigned)entry.source.thread)
			+ " (call id unknown)");
	}
	print_line("  packet_type: " + std::string(packet_type_name(entry.instrtype)));
	if (!entry.markings)
	{
		print_line("  markings: <missing>");
		return;
	}
	print_line("  count: " + _to_string((unsigned)entry.markings->count));
	for (uint32_t i = 0; i < entry.markings->count; i++)
	{
		const VkMarkingTypeARM type = entry.markings->pMarkingTypes ? entry.markings->pMarkingTypes[i] : (VkMarkingTypeARM)0;
		const VkMarkingSubTypeARM subtype = entry.markings->pSubTypes ? entry.markings->pSubTypes[i] : VkMarkingSubTypeARM{};
		const VkDeviceSize offset = entry.markings->pOffsets ? entry.markings->pOffsets[i] : 0;
		print_line("    [" + _to_string((unsigned)i) + "] offset=" + _to_string((unsigned long long)offset)
			+ " type=" + marking_type_string(type)
			+ " subtype=" + marking_subtype_string(type, subtype));
	}
}

static bool markings_entry_less(const collected_markings_entry& a, const collected_markings_entry& b)
{
	if (a.source.thread != b.source.thread) return a.source.thread < b.source.thread;
	if (a.packet != b.packet) return a.packet < b.packet;
	return a.occurrence < b.occurrence;
}

static std::string markings_semantic_key(const collected_markings_entry& entry)
{
	std::string key = _to_string((unsigned)entry.instrtype);
	if (!entry.markings)
	{
		key += "|missing";
		return key;
	}

	const VkMarkedOffsetsARM* markings = entry.markings;
	key += "|count=" + _to_string((unsigned)markings->count);
	key += "|types=" + _to_string(markings->pMarkingTypes ? 1 : 0);
	key += "|subtypes=" + _to_string(markings->pSubTypes ? 1 : 0);
	key += "|offsets=" + _to_string(markings->pOffsets ? 1 : 0);
	for (uint32_t i = 0; i < markings->count; i++)
	{
		const uint32_t type = markings->pMarkingTypes ? (uint32_t)markings->pMarkingTypes[i] : 0;
		const uint64_t subtype = markings->pSubTypes ? markings->pSubTypes[i].reserved : 0;
		const uint64_t offset = markings->pOffsets ? markings->pOffsets[i] : 0;
		key += "|" + _to_string((unsigned long long)offset)
			+ "," + _to_string((unsigned)type)
			+ "," + _to_string((unsigned long long)subtype);
	}
	return key;
}

static bool markings_entry_semantic_less(const collected_markings_entry& a, const collected_markings_entry& b)
{
	const std::string a_key = markings_semantic_key(a);
	const std::string b_key = markings_semantic_key(b);
	return a_key < b_key;
}

static void free_collected_markings(std::vector<collected_markings_entry>& entries)
{
	for (collected_markings_entry& entry : entries)
	{
		free_marked_offsets(entry.markings);
		entry.markings = nullptr;
	}
}

static void collect_markings_observer(const change_source& source, const VkMarkedOffsetsARM* markings, void* userdata)
{
	if (!userdata || !markings) return;
	markings_collect_state& state = *(markings_collect_state*)userdata;
	collected_markings_entry entry;
	entry.source = source;
	entry.packet = state.packet;
	entry.instrtype = state.instrtype;
	entry.occurrence = state.occurrence++;
	entry.markings = clone_marked_offsets(markings);
	sort_marked_offsets(entry.markings);
	state.out->push_back(entry);
}

static std::vector<collected_markings_entry> collect_trace_markings(const std::string& pack)
{
	std::vector<collected_markings_entry> out;
	retrace_reset_all();
	lava_reader reader;
	markings_collect_state state;
	state.out = &out;
	reader.run = false;
	reader.write_output = false;
	reader.validate = false;
	reader.pass = 0;
	reader.create_results_file = false;
	reader.markings_observer = collect_markings_observer;
	reader.markings_observer_data = &state;
	reader.init(pack);

	const std::vector<std::string> thread_files = packed_files(pack, "thread_");
	for (uint16_t thread_id = 0; thread_id < thread_files.size(); thread_id++)
	{
		lava_file_reader& t = reader.file_reader(thread_id);
		state.packet = 0;
		state.instrtype = 0;
		state.occurrence = 0;
		while (true)
		{
			const uint8_t instrtype = t.step();
			if (instrtype == 0) break;
			state.packet++;
			state.instrtype = instrtype;
			state.occurrence = 0;
			switchboard_packet(instrtype, t);
		}
	}
	reader.markings_observer = nullptr;
	reader.markings_observer_data = nullptr;
	std::sort(out.begin(), out.end(), markings_entry_less);
	retrace_reset_all();
	return out;
}

static markings_compare_result compare_packed_file_markings(const std::string& pack_a, const std::string& pack_b)
{
	markings_compare_result result;
	std::vector<collected_markings_entry> markings_a = collect_trace_markings(pack_a);
	std::vector<collected_markings_entry> markings_b = collect_trace_markings(pack_b);
	markings_a.erase(std::remove_if(markings_a.begin(), markings_a.end(), is_capture_flush_leftover_markings), markings_a.end());
	markings_b.erase(std::remove_if(markings_b.begin(), markings_b.end(), is_capture_flush_leftover_markings), markings_b.end());
	std::sort(markings_a.begin(), markings_a.end(), markings_entry_semantic_less);
	std::sort(markings_b.begin(), markings_b.end(), markings_entry_semantic_less);

	size_t i = 0;
	size_t j = 0;
	while (i < markings_a.size() && j < markings_b.size())
	{
		const collected_markings_entry& a = markings_a[i];
		const collected_markings_entry& b = markings_b[j];
		if (markings_entry_semantic_less(a, b))
		{
			result.identical = false;
			result.messages.push_back("Markings only in A at " + markings_location_string(a));
			break;
		}
		if (markings_entry_semantic_less(b, a))
		{
			result.identical = false;
			result.messages.push_back("Markings only in B at " + markings_location_string(b));
			break;
		}

		const marked_offsets_difference diff = compare_marked_offsets(a.markings, b.markings);
		if (diff != marked_offsets_difference::none)
		{
			result.identical = false;
			result.messages.push_back("Markings differ at " + markings_location_string(a) + ": " + packtool_marked_offsets_difference_string(diff));
			break;
		}
		i++;
		j++;
	}

	if (result.identical && i < markings_a.size())
	{
		result.identical = false;
		result.messages.push_back("Markings only in A at " + markings_location_string(markings_a[i]));
	}
	if (result.identical && j < markings_b.size())
	{
		result.identical = false;
		result.messages.push_back("Markings only in B at " + markings_location_string(markings_b[j]));
	}

	free_collected_markings(markings_a);
	free_collected_markings(markings_b);
	return result;
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
		bool assert_markings = false;
		std::vector<const char*> positional;
		for (int i = 2; i < argc; i++)
		{
			if (strcmp(argv[i], "--semantic") == 0) semantic = true;
			else if (strcmp(argv[i], "--assert-markings") == 0) assert_markings = true;
			else if (strcmp(argv[i], "--assert-sane") == 0)
			{
				assert_sane = true;
				semantic = true;
			}
			else positional.push_back(argv[i]);
		}
		if (positional.size() != 2) return usage(argv[0]);

		if (assert_markings && !semantic && !assert_sane)
		{
			markings_compare_result markings = compare_packed_file_markings(positional[0], positional[1]);
			if (markings.identical)
			{
				printf("Markings identical: %s and %s\n", positional[0], positional[1]);
				return 0;
			}
			for (const std::string& message : markings.messages)
			{
				printf("%s\n", message.c_str());
			}
			return 1;
		}

		if (semantic)
		{
			semantic_compare_result result = compare_packed_file_semantic(positional[0], positional[1]);
			if (assert_markings)
			{
				markings_compare_result markings = compare_packed_file_markings(positional[0], positional[1]);
				if (!markings.identical)
				{
					result.identical = false;
					result.sane = false;
					result.unexpected_differences++;
					for (const std::string& message : markings.messages) result.messages.push_back(message);
				}
			}
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
				if (assert_markings)
				{
					markings_compare_result markings = compare_packed_file_markings(positional[0], positional[1]);
					if (!markings.identical)
					{
						for (const std::string& message : markings.messages) printf("%s\n", message.c_str());
						return 1;
					}
				}
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
			if (assert_markings)
			{
				markings_compare_result markings = compare_packed_file_markings(positional[0], positional[1]);
				for (const std::string& message : markings.messages) printf("%s\n", message.c_str());
			}
			return 1;
		}

		if (assert_markings)
		{
			markings_compare_result markings = compare_packed_file_markings(positional[0], positional[1]);
			for (const std::string& message : markings.messages) printf("%s\n", message.c_str());
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
	else if (strcmp(argv[1], "list-markings") == 0)
	{
		if (argc != 3) return usage(argv[0]);
		std::vector<collected_markings_entry> markings = collect_trace_markings(argv[2]);
		if (markings.empty())
		{
			printf("No markings found in %s\n", argv[2]);
			return 0;
		}
		for (size_t i = 0; i < markings.size(); i++)
		{
			if (i > 0) printf("\n");
			print_markings_entry(markings[i]);
		}
		size_t ignored = 0;
		for (const auto& entry : markings) if (is_capture_flush_leftover_markings(entry)) ignored++;
		printf("\nTotal markings blocks: %zu", markings.size());
		if (ignored > 0) printf(" (%zu ignored capture leftovers)", ignored);
		printf("\n");
		free_collected_markings(markings);
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
	else if (strcmp(argv[1], "convert") == 0)
	{
		if (argc != 4) return usage(argv[0]);
		convert_packfile_to_zip(argv[2], argv[3]);
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
		int fd = open(argv[2], O_CREAT | O_TRUNC | O_WRONLY | default_file_flags, 0664);
		if (fd == -1) FAIL("Failed to create target %s: %s", argv[2], strerror(errno));
		std::vector<char> buffer(1024 * 1024);
		uint64_t remaining = pf.size();
		while (remaining > 0)
		{
			const size_t chunk = std::min<uint64_t>(buffer.size(), remaining);
			pf.read(buffer.data(), chunk);
			size_t written = 0;
			while (written < chunk)
			{
				const ssize_t res = write(fd, buffer.data() + written, chunk - written);
				if (res == -1 && errno == EINTR) continue;
				if (res <= 0) FAIL("Failed to write out the file: %s\n", strerror(errno));
				written += res;
			}
			remaining -= chunk;
		}
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
