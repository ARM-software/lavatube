#include "litert_lm_model.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <vector>

// Only the small, stable FlatBuffer tables needed for system metadata are read.
// Offsets are checked before access; model files are application input.
class litert_model_header
{
public:
	std::vector<uint8_t> bytes;
	bool valid = true;

	uint64_t number(size_t offset, size_t count)
	{
		if (offset > bytes.size() || count > bytes.size() - offset)
		{
			valid = false;
			return 0;
		}
		uint64_t value = 0;
		for (size_t i = 0; i < count; i++) value |= uint64_t(bytes[offset + i]) << (8 * i);
		return value;
	}

	size_t reference(size_t offset)
	{
		const uint64_t relative = number(offset, 4);
		if (!relative || relative > bytes.size() || offset > bytes.size() - relative)
		{
			valid = false;
			return 0;
		}
		return offset + relative;
	}

	size_t field(size_t table, unsigned index)
	{
		const int32_t displacement = static_cast<int32_t>(number(table, 4));
		const int64_t vtable = int64_t(table) - displacement;
		if (vtable < 0 || uint64_t(vtable) >= bytes.size())
		{
			valid = false;
			return 0;
		}
		const size_t length = number(vtable, 2);
		if (length < 4 || length > bytes.size() - vtable)
		{
			valid = false;
			return 0;
		}
		if (4 + index * 2 + 2 > length) return 0;
		const size_t offset = number(vtable + 4 + index * 2, 2);
		if (!offset) return 0;
		const size_t object_size = number(vtable + 2, 2);
		if (offset >= object_size || table > bytes.size() || object_size > bytes.size() - table)
		{
			valid = false;
			return 0;
		}
		return table + offset;
	}

	std::string string(size_t offset)
	{
		if (!offset) return {};
		const size_t start = reference(offset);
		const size_t length = number(start, 4);
		if (!valid || start + 4 > bytes.size() || length >= bytes.size() - start - 4)
		{
			valid = false;
			return {};
		}
		if (bytes[start + 4 + length] != 0)
		{
			valid = false;
			return {};
		}
		return std::string(reinterpret_cast<const char*>(bytes.data() + start + 4), length);
	}

	std::string uuid()
	{
		const size_t root = reference(0);
		const size_t system_field = field(root, 0);
		if (!valid || !system_field) return {};
		const size_t system = reference(system_field);
		const size_t entries_field = field(system, 0);
		if (!valid || !entries_field) return {};
		const size_t entries = reference(entries_field);
		const size_t count = number(entries, 4);
		if (!valid || entries + 4 > bytes.size() || count > (bytes.size() - entries - 4) / 4)
		{
			valid = false;
			return {};
		}
		std::string result;
		for (size_t i = 0; valid && i < count; i++)
		{
			const size_t entry = reference(entries + 4 + i * 4);
			const std::string key = string(field(entry, 0));
			if (key != "uuid") continue;
			const size_t type = field(entry, 1);
			const size_t value = field(entry, 2);
			if (!type || number(type, 1) != 9 || !value)
			{
				valid = false;
				break;
			}
			result = string(field(reference(value), 0));
		}
		return result;
	}
};

static std::string filename_part(const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A cheap identity fingerprint, not a whole-file integrity checksum. The name
// records the algorithm and sampling scheme so it can change independently.
static void fingerprint_bytes(uint64_t& hash, const std::vector<uint8_t>& bytes)
{
	for (uint8_t byte : bytes)
	{
		hash ^= byte;
		hash *= UINT64_C(1099511628211);
	}
}

bool inspect_litert_lm_model(const std::string& path, litert_lm_model_identity& identity)
{
	identity = {};
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return false;
	struct stat st;
	uint8_t prefix[32];
	if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 32
	    || fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix) || memcmp(prefix, "LITERTLM", 8) != 0)
	{
		fclose(file);
		return false;
	}
	identity.size = st.st_size;
	uint64_t end = 0;
	for (unsigned i = 0; i < 8; i++) end |= uint64_t(prefix[24 + i]) << (8 * i);
	// Bound allocation when examining malformed or unrelated files.
	if (end <= 32 || end > identity.size || end - 32 > 16 * 1024 * 1024)
	{
		fclose(file);
		return false;
	}
	litert_model_header header;
	header.bytes.resize(end - 32);
	if (fread(header.bytes.data(), 1, header.bytes.size(), file) != header.bytes.size())
	{
		fclose(file);
		return false;
	}
	identity.uuid = header.uuid();
	if (!header.valid)
	{
		fclose(file);
		return false;
	}
	uint64_t hash = UINT64_C(14695981039346656037);
	fingerprint_bytes(hash, std::vector<uint8_t>(prefix, prefix + sizeof(prefix)));
	fingerprint_bytes(hash, header.bytes);
	const uint64_t payload_size = identity.size - end;
	const size_t sample_size = std::min<uint64_t>(payload_size, 64 * 1024);
	const uint64_t offsets[] = { end, end + (payload_size - sample_size) / 2, identity.size - sample_size };
	std::vector<uint8_t> sample(sample_size);
	for (uint64_t offset : offsets)
	{
		if (fseeko(file, offset, SEEK_SET) != 0 || fread(sample.data(), 1, sample.size(), file) != sample.size())
		{
			fclose(file);
			return false;
		}
		fingerprint_bytes(hash, sample);
	}
	fclose(file);
	char fingerprint[64];
	snprintf(fingerprint, sizeof(fingerprint), "sampled-fnv1a64-v1:%016llx", (unsigned long long)hash);
	identity.fingerprint = fingerprint;
	const size_t slash = path.find_last_of("/\\");
	identity.model_id = slash == std::string::npos ? filename_part(path) : filename_part(path.substr(0, slash));
	return true;
}

