#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "jsoncpp/json/value.h"
#include "file_format.h"
#include "packfile.h"
#include "util.h"

struct random_access_chunk_info
{
	/// Offset of the compressed payload from the start of the mapped stream entry.
	uint64_t compressed_offset = 0;
	uint64_t compressed_size = 0;
	/// Offset of this chunk in the logical uncompressed stream.
	uint64_t uncompressed_offset = 0;
	uint64_t uncompressed_size = 0;
};

/// Synchronous seekable reader for a chunk-compressed lavatube stream. Construction scans only
/// the chunk headers. seek() does not decompress data, and reads retain only one decompressed
/// chunk at a time.
class random_access_file_reader
{
	random_access_file_reader(const random_access_file_reader&) = delete;
	random_access_file_reader& operator=(const random_access_file_reader&) = delete;

public:
	explicit random_access_file_reader(const std::string& filename);
	/// Takes ownership of the packed mapping and handle.
	explicit random_access_file_reader(packed pf);
	~random_access_file_reader();

	void seek(uint64_t position);
	/// Load packet checkpoints from frames_N.json. Missing checkpoint data is valid for legacy traces.
	void load_packet_checkpoints(const Json::Value& frame_info);
	/// Seek to the closest checkpoint at or before packet and return that checkpoint's packet index.
	/// Without checkpoint data, seeks to the start and returns packet zero.
	uint32_t seek_to_packet(uint32_t packet);
	void read_bytes(void* destination, uint64_t byte_count);

	inline uint8_t read_uint8_t() { return read_value<uint8_t>(); }
	inline uint16_t read_uint16_t() { return read_value<uint16_t>(); }
	inline uint32_t read_uint32_t() { return read_value<uint32_t>(); }
	inline uint64_t read_uint64_t() { return read_value<uint64_t>(); }
	inline int8_t read_int8_t() { return read_value<int8_t>(); }
	inline int16_t read_int16_t() { return read_value<int16_t>(); }
	inline int32_t read_int32_t() { return read_value<int32_t>(); }
	inline int64_t read_int64_t() { return read_value<int64_t>(); }

	inline float read_float()
	{
		const uint32_t stored = read_uint32_t();
		float value;
		memcpy(&value, &stored, sizeof(value));
		return value;
	}

	inline double read_double()
	{
		const uint64_t stored = read_uint64_t();
		double value;
		memcpy(&value, &stored, sizeof(value));
		return value;
	}

	inline size_t read_size_t() { return static_cast<size_t>(read_uint64_t()); }
	inline int read_int() { return static_cast<int>(read_uint32_t()); }
	inline long read_long() { return static_cast<long>(read_uint64_t()); }

	template <typename T> void read_array(T* values, size_t count)
	{
		assert(values || count == 0);
		if (count > SIZE_MAX / sizeof(T)) ABORT("Random-access read array size overflow");
		read_bytes(values, sizeof(T) * count);
	}

	std::string read_string();

	uint64_t position() const { return mPosition; }
	uint64_t size() const { return mTotalUncompressed; }
	uint64_t remaining() const { return mTotalUncompressed - mPosition; }
	uint8_t version() const { return mStreamVersion; }
	uint8_t compression_algorithm() const { return mCompressionAlgorithm; }
	uint64_t decompression_count() const { return mDecompressionCount; }
	const std::vector<random_access_chunk_info>& chunks() const { return mChunks; }
	const std::vector<packet_checkpoint>& packet_checkpoints() const { return mPacketCheckpoints; }
	bool has_packet_checkpoints() const { return !mPacketCheckpoints.empty(); }

private:
	template <typename T> T read_value()
	{
		T value;
		read_bytes(&value, sizeof(value));
		return value;
	}

	void initialize();
	size_t find_chunk(uint64_t position) const;
	void load_chunk(size_t chunk_index);

	const char* mMappedData = nullptr;
	uint64_t mMappedSize = 0;
	void* mDirectMapping = nullptr;
	zipc* mZipHandle = nullptr;
	zipc_mapping mZipMapping = {};
	std::string mFilename;
	std::vector<random_access_chunk_info> mChunks;
	std::vector<packet_checkpoint> mPacketCheckpoints;
	std::vector<char> mChunkData;
	uint64_t mTotalUncompressed = 0;
	uint64_t mPosition = 0;
	size_t mLoadedChunk = SIZE_MAX;
	uint64_t mDecompressionCount = 0;
	uint8_t mCompressionAlgorithm = LAVATUBE_COMPRESSION_DENSITY;
	uint8_t mStreamVersion = 0;
};
