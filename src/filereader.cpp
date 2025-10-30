#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "util.h"
#include "filereader.h"
#include "density/src/density_api.h"

void file_reader::init(int fd)
{
	if (total_left == 0) ABORT("Input file \"%s\" is empty!", mFilename.c_str());

	const off_t offset = lseek(fd, 0, SEEK_CUR);
	const off_t pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1); // must be page-aligned
	mapped_size = total_left + offset - pa_offset; // adjust for the page-alignment above

	fstart = (char*)mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, fd, pa_offset);
	madvise(fstart, mapped_size, MADV_SEQUENTIAL);
	fptr = fstart + offset - pa_offset;

	done_decompressing = false;
	done_reading = false;
	readahead_chunks.exchange(2);
	decompressor_thread = std::thread(&file_reader::decompressor, this);
	start_measurement();
}

file_reader::file_reader(const std::string& filename, unsigned mytid) : tid(mytid), mFilename(filename)
{
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) ABORT("Cannot open \"%s\": %s", filename.c_str(), strerror(errno));
	struct stat64 st;
	if (fstat64(fd, &st) == -1) ABORT("Failed to stat %s: %s", filename.c_str(), strerror(errno));
	total_left = st.st_size;
	init(fd);
	close(fd);
	DLOG("%s opened for reading (size %lu) and decompressor thread %u launched!", filename.c_str(), (unsigned long)total_left, tid);
}

file_reader::file_reader(packed pf, unsigned mytid) : tid(mytid), mFilename(pf.inside)
{
	assert(pf.fd != -1);
	total_left = pf.filesize;
	init(pf.fd);
	DLOG("%u : %s opened for reading from inside %s (size %lu) and decompressor thread launched!", tid, pf.inside.c_str(), pf.pack.c_str(), (unsigned long)pf.filesize);
}

file_reader::~file_reader()
{
	chunk.release();
	done_decompressing = true;
	decompressor_thread.join();
	munmap(fstart, mapped_size);
	if (times_caught_decompressor) // this would be bad
	{
		ELOG("We caught up with the decompressor thread %u times on thread %u!", (unsigned)times_caught_decompressor, tid);
	}
}

/// Only call this from the decompressor thread (or main thread if not using multi-threaded file reading).
bool file_reader::decompress_chunk()
{
	const uint64_t *header = (const uint64_t*)fptr;
	const uint64_t compressed_size = header[0];
	const uint64_t uncompressed_size = header[1];
	const uint64_t header_size = sizeof(uint64_t) * 2;
	fptr += header_size;

	buffer uncompressed(density_decompress_safe_size(uncompressed_size));
	density_processing_result result = density_decompress((const uint8_t*)fptr, compressed_size, (uint8_t*)uncompressed.data(), uncompressed.size());
	if (result.state != DENSITY_STATE_OK) ABORT("Failed to decompress infile - aborting");
	fptr += compressed_size;

	uncompressed.shrink(result.bytesWritten);
	total_left -= compressed_size + header_size;

	chunk_mutex.lock();
	uncompressed_bytes += uncompressed.size();
	uncompressed_chunks.push_back(uncompressed);
	if (total_left == 0) // all done!
	{
		done_decompressing = true;
	}
	chunk_mutex.unlock();

	return true;
}

/// Decompressor thread. Use the readahead_chunks variable to control how many chunks to preload.
void file_reader::decompressor()
{
	set_thread_name("decompressor");
	while (!done_decompressing)
	{
		chunk_mutex.lock();
		const int idle_chunks = uncompressed_chunks.size();
		chunk_mutex.unlock();
		if (idle_chunks < readahead_chunks)
		{
			if (!decompress_chunk()) break;
		}
		else // we have enough chunks waiting to be processed
		{
			usleep(10000);
		}
	}
	chunk_mutex.lock();
	clockid_t id;
	int r = pthread_getcpuclockid(pthread_self(), &id);
	if (r != 0)
	{
		ELOG("Failed to get worker thread ID: %s", strerror(r));
	}
	else if (clock_gettime(id, &stop_cpu_usage) != 0)
	{
		ELOG("Failed to get worker thread CPU usage!");
	}
	chunk_mutex.unlock();
}
