#pragma once

#include <assert.h>
#include <string>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <vector>
#include "jsoncpp/json/value.h"

#define FAIL(_format, ...) do { fprintf(stderr, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); exit(-1); } while(0)

struct packed
{
	inline uint64_t size() const { return filesize; }

	inline void read(void* ptr, uint_fast32_t _size)
	{
		assert(_size <= filesize - consumed);
		size_t actually_read = 0;
		errno = 0;
		do
		{
			ssize_t res = ::read(fd, ptr, _size - actually_read);
			if (res > 0) actually_read += res;
		} while (actually_read < _size && (errno == EWOULDBLOCK || errno == EINTR));
		assert(actually_read == _size);
		consumed += _size;
	}

	inline void close() { ::close(fd); fd = 0; filesize = 0; consumed = 0; }

	// treat these as private:
	uint64_t filesize = 0;
	uint64_t consumed = 0;
	uint64_t last_idx_ptr_pos = 0; // position of last next-index pointer, if non-zero
	int fd = -1;
	int version = 0;
	std::string inside;
	std::string pack;
};

/// Open a file inside a pack file
packed packed_open(const std::string& inside, const std::string& pack);

/// Convenience function to get a file inside a pack file as a JSON value
Json::Value packed_json(const std::string& inside, const std::string& pack);

/// Collapse all files in the given directory into a pack file, optionally erasing it afterward
bool pack_directory(const std::string& inside, const std::string& directory, bool erase = true);

/// Unpack all files inside a pack file to a directory (for debugging)
bool unpack_directory(const std::string& inside, const std::string& directory);

/// Put list of contents to stdout
void packed_list(const std::string& pack);

/// Return list of files, optionally filtering by a starting string that must match.
std::vector<std::string> packed_files(const std::string& pack, const std::string& startsWith = std::string());

/// Append a new file to a packfile
bool pack_add(const std::string& newfile, const std::string& pack);
