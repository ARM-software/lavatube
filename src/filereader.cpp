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

	// Check if we start with a magic word, if so the stream contains versioned metadata.
	const char* magic_word = "LAVABIN";
	if (memcmp(compressed_data, magic_word, strlen(magic_word)) == 0)
	{
		compressed_data += strlen(magic_word);
		const uint8_t version = (uint8_t)compressed_data[0];
		assert(version == 1 || version == 2);
		stream_version = version;
		compression_algorithm = compressed_data[1];
		assert(compression_algorithm == LAVATUBE_COMPRESSION_DENSITY || compression_algorithm == LAVATUBE_COMPRESSION_LZ4 || compression_algorithm == LAVATUBE_COMPRESSION_UNCOMPRESSED);
		const size_t header_bytes = strlen(magic_word) + 32;
		assert(total_left >= header_bytes);
		compressed_data += 32; // the rest is reserved space
		total_left -= header_bytes;
	}
	compressed_stream_start = compressed_data;
	total_compressed_stream = total_left;
	madvise(fstart, mapped_size, MADV_SEQUENTIAL);

	const uint64_t padded_size = (compression_algorithm == LAVATUBE_COMPRESSION_DENSITY) ? density_decompress_safe_size(uncompressed_size) : uncompressed_size;
	uncompressed_data = (char*)mmap(nullptr, padded_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	done_decompressing = false;
	decompressor_thread = std::thread(&file_reader::decompressor, this);
}

void file_reader::init_mapped(const packed& pf, size_t uncompressed_size, size_t uncompressed_target)
{
	if (total_left == 0) ABORT("Input file \"%s\" is empty!", mFilename.c_str());

	assert(pf.zip_handle);
	assert(pf.zip_mapping.data);
	fstart = (char*)pf.zip_mapping.map_base;
	compressed_data = (char*)pf.zip_mapping.data;
	mapped_size = pf.zip_mapping.map_length;
	assert(uncompressed_size > 0);
	total_uncompressed = uncompressed_size;
	uncompressed_wanted = uncompressed_target;

	const char* magic_word = "LAVABIN";
	if (memcmp(compressed_data, magic_word, strlen(magic_word)) == 0)
	{
		compressed_data += strlen(magic_word);
		const uint8_t version = (uint8_t)compressed_data[0];
		assert(version == 1 || version == 2);
		stream_version = version;
		compression_algorithm = compressed_data[1];
		assert(compression_algorithm == LAVATUBE_COMPRESSION_DENSITY || compression_algorithm == LAVATUBE_COMPRESSION_LZ4 || compression_algorithm == LAVATUBE_COMPRESSION_UNCOMPRESSED);
		const size_t header_bytes = strlen(magic_word) + 32;
		assert(total_left >= header_bytes);
		compressed_data += 32;
		total_left -= header_bytes;
	}
	compressed_stream_start = compressed_data;
	total_compressed_stream = total_left;
	madvise(fstart, mapped_size, MADV_SEQUENTIAL);

	const uint64_t padded_size = (compression_algorithm == LAVATUBE_COMPRESSION_DENSITY) ? density_decompress_safe_size(uncompressed_size) : uncompressed_size;
	uncompressed_data = (char*)mmap(nullptr, padded_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	done_decompressing = false;
	decompressor_thread = std::thread(&file_reader::decompressor, this);
}

file_reader::file_reader(const std::string& filename, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target, bool preload_active)
	: preload_activated(preload_active), tid(mytid), mFilename(filename)
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

file_reader::file_reader(packed pf, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target, bool preload_active)
	: preload_activated(preload_active), tid(mytid), mFilename(pf.inside)
{
	total_left = pf.filesize;
	if (pf.zip_handle)
	{
		zip_handle = pf.zip_handle;
		zip_mapping = pf.zip_mapping;
		init_mapped(pf, uncompressed_size, uncompressed_target);
	}
	else
	{
		assert(pf.fd != -1);
		init(pf.fd, uncompressed_size, uncompressed_target);
		close(pf.fd);
	}
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
		madvise((void*)pa_ptr, pa_size, MADV_DONTNEED); // aggressively discard pages we will never read again
		freed_position = pa_ptr + pa_size - (uintptr_t)uncompressed_data; // remember last freed position
		uncompressed_released_bytes.store(freed_position, std::memory_order_relaxed);
		assert(freed_position <= checkpoint_position);
	}
	checkpoint_position = UINT64_MAX;
}