static bool matching_model(const std::string& candidate, const litert_lm_model_identity& expected)
{
	struct stat st;
	if (stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) != expected.size) return false;
	litert_lm_model_identity actual;
	if (!inspect_litert_lm_model(candidate, actual)) return false;
	// Check the samples even with a UUID, catching common accidental edits that
	// preserve a packaging UUID. Neither check claims whole-file integrity.
	return actual.fingerprint == expected.fingerprint && (expected.uuid.empty() || actual.uuid == expected.uuid);
}

static void model_candidates(const std::string& directory, std::vector<std::string>& candidates, bool descend)
{
	DIR* dir = opendir(directory.c_str());
	if (!dir) return;
	struct dirent* entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		const std::string path = directory + "/" + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode) && descend) model_candidates(path, candidates, false);
		else if (S_ISREG(st.st_mode) && name.size() >= 9 && name.compare(name.size() - 9, 9, ".litertlm") == 0) candidates.push_back(path);
	}
	closedir(dir);
}

bool find_litert_lm_model(const std::string& filename, const litert_lm_model_identity& identity,
	const char* override_path, std::string& path, std::string& error)
{
	path.clear();
	error.clear();
	if (!identity.size || identity.fingerprint.empty())
	{
		error = "Trace has no LiteRT-LM model identity; recapture with model identification enabled";
		return false;
	}
	std::vector<std::string> roots;
	if (override_path && *override_path)
	{
		struct stat st;
		if (stat(override_path, &st) != 0)
		{
			error = std::string("LiteRT-LM model override does not exist: ") + override_path;
			return false;
		}
		if (!S_ISDIR(st.st_mode))
		{
			if (matching_model(override_path, identity))
			{
				path = override_path;
				return true;
			}
			error = std::string("LiteRT-LM model override does not match captured model '") + identity.model_id + "': " + override_path;
			return false;
		}
		roots.push_back(override_path);
	}
	else
	{
		roots.push_back(".");
		const char* cli_dir = getenv("LITERT_LM_DIR");
		const char* home = getenv("HOME");
		if (cli_dir && *cli_dir) roots.push_back(std::string(cli_dir) + "/models");
		else if (home && *home) roots.push_back(std::string(home) + "/.litert-lm/models");
		roots.push_back("/opt/litert-lm/models");
	}
	for (const std::string& root : roots)
	{
		std::vector<std::string> candidates;
		// Recorded names are hints only; reject path components from a trace.
		if (!identity.model_id.empty() && identity.model_id != "." && identity.model_id != ".."
		    && identity.model_id == filename_part(identity.model_id))
		{
			candidates.push_back(root + "/" + identity.model_id + "/" + filename_part(filename));
		}
		candidates.push_back(root + "/" + filename_part(filename));
		const size_t hints = candidates.size();
		model_candidates(root, candidates, root != ".");
		std::sort(candidates.begin() + hints, candidates.end());
		for (const std::string& candidate : candidates)
		{
			if (!matching_model(candidate, identity)) continue;
			path = candidate;
			return true;
		}
	}
	error = "No matching LiteRT-LM model found for '" + identity.model_id + "' (" + filename
		+ ", UUID " + (identity.uuid.empty() ? "absent" : identity.uuid) + ", " + identity.fingerprint
		+ "). Use --litert-model-path or LITERT_MODEL_PATH to select the matching file or directory";
	return false;
}
