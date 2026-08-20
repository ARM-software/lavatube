#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <lz4.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>

#include "density/src/density_api.h"
#include "random_access_file_reader.h"

random_access_file_reader::random_access_file_reader(const std::string& filename)
	: mFilename(filename)
{
	const int fd = open(filename.c_str(), O_RDONLY | default_file_flags);
	if (fd == -1) ABORT("Cannot open random-access input file \"%s\": %s", filename.c_str(), strerror(errno));

	struct stat64 status;
	if (fstat64(fd, &status) == -1)
	{
		close(fd);
		ABORT("Failed to stat random-access input file \"%s\": %s", filename.c_str(), strerror(errno));
	}
	if (status.st_size <= 0)
	{
		close(fd);
		ABORT("Random-access input file \"%s\" is empty", filename.c_str());
	}

	mMappedSize = static_cast<uint64_t>(status.st_size);
	mDirectMapping = mmap(nullptr, mMappedSize, PROT_READ, MAP_PRIVATE, fd, 0);
	const int close_result = close(fd);
	if (close_result != 0) ABORT("Failed to close random-access input file \"%s\": %s", filename.c_str(), strerror(errno));
	if (mDirectMapping == MAP_FAILED)
	{
		mDirectMapping = nullptr;
		ABORT("Failed to map random-access input file \"%s\": %s", filename.c_str(), strerror(errno));
	}
	mMappedData = static_cast<const char*>(mDirectMapping);
	initialize();
}

random_access_file_reader::random_access_file_reader(packed pf)
	: mMappedData(static_cast<const char*>(pf.zip_mapping.data)),
	  mMappedSize(pf.filesize),
	  mZipHandle(pf.zip_handle),
	  mZipMapping(pf.zip_mapping),
	  mFilename(pf.inside + " inside " + pf.pack)
{
	if (!mZipHandle || !mMappedData || mMappedSize == 0)
	{
		ABORT("Invalid packed random-access input for \"%s\"", mFilename.c_str());
	}
	initialize();
}

random_access_file_reader::~random_access_file_reader()
{
	if (mZipHandle)
	{
		zipc_unmap_read(mZipHandle, mZipMapping);
		zipc_close(mZipHandle);
	}
	else if (mDirectMapping)
	{
		munmap(mDirectMapping, mMappedSize);
	}
}

void random_access_file_reader::initialize()
{
	const char magic[] = "LAVABIN";
	const uint64_t stream_metadata_size = 32;
	const uint64_t chunk_header_size = sizeof(uint64_t) * 2;
	uint64_t stream_offset = 0;
	const uint64_t magic_size = sizeof(magic) - 1;
	if (mMappedSize >= magic_size && memcmp(mMappedData, magic, magic_size) == 0)
	{
		const uint64_t header_size = magic_size + stream_metadata_size;
		if (mMappedSize < header_size)
		{
			ABORT("Truncated random-access stream header in \"%s\"", mFilename.c_str());
		}
		mStreamVersion = static_cast<uint8_t>(mMappedData[magic_size]);
		mCompressionAlgorithm = static_cast<uint8_t>(mMappedData[magic_size + 1]);
		stream_offset = header_size;
	}

	if (mCompressionAlgorithm != LAVATUBE_COMPRESSION_DENSITY
	    && mCompressionAlgorithm != LAVATUBE_COMPRESSION_LZ4
	    && mCompressionAlgorithm != LAVATUBE_COMPRESSION_UNCOMPRESSED)
	{
		ABORT("Invalid compression algorithm %u in random-access input \"%s\"",
		      static_cast<unsigned>(mCompressionAlgorithm), mFilename.c_str());
	}

	uint64_t uncompressed_offset = 0;
	while (stream_offset < mMappedSize)
	{
		if (mMappedSize - stream_offset < chunk_header_size)
		{
			ABORT("Truncated chunk header at offset %lu in random-access input \"%s\"",
			      static_cast<unsigned long>(stream_offset), mFilename.c_str());
		}

		uint64_t compressed_size = 0;
		uint64_t uncompressed_size = 0;
		memcpy(&compressed_size, mMappedData + stream_offset, sizeof(compressed_size));
		memcpy(&uncompressed_size, mMappedData + stream_offset + sizeof(compressed_size), sizeof(uncompressed_size));
		stream_offset += chunk_header_size;

		if (compressed_size == 0 || uncompressed_size == 0)
		{
			ABORT("Invalid empty chunk at offset %lu in random-access input \"%s\"",
			      static_cast<unsigned long>(stream_offset - chunk_header_size), mFilename.c_str());
		}
		if (compressed_size > mMappedSize - stream_offset)
		{
			ABORT("Chunk at offset %lu exceeds random-access input \"%s\"",
			      static_cast<unsigned long>(stream_offset - chunk_header_size), mFilename.c_str());
		}
		if (uncompressed_size > std::numeric_limits<uint64_t>::max() - uncompressed_offset)
		{
			ABORT("Uncompressed size overflow in random-access input \"%s\"", mFilename.c_str());
		}

		random_access_chunk_info info;
		info.compressed_offset = stream_offset;
		info.compressed_size = compressed_size;
		info.uncompressed_offset = uncompressed_offset;
		info.uncompressed_size = uncompressed_size;
		mChunks.push_back(info);

		stream_offset += compressed_size;
		uncompressed_offset += uncompressed_size;
	}

	if (mChunks.empty()) ABORT("Random-access input \"%s\" contains no chunks", mFilename.c_str());
	mTotalUncompressed = uncompressed_offset;
	if (mDirectMapping) madvise(mDirectMapping, mMappedSize, MADV_RANDOM);
	else madvise(const_cast<void*>(mZipMapping.map_base), mZipMapping.map_length, MADV_RANDOM);
}