void file_reader::release_compressed_pages()
{
	if (!fstart || !compressed_data) return;

	const uintptr_t released_start = (uintptr_t)fstart + compressed_mapping_released_bytes.load(std::memory_order_relaxed);
	const uintptr_t released_end = (uintptr_t)compressed_data & page_mask();
	if (released_end <= released_start) return;

	const size_t releasable = released_end - released_start;
	if (releasable <= page_size) return;

	madvise((void*)released_start, releasable, MADV_DONTNEED); // compressed pages are strictly forward-only
	compressed_mapping_released_bytes.store(released_end - (uintptr_t)fstart, std::memory_order_relaxed);
}

file_reader::~file_reader()
{
	done_decompressing = true;
	if (decompressor_thread.joinable()) decompressor_thread.join();
	if (zip_handle)
	{
		zipc_unmap_read(zip_handle, zip_mapping);
		zipc_close(zip_handle);
	}
	else munmap(fstart, mapped_size);
	const uint64_t padded_size = (compression_algorithm == LAVATUBE_COMPRESSION_DENSITY) ? density_decompress_safe_size(total_uncompressed) : total_uncompressed;
	munmap(uncompressed_data, padded_size);
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
	else if (compression_algorithm == LAVATUBE_COMPRESSION_LZ4)
	{
		int result = LZ4_decompress_safe(compressed_data, (char*)destination, compressed_size, uncompressed_size);
		if (result < 0) ABORT("Failed to decompress infile - aborting read thread");
		if ((uint64_t)result != uncompressed_size) ABORT("Failed to decompress the full chunk in infile - aborting read thread");
	}
	else // uncompressed
	{
		assert(compression_algorithm == LAVATUBE_COMPRESSION_UNCOMPRESSED);
		memcpy(destination, compressed_data, uncompressed_size);
	}
	compressed_data += compressed_size;
	compressed_stream_consumed_bytes.store((uint64_t)(compressed_data - compressed_stream_start), std::memory_order_relaxed);
	write_position.fetch_add(uncompressed_size, std::memory_order_release);
	write_position.notify_one();
	last_chunk_uncompressed_size = uncompressed_size;
	total_left -= compressed_size + header_size;
	uncompressed_bytes += uncompressed_size;
	release_compressed_pages();
	if (total_left == 0 || write_position >= uncompressed_wanted) done_decompressing = true;  // all done!
}

