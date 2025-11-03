#pragma once

#include <assert.h>
#include <atomic>
#include <thread>
#include <cstdint>
#include <thread>
#include <cstring>
#include <stdio.h>
#include <stdarg.h>

#include "lavamutex.h"
#include "packfile.h"
#include "containers.h"
#include "util.h"

class file_reader
{
	file_reader(const file_reader&) = delete;
	file_reader& operator=(const file_reader&) = delete;

protected:
	inline void check_space(unsigned size)
	{
		while (unlikely(size > write_position.load(std::memory_order_relaxed) - read_position))
		{
			assert(write_position.load(std::memory_order_relaxed) + size < total_uncompressed);
			if (!p__allow_stalls) ABORT("We caught up with our file read thread! Performance data may become unreliable, so aborting!");
			if (multithreaded_read) usleep(10000); // wait for more data
			else decompress_chunk(); // generate new data
		}
	}

	/// Do not release any memory past this point until it has been released again.
	void set_checkpoint() { checkpoint_position = read_position; }

	/// Let us know that we can now release any memory before the current read position. Safe to call even if no checkpoint set.
	void release_checkpoint();

	std::atomic_uint64_t uncompressed_bytes { 0 };

	void decompress_chunk();

	template <typename T> inline void read_value(T* val)
	{
		check_space(sizeof(T));
		const char* uptr = uncompressed_data + read_position; // pointer into current uncompressed chunk
		memcpy(val, uptr, sizeof(T)); // memcpy to avoid aliasing issues
		read_position += sizeof(T);
		DLOG3("%u : read value of size %u (value %lu; %lu left in file)", tid, (unsigned)sizeof(T), (unsigned long)*val, (unsigned long)total_left); // unsafe read of total_left
	}

public:
	/// Initialize one thread of replay.
	file_reader(const std::string& filename, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target);
	file_reader(packed pf, unsigned mytid, size_t uncompressed_size, size_t uncompressed_target);
	~file_reader();

	inline uint8_t read_uint8_t() { uint8_t t; read_value(&t); return t; }
	inline uint16_t read_uint16_t() { uint16_t t; read_value(&t); return t; }
	inline uint32_t read_uint32_t() { uint32_t t; read_value(&t); return t; }
	inline uint64_t read_uint64_t() { uint64_t t; read_value(&t); return t; }
	inline int8_t read_int8_t() { int8_t t; read_value(&t); return t; }
	inline int16_t read_int16_t() { int16_t t; read_value(&t); return t; }
	inline int32_t read_int32_t() { int32_t t; read_value(&t); return t; }
	inline int64_t read_int64_t() { int64_t t; read_value(&t); return t; }
	inline float read_float() { uint32_t t; float r; read_value(&t); memcpy(&r, &t, sizeof(float)); return r; }
	inline double read_double() { uint64_t t; double r; read_value(&t); memcpy(&r, &t, sizeof(double)); return r; }
	inline size_t read_size_t() { uint64_t t; read_value(&t); return static_cast<size_t>(t); }
	inline int read_int() { uint32_t t; read_value(&t); return static_cast<int>(t); }
	inline long read_long() { uint64_t t; read_value(&t); return static_cast<long>(t); }

	/// Patch a memory area, return number of bytes changed.
	uint32_t read_patch(char* buf, uint64_t maxsize)
	{
		char* ptr = buf;
		uint32_t offset;
		uint32_t size;
		uint64_t changed = 0;
		do {
			offset = read_uint32_t();
			ptr += offset;
			// cppcheck-suppress nullPointerRedundantCheck
			assert(maxsize == 0 || ptr <= buf + maxsize);
			size = read_uint32_t();
			check_space(size);
			const char* uptr = uncompressed_data + read_position;
			if (buf && size) memcpy(ptr, uptr, size);
			read_position += size;
			ptr += size;
			changed += size;
			// cppcheck-suppress nullPointerRedundantCheck
			assert(maxsize == 0 || ptr <= buf + maxsize);
		}
		while (!(offset == 0 && size == 0));
		return changed;
	}

	template <typename T> inline void read_array(T* arr, size_t count)
	{
		assert((arr && count) || (!arr && !count));
		const unsigned size = sizeof(T) * count;
		check_space(size);
		const char* uptr = uncompressed_data + read_position;
		if (arr) memcpy(arr, uptr, size); // values are already made portable by the time we get here
		DLOG3("%u : read array of size %u * %u, first value is %lu", tid, (unsigned)sizeof(T), (unsigned)count, (unsigned long)((count > 0) ? arr[0] : 0));
		read_position += size;
	}