size_t random_access_file_reader::find_chunk(uint64_t position) const
{
	assert(position < mTotalUncompressed);
	size_t first = 0;
	size_t last = mChunks.size();
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (mChunks[middle].uncompressed_offset <= position) first = middle + 1;
		else last = middle;
	}
	assert(first > 0);
	const size_t chunk_index = first - 1;
#ifdef DEBUG
	const random_access_chunk_info& info = mChunks[chunk_index];
	assert(position < info.uncompressed_offset + info.uncompressed_size);
#endif
	return chunk_index;
}

void random_access_file_reader::load_chunk(size_t chunk_index)
{
	if (mLoadedChunk == chunk_index) return;
	const random_access_chunk_info& info = mChunks.at(chunk_index);
	const char* source = mMappedData + info.compressed_offset;

	if (info.uncompressed_size > SIZE_MAX)
	{
		ABORT("Chunk %zu in random-access input \"%s\" is too large", chunk_index, mFilename.c_str());
	}

	if (mCompressionAlgorithm == LAVATUBE_COMPRESSION_DENSITY)
	{
		const uint64_t destination_size = density_decompress_safe_size(info.uncompressed_size);
		if (destination_size > SIZE_MAX)
		{
			ABORT("Density output for chunk %zu in random-access input \"%s\" is too large", chunk_index, mFilename.c_str());
		}
		mChunkData.resize(static_cast<size_t>(destination_size));
		const density_processing_result result = density_decompress(
			reinterpret_cast<const uint8_t*>(source), info.compressed_size,
			reinterpret_cast<uint8_t*>(mChunkData.data()), destination_size);
		if (result.state != DENSITY_STATE_OK || result.bytesRead > info.compressed_size || result.bytesWritten != info.uncompressed_size)
		{
			ABORT("Failed to decompress density chunk %zu in random-access input \"%s\": state=%u read=%lu/%lu written=%lu/%lu",
			      chunk_index, mFilename.c_str(), static_cast<unsigned>(result.state),
			      static_cast<unsigned long>(result.bytesRead), static_cast<unsigned long>(info.compressed_size),
			      static_cast<unsigned long>(result.bytesWritten), static_cast<unsigned long>(info.uncompressed_size));
		}
	}
	else
	{
		mChunkData.resize(static_cast<size_t>(info.uncompressed_size));
		if (mCompressionAlgorithm == LAVATUBE_COMPRESSION_LZ4)
		{
			if (info.compressed_size > INT_MAX || info.uncompressed_size > INT_MAX)
			{
				ABORT("LZ4 chunk %zu in random-access input \"%s\" is too large", chunk_index, mFilename.c_str());
			}
			const int result = LZ4_decompress_safe(source, mChunkData.data(),
				static_cast<int>(info.compressed_size), static_cast<int>(info.uncompressed_size));
			if (result < 0 || static_cast<uint64_t>(result) != info.uncompressed_size)
			{
				ABORT("Failed to decompress LZ4 chunk %zu in random-access input \"%s\"", chunk_index, mFilename.c_str());
			}
		}
		else
		{
			assert(mCompressionAlgorithm == LAVATUBE_COMPRESSION_UNCOMPRESSED);
			// Version 3 file_writer streams include the chunk header size in compressed_size for
			// uncompressed chunks. The streaming reader consumes those streams by copying only
			// uncompressed_size bytes and advancing by the larger stored compressed_size.
			const uint64_t header_size = sizeof(uint64_t) * 2;
			const bool legacy_size_valid = info.uncompressed_size <= std::numeric_limits<uint64_t>::max() - header_size
				&& info.compressed_size == info.uncompressed_size + header_size;
			if (info.compressed_size != info.uncompressed_size && !legacy_size_valid)
			{
				ABORT("Uncompressed chunk %zu has mismatched sizes in random-access input \"%s\"", chunk_index, mFilename.c_str());
			}
			memcpy(mChunkData.data(), source, static_cast<size_t>(info.uncompressed_size));
		}
	}

	mLoadedChunk = chunk_index;
	mDecompressionCount++;
}

