#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <lz4.h>

#include "util.h"
#include "filereader.h"
#include "density/src/density_api.h"

void file_reader::init(int fd, size_t uncompressed_size, size_t uncompressed_target)
{
	if (total_left == 0) ABORT("Input file \"%s\" is empty!", mFilename.c_str());

	const off_t offset = lseek(fd, 0, SEEK_CUR);
	const off_t pa_offset = offset & page_mask(); // must be page-aligned
	mapped_size = total_left + offset - pa_offset; // adjust for the page-alignment above

	fstart = (char*)mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, fd, pa_offset);
	compressed_data = fstart + offset - pa_offset;
	assert(uncompressed_size > 0);
	total_uncompressed = uncompressed_size;
	uncompressed_wanted = uncompressed_target;

	// Check if we start with a magic word, if so we're file version 2
	const char* magic_word = "LAVABIN";
	if (memcmp(compressed_data, magic_word, strlen(magic_word)) == 0)
	{
		compressed_data += strlen(magic_word);
		const uint8_t version = (uint8_t)compressed_data[0];
		assert(version == 1);
		compression_algorithm = compressed_data[1];
		assert(compression_algorithm == LAVATUBE_COMPRESSION_DENSITY || compression_algorithm == LAVATUBE_COMPRESSION_LZ4);
		compressed_data += 32; // the rest is reserved space
		(void)version;
		(void)compression_algorithm;
	}
	madvise(fstart, mapped_size, MADV_SEQUENTIAL);

	const uint64_t padded_size = density_decompress_safe_size(uncompressed_size);
	uncompressed_data = (char*)mmap(nullptr, padded_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	done_decompressing = false;
	decompressor_thread = std::thread(&file_reader::decompressor, this);
	start_measurement();
}

file_reader::file_reader(const std::string& filename, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target) : tid(mytid), mFilename(filename)
{
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) ABORT("Cannot open \"%s\": %s", filename.c_str(), strerror(errno));
	struct stat64 st;
	if (fstat64(fd, &st) == -1) ABORT("Failed to stat %s: %s", filename.c_str(), strerror(errno));
	total_left = st.st_size;
	init(fd, uncompressed_size, uncompressed_target);
	close(fd);
	(void)tid; // silence compiler
	DLOG("%s opened for reading (size %lu) and decompressor thread %u launched!", filename.c_str(), (unsigned long)total_left, tid);
}

file_reader::file_reader(packed pf, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target) : tid(mytid), mFilename(pf.inside)
{
	assert(pf.fd != -1);
	total_left = pf.filesize;
	init(pf.fd, uncompressed_size, uncompressed_target);
	DLOG("%u : %s opened for reading from inside %s (size %lu) and decompressor thread launched!", tid, pf.inside.c_str(), pf.pack.c_str(), (unsigned long)pf.filesize);
}

void file_reader::release_checkpoint()
{
	if (checkpoint_position == UINT64_MAX) return;
	assert(checkpoint_position >= freed_position);
	const uintptr_t pa_ptr = ((uintptr_t)uncompressed_data + freed_position) & page_mask(); // must be page-aligned
	const size_t pa_size = (checkpoint_position - freed_position) & page_mask(); // must be page-aligned
	assert(pa_size <= total_uncompressed);
	if (pa_size > page_size)
	{
		madvise((void*)pa_ptr, pa_size, MADV_FREE); // release no longer needed memory
		freed_position = pa_ptr + pa_size - (uintptr_t)uncompressed_data; // remember last freed position
		assert(freed_position <= checkpoint_position);
	}
	checkpoint_position = UINT64_MAX;
}

file_reader::~file_reader()
{
	done_decompressing = true;
	decompressor_thread.join();
	munmap(fstart, mapped_size);
	munmap(uncompressed_data, total_uncompressed);
}

