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
uint64_t file_writer::write_patch(char* orig, const char* const chng, uint64_t offset, uint64_t size)
{
	uint64_t total_left = size;
	uint32_t c = offset;
	uint32_t offset_to_write = 0;
	uint64_t changed = 0;
	uint64_t* origalias = (uint64_t*)(orig + offset);
	uint64_t* chngalias = (uint64_t*)(chng + offset);
	while (total_left)
	{
		for (; total_left >= 8 && *origalias == *chngalias; origalias++, chngalias++, c += 8, total_left -= 8) {} // Skip identical sequence
		offset_to_write = c;
		char* startchange = (char*)chngalias;
		char* startoriginal = (char*)origalias;
		for (c = 0; total_left >= 8 && *origalias != *chngalias; origalias++, chngalias++, c += 8, total_left -= 8) {} // Skip difference sequence

		if (c == 0 && total_left < 8 && memcmp(startchange, startoriginal, total_left) == 0) total_left = 0; // check if we have no changes, then we can skip some work altogether
		else if (total_left < 8) { c += total_left; total_left = 0; } // write out remainder

		if (c)
		{
			check_space(8 + c);
			char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
			memcpy(uptr, &offset_to_write, 4); // write offset
			uptr += 4;
			memcpy(uptr, &c, 4); // write size of patch
			uptr += 4;
			memcpy(uptr, startchange, c); // write payload
			memcpy(startoriginal, startchange, c); // update the clone
			uidx += 8 + c;
			uncompressed_bytes += 8 + c;
			changed += c;
			c = 0;
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
	chunk.shrink(uidx);
	uncompressed_chunks.push_front(chunk);
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
			size_t written = 0;
			size_t size = active.size();
			char* ptr = active.data();
			int err = 0;
			DLOG3("\thunk=%u", (unsigned)size);
			do {
				written = fwrite(ptr, 1, size, fp);
				DLOG3("\t\twritten=%u / %u", (unsigned)written, (unsigned)size);
				ptr += written;
				size -= written;
				err = ferror(fp);
			} while (size > 0 && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR));
			if (size > 0)
			{
				ELOG("Failed to write out file (%u bytes left): %s", (unsigned)size, strerror(ferror(fp)));
			}
			active.release();
		}
		// if not done and no work done, wait a bit
		else if (!done_compressing)
		{
			usleep(10000);
		}
	}
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
			const uint64_t header_size = sizeof(uint64_t) * 2;
			uint64_t compressed_size = density_compress_safe_size(uncompressed.size()) + header_size;
			buffer compressed(compressed_size);
			density_processing_result result = density_compress((const uint8_t *)uncompressed.data(), uncompressed.size(),
			                                                    (uint8_t *)compressed.data() + header_size, compressed.size(),
			                                                    DENSITY_ALGORITHM_CHEETAH);
			uncompressed.release();
			if (result.state != DENSITY_STATE_OK)
			{
				ELOG("Failed to compress buffer - aborting compression thread");
				break;
			}
			uint64_t header[2] = { result.bytesWritten, result.bytesRead }; // store compressed and uncompressed sizes
			memcpy(compressed.data(), header, header_size); // use memcpy to avoid aliasing issues
			compressed.shrink(result.bytesWritten + header_size);
			chunk_mutex.lock();
			compressed_chunks.push_front(compressed);
			chunk_mutex.unlock();
		}
		// if not done and no work done, wait a bit
		else if (!done_feeding)
		{
			usleep(100000);
		}
	}
}