void random_access_file_reader::seek(uint64_t position)
{
	if (position > mTotalUncompressed)
	{
		ABORT("Cannot seek to %lu in %lu-byte random-access input \"%s\"",
		      static_cast<unsigned long>(position), static_cast<unsigned long>(mTotalUncompressed), mFilename.c_str());
	}
	mPosition = position;
}

void random_access_file_reader::load_packet_checkpoints(const Json::Value& frame_info)
{
	mPacketCheckpoints.clear();
	if (!frame_info.isMember("packet_checkpoints")) return;

	const Json::Value& values = frame_info["packet_checkpoints"];
	if (!values.isArray()) ABORT("Packet checkpoints are not an array for random-access input \"%s\"", mFilename.c_str());
	for (Json::ArrayIndex i = 0; i < values.size(); i++)
	{
		const Json::Value& value = values[i];
		if (!value.isObject() || !value.isMember("packet") || !value["packet"].isUInt()
		    || !value.isMember("position") || !value["position"].isUInt64())
		{
			ABORT("Invalid packet checkpoint %u for random-access input \"%s\"", i, mFilename.c_str());
		}

		packet_checkpoint checkpoint;
		checkpoint.packet = value["packet"].asUInt();
		checkpoint.position = value["position"].asUInt64();
		if (checkpoint.position >= mTotalUncompressed)
		{
			ABORT("Packet checkpoint %u exceeds random-access input \"%s\"", i, mFilename.c_str());
		}
		if (i == 0 && (checkpoint.packet != 0 || checkpoint.position != 0))
		{
			ABORT("First packet checkpoint is not packet zero at position zero for random-access input \"%s\"",
			      mFilename.c_str());
		}
		if (!mPacketCheckpoints.empty())
		{
			const packet_checkpoint& previous = mPacketCheckpoints.back();
			if (checkpoint.packet <= previous.packet || checkpoint.position <= previous.position)
			{
				ABORT("Packet checkpoints are not strictly increasing for random-access input \"%s\"", mFilename.c_str());
			}
		}
		mPacketCheckpoints.push_back(checkpoint);
	}
}

uint32_t random_access_file_reader::seek_to_packet(uint32_t packet)
{
	if (mPacketCheckpoints.empty())
	{
		seek(0);
		return 0;
	}

	size_t first = 0;
	size_t last = mPacketCheckpoints.size();
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (mPacketCheckpoints[middle].packet <= packet) first = middle + 1;
		else last = middle;
	}
	assert(first > 0);
	const packet_checkpoint& checkpoint = mPacketCheckpoints[first - 1];
	seek(checkpoint.position);
	return checkpoint.packet;
}

void random_access_file_reader::read_bytes(void* destination, uint64_t byte_count)
{
	if (!destination && byte_count != 0) ABORT("Null destination for random-access read");
	if (byte_count > remaining())
	{
		ABORT("Cannot read %lu bytes with %lu remaining in random-access input \"%s\"",
		      static_cast<unsigned long>(byte_count), static_cast<unsigned long>(remaining()), mFilename.c_str());
	}

	char* output = static_cast<char*>(destination);
	while (byte_count > 0)
	{
		const size_t chunk_index = find_chunk(mPosition);
		load_chunk(chunk_index);
		const random_access_chunk_info& info = mChunks[chunk_index];
		const uint64_t offset = mPosition - info.uncompressed_offset;
		const uint64_t available = info.uncompressed_size - offset;
		const uint64_t copy_size = std::min(byte_count, available);
		memcpy(output, mChunkData.data() + static_cast<size_t>(offset), static_cast<size_t>(copy_size));
		output += copy_size;
		mPosition += copy_size;
		byte_count -= copy_size;
	}
}

std::string random_access_file_reader::read_string()
{
	const uint16_t length = read_uint16_t();
	std::string value(length, '\0');
	if (length > 0) read_bytes(&value[0], length);
	return value;
}
