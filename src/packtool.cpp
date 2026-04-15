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

int main(int argc, char* argv[])
{
	if (argc < 3 || (argc < 4 && strcmp(argv[1], "list") != 0 && strcmp(argv[1], "check") != 0)
	    || (argc > 4 && strcmp(argv[1], "diff") == 0)
	    || (argc > 3 && strcmp(argv[1], "diff") != 0 && (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "check") == 0)))
	{
		fprintf(stdout, "Usage:\n");
		fprintf(stdout, "\t%s pack <output file> <input directory>\n", argv[0]);
		fprintf(stdout, "\t%s unpack <output directory> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s extract <output file> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s add <input file> <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s list <input packaged file>\n", argv[0]);
		fprintf(stdout, "\t%s print <target file> <input packaged file>\n", argv[0]); // pretty print a JSON
		fprintf(stdout, "\t%s check <input packaged file>\n", argv[0]); // sanitycheck contents
		fprintf(stdout, "\t%s diff <input packaged file A> <input packaged file B>\n", argv[0]);
		return 0;
	}
	else if (strcmp(argv[1], "diff") == 0)
	{
		std::vector<std::string> files_a = packed_files(argv[2]);
		std::vector<std::string> files_b = packed_files(argv[3]);
		std::set<std::string> set_a(files_a.begin(), files_a.end());
		std::set<std::string> set_b(files_b.begin(), files_b.end());
		bool different = false;
		unsigned identical_files = 0;
		unsigned differing_files = 0;

		for (const std::string& name : set_a)
		{
			if (set_b.count(name) == 0)
			{
				printf("Only in %s: %s\n", argv[2], name.c_str());
				different = true;
			}
		}
		for (const std::string& name : set_b)
		{
			if (set_a.count(name) == 0)
			{
				printf("Only in %s: %s\n", argv[3], name.c_str());
				different = true;
			}
		}

		for (const std::string& name : set_a)
		{
			if (set_b.count(name) == 0) continue;
			packed_compare_result result = compare_packed_file(argv[2], argv[3], name);
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
			packed_compare_result raw = compare_regular_file(argv[2], argv[3]);
			if (raw.equal)
			{
				printf("Identical: %s and %s (%u files)\n", argv[2], argv[3], identical_files);
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
		if (!pack_directory(argv[2], argv[3], false))
		{
			FAIL("Failed to pack directory \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "add") == 0)
	{
		if (!pack_add(argv[2], argv[3]))
		{
			FAIL("Failed to add \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "unpack") == 0)
	{
		if (!unpack_directory(argv[3], argv[2]))
		{
			FAIL("Failed to unpack file \"%s\" into \"%s\"", argv[3], argv[2]);
		}
		return 0;
	}
	else if (strcmp(argv[1], "print") == 0)
	{
		Json::Value js = packed_json(argv[2], argv[3]);
		printf("%s", js.toStyledString().c_str());
		return 0;
	}
	else if (strcmp(argv[1], "extract") == 0)
	{
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
		packed_list(argv[2]);
		return 0;
	}
	fprintf(stderr, "Unrecognized command: %s\n", argv[1]);
	return -1;
}
