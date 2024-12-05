#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <errno.h>
#include <unistd.h>

#include "filewriter.h"
#include "density/src/density_api.h"

void file_writer::write_memory(const char* const ptr, uint64_t offset, uint64_t size)
{
	write_uint32_t(offset);
	write_uint32_t(size);
	check_space(size);
	char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
	memcpy(uptr, ptr + offset, size);
	uidx += size;
	assert(uidx <= chunk.size());
	uncompressed_bytes += size;
	write_uint32_t(0);
	write_uint32_t(0);
}

// memory areas should be 64bit aligned
// size is number of bytes to scan for changes
uint64_t file_writer::write_patch(char* __restrict__ orig, const char* __restrict__ chng, uint32_t offset, uint64_t size)
{
	uint64_t total_left = size;
	uint32_t c;
	uint64_t changed = 0;
	orig += offset;
	chng += offset;
	const char* startchng;
	while (total_left)
	{
		// Skip identical sequence
		for (; total_left >= 8 && *((uint64_t*)orig) == *((uint64_t*)chng); orig += 8, offset += 8, chng += 8, total_left -= 8) {}

		// Process difference sequence and update the clone
		startchng = chng;
		for (c = 0; total_left >= 8 && *((uint64_t*)orig) != *((uint64_t*)chng); orig += 8, chng += 8, c += 8, total_left -= 8) { *((uint64_t*)orig) = *((uint64_t*)chng); }

		// Check remainder
		if (total_left < 8 && memcmp(chng, orig, total_left) == 0) total_left = 0;
		else if (total_left < 8) { memcpy(orig, chng, total_left); c += total_left; total_left = 0; }

		if (c)
		{
			check_space(8 + c);
			char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
			memcpy(uptr, &offset, 4); // write offset
			uptr += 4;
			memcpy(uptr, &c, 4); // write size of patch
			uptr += 4;
			memcpy(uptr, startchng, c); // write payload
			uidx += 8 + c;
			uncompressed_bytes += 8 + c;
			changed += c;
			offset = 0; // offset is relative
		}
	}
	// terminate with zero offset, zero size
	assert(uidx <= chunk.size());
	write_uint32_t(0);
	write_uint32_t(0);
	return changed;
}

file_writer::file_writer() : done_feeding(false), done_compressing(false)
{
	uncompressed_chunk_size = p__chunksize;
	chunk = buffer(uncompressed_chunk_size);
}

void file_writer::set(const std::string& filename)
{
	mFilename = filename; // for debugging
	if (fp)
	{
		fclose(fp);
	}
	fp = fopen(filename.c_str(), "wb");
	if (!fp)
	{
		ELOG("Failed to create \"%s\": %s", filename.c_str(), strerror(errno));
	}

	// launch serialization threads
	if (!compressor_thread.joinable()) compressor_thread = std::thread(&file_writer::compressor, this);
	if (!serializer_thread.joinable()) serializer_thread = std::thread(&file_writer::serializer, this);
}

void file_writer::finalize()
{
	if (!fp)
	{
		return;
	}
	// whatever is left in our current buffer, move to work list
	chunk_mutex.lock();
	printf("Filewriter finalizing thread %u: %lu total bytes, %lu in last chunk, %d uncompressed chunks, and %d compressed chunks to be written out\n",
	       mTid, (unsigned long)uncompressed_bytes, (unsigned long)uidx, (int)uncompressed_chunks.size(), (int)compressed_chunks.size());
	chunk.shrink(uidx);
	if (!multithreaded_compress)
	{
		chunk = compress_chunk(chunk);
		if (multithreaded_write) compressed_chunks.push_front(chunk);
		else write_chunk(chunk);
	}
	else uncompressed_chunks.push_front(chunk);
	chunk = buffer(uncompressed_chunk_size); // ready to go again
	chunk_mutex.unlock();
	// wrap up work in work lists
	done_feeding.exchange(true);
	if (compressor_thread.joinable()) compressor_thread.join();
	done_compressing.exchange(true);
	if (serializer_thread.joinable()) serializer_thread.join();
	compressor_thread = std::thread();
	serializer_thread = std::thread();
	chunk_mutex.lock();
	assert(uncompressed_chunks.size() == 0);
	assert(compressed_chunks.size() == 0);
	chunk_mutex.unlock();
	int err = fclose(fp);
	if (err != 0)
	{
		ELOG("Failed to close out file: %s", strerror(err));
	}
	fp = nullptr;
}

