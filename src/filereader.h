#pragma once

#include <assert.h>
#include <atomic>
#include <thread>
#include <cstdint>
#include <thread>
#include <list>
#include <cstring>
#include <stdio.h>
#include <stdarg.h>

#include "lavamutex.h"
#include "packfile.h"
#include "containers.h"
#include "util.h"

#define MULTITHREADED_READ

class file_reader
{
	file_reader(const file_reader&) = delete;
	file_reader& operator=(const file_reader&) = delete;

	void new_chunk()
	{
		bool caught_decompressor = false; // if we caught up with the decompressor and had to wait

		// There should not be anything 'left over' in the chunk by now
		assert(chunk.size() - uidx == 0);
		// Grab a new chunk to process
		uidx = 0xffff; // make sure it is a non-zero value to indicate we have work left to do
		while (uidx != 0)
		{
			chunk_mutex.lock();

			if (uncompressed_chunks.size())
			{
				chunk.release();
				chunk = uncompressed_chunks.front();
				uncompressed_chunks.pop_front();
				uidx = 0;
				if (done_decompressing && uncompressed_chunks.size() == 0)
				{
					done_reading = true;
				}
			}
			else
			{
				assert(!done_decompressing); // if this triggers, it means we tried to read more data than there is
			}
			chunk_mutex.unlock();

			if (uidx != 0)
			{
				if (multithreaded_read)
				{
					if (!caught_decompressor)
					{
						caught_decompressor = true;
						times_caught_decompressor++; // only count the unique times this happened, not each iteration of the wait loop
					}
					usleep(10000); // wait for more data
				}
				else
				{
					if (!decompress_chunk()) break; // generate new chunk
				}
			}
		}
	}

	inline void check_space(unsigned size)
	{
		assert(chunk.size() >= uidx);
		if (unlikely(size > chunk.size() - uidx)) new_chunk();
		assert(chunk.size() >= uidx);
	}

protected:
	uint64_t uncompressed_bytes GUARDED_BY(chunk_mutex) = 0;
	uint32_t times_caught_decompressor = 0; // unique number of times we caught up with the decompressor

	bool decompress_chunk();

	template <typename T> inline void read_value(T* val)
	{
		check_space(sizeof(T));
		const char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
		memcpy(val, uptr, sizeof(T)); // memcpy to avoid aliasing issues
		uidx += sizeof(T);
		DLOG3("%u : read value of size %u (value %lu; %lu left in file; %lu left in chunk)", tid, (unsigned)sizeof(T), (unsigned long)*val, (unsigned long)total_left, (unsigned long)chunk.size() - uidx);
	}

public:
	/// Initialize one thread of replay.
	file_reader(const std::string& filename, unsigned mytid);
	file_reader(packed pf, unsigned mytid);
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
			const char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
			if (buf && size) memcpy(ptr, uptr, size);
			uidx += size;
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
		const char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
		if (arr) memcpy(arr, uptr, size); // values are already made portable by the time we get here
		DLOG3("%u : read array of size %u * %u, first value is %lu", tid, (unsigned)sizeof(T), (unsigned)count, (unsigned long)((count > 0) ? arr[0] : 0));
		uidx += size;
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

	void initiate_preload(uint64_t end);
	void reset_preload();

	memory_pool pool;

	/// Return true if we are done processing input data (no more data to be read from file, and more data
	/// awaiting to be read from uncompressed buffers).
	inline bool done()
	{
		if (unlikely(done_decompressing && done_reading && (chunk.size() - uidx) == 0))
		{
			return true;
		}
		return false;
	}

	void disable_multithreaded_read() // we can only disable on the fly, enable makes less sense
	{
		chunk_mutex.lock();
		done_decompressing = true;
		decompressor_thread.join();
		multithreaded_read = false;
		chunk_mutex.unlock();
	}

private:
	void decompressor(); // runs in separate thread, moves chunks from file to uncompressed chunks

	bool multithreaded_read = true;
	unsigned tid = -1; // only used for logging
	lava::mutex chunk_mutex;
	FILE* fp = nullptr;
	std::string mFilename;
	unsigned uidx = 0; // index into current uncompressed chunk
	uint64_t total_left = 0; // amount of compressed bytes left in input file
	buffer chunk; // current uncompressed chunk
	std::list<buffer> uncompressed_chunks GUARDED_BY(chunk_mutex);
	std::thread decompressor_thread;
	std::atomic_bool done_decompressing;
	std::atomic_bool done_reading;
	std::atomic_int readahead_chunks; // maximum number of chunks to keep in memory, if got more then decompressor thread will idle
};
