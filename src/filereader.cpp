#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <errno.h>

#include "util.h"
#include "filereader.h"
#include "density/src/density_api.h"

file_reader::file_reader(const std::string& filename, unsigned mytid) : tid(mytid), mFilename(filename)
{
	fp = fopen(filename.c_str(), "rb");
	if (!fp)
	{
		ABORT("Cannot open \"%s\": %s", filename.c_str(), strerror(errno));
		return;
	}
	struct stat st;
	fstat(fileno(fp), &st);
	total_left = st.st_size;
	if (total_left == 0) ABORT("Input file \"%s\" is empty!", filename.c_str());
	done_decompressing = false;
	done_reading = false;
	readahead_chunks.exchange(2);
	decompressor_thread = std::thread(&file_reader::decompressor, this);
	DLOG("%s opened for reading (size %lu) and decompressor thread launched!", filename.c_str(), (unsigned long)total_left);
}

file_reader::file_reader(packed pf, unsigned mytid) : tid(mytid), mFilename(pf.inside)
{
	if (pf.fd == -1)
	{
		ABORT("Failed to open \"%s\"", mFilename.c_str());
		return;
	}
	fp = fdopen(pf.fd, "rb");
	total_left = pf.filesize;
	if (total_left == 0) ABORT("Input file \"%s\" is empty!", pf.inside.c_str());
	done_decompressing = false;
	done_reading = false;
	readahead_chunks.exchange(2);
	decompressor_thread = std::thread(&file_reader::decompressor, this);
	DLOG("%u : %s opened for reading from inside %s (size %lu) and decompressor thread launched!", tid, pf.inside.c_str(), pf.pack.c_str(), (unsigned long)pf.filesize);
}

file_reader::~file_reader()
{
	chunk.release();
	done_decompressing = true;
	decompressor_thread.join();
	fclose(fp);
	if (times_caught_decompressor) // this would be bad
	{
		ELOG("We caught up with the decompressor thread %u times!", (unsigned)times_caught_decompressor);
	}
}

void file_reader::reset_preload()
{
	readahead_chunks.exchange(2);
}

/// Increase readahead_chunks and wait until uncompressed_bytes >= end
void file_reader::initiate_preload(uint64_t end)
{
	printf("Starting to preload of %lu bytes for thread %u...\n", (unsigned long)end, tid);
	int chunks = 0;
	uint64_t total_read;
	while (1)
	{
		chunk_mutex.lock();
		total_read = uncompressed_bytes;
		chunks = uncompressed_chunks.size();
		chunk_mutex.unlock();

		// these variables are all local or atomic
		if (total_read >= end || done_decompressing) break;
		if (readahead_chunks <= chunks) readahead_chunks++;
		usleep(10000);
	}
	printf("Thread %u running preloaded! Uncompressed %lu bytes in total with %d unused chunks\n", tid, (unsigned long)total_read, chunks);
}

/// Only call this from the decompressor thread (or main thread if not using multi-threaded file reading).
bool file_reader::decompress_chunk()
{
	const uint64_t header_size = sizeof(uint64_t) * 2;
	uint64_t header[2];
	size_t actually_read = 0;
	int err = 0;
	do
	{
		actually_read = fread(header, header_size, 1, fp);
		err = ferror(fp);
	} while (!feof(fp) && !actually_read && (err == EWOULDBLOCK || err == EINTR || err == EAGAIN));
	const uint64_t compressed_size = header[0];
	uint64_t uncompressed_size = header[1];
	buffer uncompressed(density_decompress_safe_size(uncompressed_size));

	assert(actually_read);
	assert(fp);
	assert(total_left >= compressed_size);

	uint64_t readsize = compressed_size;
	buffer compressed(compressed_size);
	actually_read = 0;
	do {
		size_t result = fread(compressed.data() + actually_read, 1, readsize, fp);
		err = ferror(fp);
		readsize -= result;
		actually_read += result;
	} while (!feof(fp) && readsize > 0 && (err == EWOULDBLOCK || err == EINTR || err == EAGAIN));
	if (err)
	{
		ABORT("Failed to read input data: %s", strerror(err));
		done_decompressing = true;
		return false;
	}
	else if (feof(fp))
	{
		ABORT("Tried too read more input data than there was");
		done_decompressing = true;
		return false;
	}
	assert(readsize == 0);
	density_processing_result result = density_decompress((const uint8_t*)compressed.data(), compressed.size(), (uint8_t*)uncompressed.data(), uncompressed.size());
	compressed.release();
	if (result.state != DENSITY_STATE_OK)
	{
		ABORT("Failed to decompress infile - aborting read thread");
		done_decompressing = true;
		return false;
	}
	uncompressed.shrink(result.bytesWritten);
	total_left -= compressed_size + header_size;
#ifdef MULTITHREADED_READ
	chunk_mutex.lock();
#endif
	uncompressed_bytes += uncompressed.size();
	uncompressed_chunks.push_back(uncompressed);
	if (total_left == 0) // all done!
	{
		done_decompressing = true;
	}
#ifdef MULTITHREADED_READ
	chunk_mutex.unlock();
#endif
	return true;
}

/// Decompressor thread. Use the readahead_chunks variable to control how many chunks to preload.
void file_reader::decompressor()
{
	set_thread_name("decompressor");
#ifdef MULTITHREADED_READ
	while (!done_decompressing)
	{
		chunk_mutex.lock();
		const int idle_chunks = uncompressed_chunks.size();
		chunk_mutex.unlock();
		if (idle_chunks < readahead_chunks)
		{
			if (!decompress_chunk()) break;
		}
		else
		{
			usleep(10000);
		}
	}
#endif
}