file_writer::~file_writer()
{
	finalize();
	chunk.release();
}

void file_writer::write_chunk(buffer& active)
{
	size_t written = 0;
	size_t size = active.size();
	char* ptr = active.data();
	int err = 0;
	do {
		written = fwrite(ptr, 1, size, fp);
		ptr += written;
		size -= written;
		err = ferror(fp);
	} while (size > 0 && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR));
	if (size > 0)
	{
		ELOG("Failed to write out file (%u bytes left): %s", (unsigned)size, strerror(ferror(fp)));
	}
	DLOG3("Filewriter thread %d wrote out compressed buffer of %lu size\n", mTid, (unsigned long)active.size());
	active.release();
}

void file_writer::serializer()
{
	// lock, steal compressed buffer, unlock, store to disk, sleep, repeat
	set_thread_name("serializer");
	while (1)
	{
		buffer active;
		chunk_mutex.lock();
		// try to steal a compressed buffer
		if (compressed_chunks.size()) // we have buffers to compress
		{
			active = compressed_chunks.back();
			compressed_chunks.pop_back();
		}
		// note that we only exit thread if we have no more work to do
		else if (done_compressing)
		{
			chunk_mutex.unlock();
			break; // exit thread
		}
		chunk_mutex.unlock();
		// save compressed buffer
		if (active.size() > 0)
		{
			write_chunk(active);
		}
		// if not done and no work done, wait a bit
		else if (!done_compressing)
		{
			usleep(2000);
		}
	}
}

buffer file_writer::compress_chunk(buffer& uncompressed)
{
	const uint64_t header_size = sizeof(uint64_t) * 2;
	uint64_t compressed_size = density_compress_safe_size(uncompressed.size()) + header_size;
	buffer compressed(compressed_size);
	density_processing_result result = density_compress((const uint8_t *)uncompressed.data(), uncompressed.size(),
	                                                    (uint8_t *)compressed.data() + header_size, compressed.size(),
	                                                    DENSITY_ALGORITHM_CHEETAH);
	uncompressed.release();
	if (result.state != DENSITY_STATE_OK)
	{
		ABORT("Failed to compress buffer - aborting from compression thread");
	}
	uint64_t header[2] = { result.bytesWritten, result.bytesRead }; // store compressed and uncompressed sizes
	memcpy(compressed.data(), header, header_size); // use memcpy to avoid aliasing issues
	compressed.shrink(result.bytesWritten + header_size);
	DLOG3("Filewriter thread %d handing over compressed buffer of %lu bytes, was %lu bytes uncompressed", mTid, (unsigned long)(result.bytesWritten + header_size), (unsigned long)result.bytesRead);
	return compressed;
}

void file_writer::compressor()
{
	// lock, grab pointer to uncompressed, make new compressed, unlock, compress, sleep, repeat
	set_thread_name("compressor");
	while (1)
	{
		buffer uncompressed;
		chunk_mutex.lock();
		// try to steal uncompressed buffer
		if (uncompressed_chunks.size()) // we have buffers to compress
		{
			// take oldest chunk to compress
			uncompressed = uncompressed_chunks.back();
			uncompressed_chunks.pop_back();
		}
		// note that we only exit thread if we have no more work to do
		else if (done_feeding)
		{
			chunk_mutex.unlock();
			break; // exit thread
		}
		chunk_mutex.unlock();
		// compress it
		if (uncompressed.size() > 0)
		{
			buffer compressed = compress_chunk(uncompressed);

			if (multithreaded_write)
			{
				chunk_mutex.lock();
				compressed_chunks.push_front(compressed);
				chunk_mutex.unlock();
			}
			else write_chunk(compressed);
		}
		// if not done and no work done, wait a bit
		else if (!done_feeding)
		{
			usleep(2000);
		}
	}
}