	const char* make_string(const char* format, ...) __attribute__((format (printf, 2, 3)))
	{
		char* buf = pool.pointer<char>();
		va_list ap;
		va_start(ap, format);
		int size = vsprintf(buf, format, ap);
		va_end(ap);
		pool.spend(size + 1);
		return buf;
	}

	void append_string(const char* format, ...) __attribute__((format (printf, 2, 3)))
	{
		char* buf = pool.pointer<char>() - 1; // remove terminating null from previous string entry
		assert(*buf == 0); // verify that we're actually appending to a previous string
		va_list ap;
		va_start(ap, format);
		int size = vsprintf(buf, format, ap);
		va_end(ap);
		pool.spend(size);
	}

	const char* read_string() // copies string over to memory pool and returns pointer to it
	{
		uint16_t len = read_uint16_t();
		char* dst = nullptr;
		if (len > 0)
		{
			dst = pool.allocate<char>(len + 1);
			read_array(dst, len);
			dst[len] = '\0'; // add terminating null
		}
		DLOG3("%u : read string \"%s\" (len=%u)", tid, dst, (unsigned)len);
		return dst;
	}

	const char* const* read_string_array(unsigned len) // copies strings over to memory pool, and returns pointer to them
	{
		DLOG3("%u : read string array (len=%u)", tid, (unsigned)len);
		const char** dst = nullptr;
		if (len > 0) dst = (const char**)pool.allocate<void*>(len);
		for (uint32_t i = 0; i < len; i++)
		{
			dst[i] = read_string();
		}
		return dst;
	}

	memory_pool pool;

	/// Return true if we are done processing input data (no more data to be read from file, and more data
	/// awaiting to be read from uncompressed buffer).
	inline bool done() const { return unlikely(done_decompressing && write_position.load(std::memory_order_relaxed) - read_position == 0); }

	void disable_multithreaded_read() // we can only disable on the fly, enable makes less sense
	{
		chunk_mutex.lock();
		done_decompressing = true;
		decompressor_thread.join();
		multithreaded_read = false;
		chunk_mutex.unlock();
	}

	void self_test() const
	{
	}

	/// Start measuring worker thread CPU usage
	void start_measurement();

	/// Return spent CPU time in microseconds in worker thread
	void stop_measurement(uint64_t& worker, uint64_t& runner);

private:
	void decompressor(); // runs in separate thread, moves chunks from file to uncompressed chunks
	void init(int fd, size_t uncompressed_size, size_t uncompressed_target);

	bool multithreaded_read = true;
	unsigned tid = -1; // only used for logging
	lava::mutex chunk_mutex;
	/// Pointer to mapped memory of compressed file
	char* compressed_data = nullptr; // current position in compressed buffer
	char* fstart = nullptr; // start position
	/// Name of compressed input file
	std::string mFilename;
	/// Start CPU usage for our worker thread
	struct timespec worker_cpu_usage;
	/// Start CPU usage for our runner thread
	struct timespec runner_cpu_usage;
	/// Stop CPU usage for our worker thread
	struct timespec stop_worker_cpu_usage;
	/// Amount of memory mapped compressed data
	uint64_t mapped_size = 0;
	/// Checkpoint position - from where we have last started reading, but need to preserve data from. Only updated from main thread.
	uint64_t checkpoint_position = 0;
	/// Tip of the uncompressed buffer, the end of where we have last put data
	std::atomic_uint64_t write_position { 0 };
	/// Last freed position, page-aligned position before the last checkpoint. Only updated by main thread.
	uint64_t freed_position { 0 };
	int compression_algorithm = LAVATUBE_COMPRESSION_DENSITY;

protected:
	const uintptr_t page_size = sysconf(_SC_PAGE_SIZE); // for doing page-alignment
	uintptr_t page_mask() const { return ~(page_size - 1); } // for doing page-alignment
	uint64_t total_left = 0; // amount of compressed bytes left in input file, only use from decompressor thread
	uint64_t total_uncompressed = 0; // amount of uncompressed bytes that will come from the input file
	uint64_t uncompressed_wanted = 0; // amount of uncompressed bytes that we want to read
	/// Start of anonymous memory map for uncompressed data
	char* uncompressed_data = nullptr;
	/// Current position in the uncompressed buffer. Only modified by the main thread.
	uint64_t read_position = 0;

private:
	std::thread decompressor_thread;
	std::atomic_bool done_decompressing;
};