/// Only call this from the decompressor thread (or main thread if not using multi-threaded file reading).
void file_reader::decompress_chunk()
{
	const uint64_t *header = (const uint64_t*)compressed_data;
	const uint64_t compressed_size = header[0];
	const uint64_t uncompressed_size = header[1];
	const uint64_t header_size = sizeof(uint64_t) * 2;
	compressed_data += header_size;
	assert(compressed_size <= total_left);
	uint8_t* destination = (uint8_t*)uncompressed_data + write_position.load(std::memory_order_relaxed);
	assert(uncompressed_data + total_uncompressed >= (char*)destination + uncompressed_size);
	if (compression_algorithm == LAVATUBE_COMPRESSION_DENSITY)
	{
		const uint64_t estimated_size = density_decompress_safe_size(uncompressed_size);
		assert(uncompressed_data + density_decompress_safe_size(total_uncompressed) >= (char*)destination + estimated_size);
		density_processing_result result = density_decompress((const uint8_t*)compressed_data, compressed_size, destination, estimated_size);
		if (result.state != DENSITY_STATE_OK) ABORT("Failed to decompress infile - aborting");
	}
	else // LZ4
	{
		int result = LZ4_decompress_safe(compressed_data, (char*)destination, compressed_size, uncompressed_size);
		if (result < 0) ABORT("Failed to decompress infile - aborting read thread");
		if ((uint64_t)result != uncompressed_size) ABORT("Failed to decompress the full chunk in infile - aborting read thread");
	}
	compressed_data += compressed_size;
	write_position += uncompressed_size;
	total_left -= compressed_size + header_size;
	uncompressed_bytes += uncompressed_size;
	if (total_left == 0 || write_position >= uncompressed_wanted) done_decompressing = true;  // all done!
}

/// Decompressor thread
void file_reader::decompressor()
{
	set_thread_name("decompressor");

	const size_t preload_size = p__preload * 1024 * 1024; // TBD move to framerange start
	while (!done_decompressing)
	{
		while (preload_size > 0 && write_position.load(std::memory_order_relaxed) > read_position + preload_size) usleep(10000); // we're too far ahead
		decompress_chunk();
	}

	clockid_t id;
	int r = pthread_getcpuclockid(pthread_self(), &id);
	if (r != 0)
	{
		ELOG("Failed to get worker thread %u ID: %s", tid, strerror(r));
	}
	else if (clock_gettime(id, &stop_worker_cpu_usage) != 0)
	{
		ELOG("Failed to get worker thread %u CPU usage: %s", tid, strerror(errno));
	}
}

void file_reader::start_measurement()
{
	clockid_t id;
	int r = pthread_getcpuclockid(pthread_self(), &id);
	if (r != 0)
	{
		ELOG("Failed to get API runner thread %u ID: %s", tid, strerror(r));
	}
	else if (clock_gettime(id, &runner_cpu_usage) != 0)
	{
		ELOG("Failed to get API runner thread %u CPU usage: %s", tid, strerror(errno));
	}

	if (!multithreaded_read)
	{
		// No longer valid
		clear_timespec(&worker_cpu_usage);
		clear_timespec(&stop_worker_cpu_usage);
		return;
	}
	pthread_t t = decompressor_thread.native_handle();
	r = pthread_getcpuclockid(t, &id);
	if (r != 0)
	{
		// This usually means our read-ahead worker is already done.
		clear_timespec(&worker_cpu_usage);
		clear_timespec(&stop_worker_cpu_usage);
	}
	else if (clock_gettime(id, &worker_cpu_usage) != 0)
	{
		ELOG("Failed to get worker thread %u CPU usage: %s", tid, strerror(errno));
	}
}

void file_reader::stop_measurement(uint64_t& worker, uint64_t& runner)
{
	struct timespec stop_runner_cpu_usage;
	clockid_t id;
	int r = pthread_getcpuclockid(pthread_self(), &id);
	if (r != 0)
	{
		ELOG("Failed to get runner thread %u ID: %s", tid, strerror(r));
		runner = 0;
	}
	else if (clock_gettime(id, &stop_runner_cpu_usage) != 0)
	{
		ELOG("Failed to get runner thread %u CPU usage: %s", tid, strerror(errno));
		runner = 0;
	}
	else
	{
		assert(stop_runner_cpu_usage.tv_sec >= runner_cpu_usage.tv_sec);
		runner = diff_timespec(&stop_runner_cpu_usage, &runner_cpu_usage);
	}

	if (!multithreaded_read)
	{
		worker = 0;
		return;
	}
	pthread_t t = decompressor_thread.native_handle();
	r = pthread_getcpuclockid(t, &id);
	if (r != 0)
	{
		// Read-ahead worker is already done. This is ok, if it stopped during frame range,
		// we got data in stop_worker_cpu_usage already
		worker = 0;
	}
	else if (clock_gettime(id, &stop_worker_cpu_usage) != 0)
	{
		ELOG("Failed to get worker thread %u CPU usage: %s", tid, strerror(errno));
		worker = 0;
	}
	else
	{
		assert(stop_worker_cpu_usage.tv_sec >= worker_cpu_usage.tv_sec);
		worker = diff_timespec(&stop_worker_cpu_usage, &worker_cpu_usage);
	}
}