/// Decompressor thread
void file_reader::decompressor()
{
	set_thread_name("decompressor");

	const size_t fixed_preload = p__preload * 1024 * 1024;
	while (!done_decompressing)
	{
		// Throttle unless the replayer is blocked waiting for data (needed_write_position > write_position),
		// which can happen when a packet spans more than one chunk in one-chunk-ahead mode.
		while (true)
		{
			const size_t active_preload = preload_activated.load(std::memory_order_relaxed) ? fixed_preload : 0;
			const size_t preload_size = active_preload > 0 ? active_preload : last_chunk_uncompressed_size;
			if (!(preload_size > 0
			      && write_position.load(std::memory_order_relaxed) > read_position + preload_size
			      && write_position.load(std::memory_order_relaxed) >= needed_write_position.load(std::memory_order_relaxed)))
			{
				break;
			}
			usleep(1000); // we're too far ahead and replayer is not blocked
		}
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
	else
	{
		worker_measurement_ready.store(true, std::memory_order_release);
	}
}

void file_reader::start_measurement()
{
	preload_activated.store(true, std::memory_order_release);
	measurement_stopped.store(false, std::memory_order_release);
	cached_worker_time = 0;
	cached_runner_time = 0;
	worker_measurement_ready.store(false, std::memory_order_release);

	if (multithreaded_read && p__preload > 0)
	{
		const size_t fixed_preload = p__preload * 1024 * 1024;
		uint64_t target = read_position + fixed_preload;
		if (target > uncompressed_wanted) target = uncompressed_wanted;
		if (target > total_uncompressed) target = total_uncompressed;

		uint64_t current_write = write_position.load(std::memory_order_acquire);
		while (current_write < target)
		{
			write_position.wait(current_write, std::memory_order_acquire);
			current_write = write_position.load(std::memory_order_acquire);
			assert(current_write >= target || !done_decompressing.load(std::memory_order_relaxed));
		}
	}

	clockid_t id;
	pthread_t runner = pthread_self();
	if (runner_thread_bound.load(std::memory_order_acquire)) runner = runner_thread;
	int r = pthread_getcpuclockid(runner, &id);
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

void file_reader::bind_runner_thread()
{
	runner_thread = pthread_self();
	runner_thread_bound.store(true, std::memory_order_release);
}

void file_reader::stop_measurement(uint64_t& worker, uint64_t& runner)
{
	if (measurement_stopped.load(std::memory_order_acquire))
	{
		worker = cached_worker_time;
		runner = cached_runner_time;
		return;
	}
	struct timespec stop_runner_cpu_usage;
	clockid_t id;
	pthread_t runner_handle = pthread_self();
	if (runner_thread_bound.load(std::memory_order_acquire)) runner_handle = runner_thread;
	int r = pthread_getcpuclockid(runner_handle, &id);
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
		if (timespec_less(&stop_runner_cpu_usage, &runner_cpu_usage))
		{
			ELOG("Failed to measure runner thread %u CPU usage: stop time precedes start time", tid);
			runner = 0;
		}
		else
		{
			runner = diff_timespec(&stop_runner_cpu_usage, &runner_cpu_usage);
		}
	}

	if (!multithreaded_read)
	{
		worker = 0;
		cached_worker_time = worker;
		cached_runner_time = runner;
		measurement_stopped.store(true, std::memory_order_release);
		return;
	}
	pthread_t t = decompressor_thread.native_handle();
	r = pthread_getcpuclockid(t, &id);
	if (r != 0)
	{
		// Read-ahead worker is already done. This is ok if it stopped during frame range,
		// in which case the worker thread saved its final CPU timestamp for us.
		if (worker_measurement_ready.load(std::memory_order_acquire))
		{
			if (timespec_less(&stop_worker_cpu_usage, &worker_cpu_usage))
			{
				ELOG("Failed to measure worker thread %u CPU usage: stop time precedes start time", tid);
				worker = 0;
			}
			else
			{
				worker = diff_timespec(&stop_worker_cpu_usage, &worker_cpu_usage);
			}
		}
		else
		{
			worker = 0;
		}
	}
	else if (clock_gettime(id, &stop_worker_cpu_usage) != 0)
	{
		ELOG("Failed to get worker thread %u CPU usage: %s", tid, strerror(errno));
		worker = 0;
	}
	else
	{
		worker_measurement_ready.store(true, std::memory_order_release);
		if (timespec_less(&stop_worker_cpu_usage, &worker_cpu_usage))
		{
			ELOG("Failed to measure worker thread %u CPU usage: stop time precedes start time", tid);
			worker = 0;
		}
		else
		{
			worker = diff_timespec(&stop_worker_cpu_usage, &worker_cpu_usage);
		}
	}
	cached_worker_time = worker;
	cached_runner_time = runner;
	measurement_stopped.store(true, std::memory_order_release);
}
