#pragma once

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdint.h>
#include <vector>
#include <cstring>
#include "jsoncpp/json/value.h"
#include "zipc.h"

#define FAIL(_format, ...) do { fprintf(stderr, "%s:%d " _format "\n", __FILE__, __LINE__, ## __VA_ARGS__); exit(-1); } while(0)

struct packed
{
	inline uint64_t size() const { return filesize; }

	inline void read(void* ptr, uint_fast32_t _size)
	{
		assert(_size <= filesize - consumed);
		assert(zip_handle);
		memcpy(ptr, static_cast<const char*>(zip_mapping.data) + consumed, _size);
		consumed += _size;
	}

	inline void close()
	{
		if (zip_handle)
		{
			zipc_unmap_read(zip_handle, zip_mapping);
			zipc_close(zip_handle);
			zip_handle = nullptr;
			zip_mapping = {};
		}
		filesize = 0;
		consumed = 0;
	}

	// treat these as private:
	uint64_t filesize = 0;
	uint64_t consumed = 0;
	zipc* zip_handle = nullptr;
	zipc_mapping zip_mapping = {};
	std::string inside;
	std::string pack;
};

/// Open a file inside a pack file
packed packed_open(const std::string& inside, const std::string& pack);

/// Convenience function to get a file inside a pack file as a JSON value
Json::Value packed_json(const std::string& inside, const std::string& pack);

/// Delete all files in the given directory and then the directory itself
void erase_directory(const std::string& directory);

/// Collapse all files in the given directory into a pack file, optionally erasing it afterward
bool pack_directory(const std::string& inside, const std::string& directory, bool erase = true);

/// Unpack all files inside a pack file to a directory (for debugging)
bool unpack_directory(const std::string& inside, const std::string& directory);

/// Put list of contents to stdout
void packed_list(const std::string& pack);

/// Return list of files, optionally filtering by a starting string that must match.
std::vector<std::string> packed_files(const std::string& pack, const std::string& startsWith = std::string());

/// Append a new file to a trace container
bool pack_add(const std::string& newfile, const std::string& pack);
