#pragma once

#include <assert.h>
#include <atomic>
#include <thread>
#include <cstdint>
#include <list>
#include <cstring>
#include <stdio.h>

#include "lavamutex.h"
#include "util.h"

class file_writer
{
	file_writer(const file_writer&) = delete;
	file_writer& operator=(const file_writer&)= delete;

	void make_space(unsigned size)
	{
		// shrink existing chunk to actually used size
		chunk.shrink(uidx);

		// move chunk into list of chunks to compress
		if (holding)
		{
			held_chunks.push_front(chunk);
		}
		else if (multithreaded_compress)
		{
			chunk_mutex.lock();
			uncompressed_chunks.push_front(chunk);
			chunk_mutex.unlock();
		}
		else
		{
			buffer compressed = compress_chunk(chunk);
			if (multithreaded_write)
			{
				chunk_mutex.lock();
				compressed_chunks.push_front(compressed);
				chunk_mutex.unlock();
			}
			else write_chunk(compressed);
		}

		// create a new chunk for writing into (we could employ a free list here as a possible optimization)
		if (size > uncompressed_chunk_size) // make sure our new chunk is big enough
		{
			chunk = buffer(size);
		}
		else
		{
			chunk = buffer(uncompressed_chunk_size);
		}
		uidx = 0;
	}

	inline void check_space(unsigned size)
	{
		if (unlikely(size > chunk.size() - uidx)) make_space(size); // need new chunk?
	}

	template <typename T> inline void write_value(T value) // for single values
	{
		DLOG3("%d : write value of size %u (value %lu)", mTid, (unsigned)sizeof(T), (unsigned long)value);
		check_space(sizeof(T));
		char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
		memcpy(uptr, &value, sizeof(T)); // avoids aliasing issues
		uidx += sizeof(T);
		assert(uidx <= chunk.size());
		uncompressed_bytes += sizeof(T);
	}

public:
	file_writer(const std::string& name, int mytid = 0) : file_writer(mytid) { set(name); }
	file_writer(int mytid = 0);
	~file_writer();
	void finalize(); // basically our destructor, but we need to call it out of the normal destructor call order

	template <typename T> inline void write_array(const T* values, unsigned len) // for saving c arrays
	{
		assert(values != nullptr || len == 0);
		DLOG3("%d : write array of length objsize=%u * len=%u", mTid, (unsigned)sizeof(T), len);
		if (len > 0)
		{
			const unsigned size = sizeof(T) * len;
			check_space(size);
			char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
			memcpy(uptr, values, size); // values are already made portable by the time we get here
			uidx += size;
			assert(uidx <= chunk.size());
			uncompressed_bytes += size;
		}
	}

	void write_string(const char* text)
	{
		if (!text)
		{
			write_uint16_t(0);
			DLOG3("wrote null string");
		}
		else
		{
			uint16_t len = strlen(text); // not including terminating null
			write_uint16_t(len);
			write_array(text, len);
			DLOG3("%d : wrote string \"%s\" (len=%u)", mTid, text, (unsigned)len);
		}
	}

	void write_string(const std::string& text)
	{
		write_value(static_cast<uint16_t>(text.size()));
		if (text.size() > 0)
		{
			write_array(text.data(), text.size());
		}
		DLOG3("%d : wrote string \"%s\" (len=%u)", mTid, text.c_str(), (unsigned)text.size());
	}

	void write_string_array(const char* const* strs, unsigned len)
	{
		DLOG3("%d : write array of strings (len=%u)", mTid, (unsigned)len);
		for (uint32_t i = 0; i < len; i++)
		{
			const char* str = strs[i];
			write_string(str);
		}
	}

	inline void write_uint8_t(uint8_t value) { write_value(value); }
	inline void write_uint16_t(uint16_t value) { write_value(value); }
	inline void write_uint32_t(uint32_t value) { write_value(value); }
	inline void write_uint64_t(uint64_t value) { write_value(value); }
	inline void write_int8_t(int8_t value) { write_value(value); }
	inline void write_int16_t(int16_t value) { write_value(value); }
	inline void write_int32_t(int32_t value) { write_value(value); }
	inline void write_int64_t(int64_t value) { write_value(value); }
	inline void write_float(float value) { uint32_t t; memcpy(&t, &value, sizeof(uint32_t)); write_uint32_t(t); }
	inline void write_double(double value) { uint64_t t; memcpy(&t, &value, sizeof(uint64_t)); write_uint64_t(t); }

	template <typename T> inline T* write_value_later(T value) // delayed write, for single value only
	{
		check_space(sizeof(T));
		freeze(); // just in case it is not already
		char* uptr = chunk.data() + uidx; // pointer into current uncompressed chunk
		DLOG3("%d : write value of size %u (value %lu) and returning pointer %p to it", mTid, (unsigned)sizeof(T), (unsigned long)value, uptr);
		memcpy(uptr, &value, sizeof(T)); // avoids aliasing issues
		uidx += sizeof(T);
		assert(uidx <= chunk.size());
		uncompressed_bytes += sizeof(T);
		return (T*)uptr;
	}
	inline uint8_t* write_later_uint8_t(uint8_t value = 0) { return write_value_later(value); }
	inline uint16_t* write_later_uint16_t(uint16_t value = 0) { return write_value_later(value); }
	inline uint32_t* write_later_uint32_t(uint32_t value = 0) { return write_value_later(value); }
	inline uint64_t* write_later_uint64_t(uint64_t value = 0) { return write_value_later(value); }
	inline int8_t* write_later_int8_t(int8_t value = 0) { return write_value_later(value); }
	inline int16_t* write_later_int16_t(int16_t value = 0) { return write_value_later(value); }
	inline int32_t* write_later_int32_t(int32_t value = 0) { return write_value_later(value); }
	inline int64_t* write_later_int64_t(int64_t value = 0) { return write_value_later(value); }

	/// Write out diff of memory area. Returns number of bytes written out.
	uint64_t write_patch(char* __restrict__ orig, const char* __restrict__ chng, uint32_t offset, uint64_t size); // returns bytes changed
	void write_memory(const char* const ptr, uint64_t offset, uint64_t size);

	void set(const std::string& path);

	void change_default_chunk_size(size_t size) { assert(uidx < size); uncompressed_chunk_size = size; chunk.shrink(size); }

	void disable_multithreaded_compress()
	{
		chunk_mutex.lock();
		done_compressing.exchange(false);
		if (compressor_thread.joinable()) compressor_thread.join();
		multithreaded_compress = false;
		chunk_mutex.unlock();
	}

	void disable_multithreaded_writeout()
	{
		chunk_mutex.lock();
		done_feeding.exchange(false);
		if (serializer_thread.joinable()) serializer_thread.join();
		multithreaded_write = false;
		chunk_mutex.unlock();
	}

	void self_test()
	{
		chunk_mutex.lock();
		if (done_compressing.load()) assert(done_feeding.load());
		if (!holding) assert(held_chunks.size() == 0);
		chunk_mutex.unlock();
	}

protected:
	// these only written to by worker thread until the end when they are read out
	std::vector<uint64_t> compressed_sizes;
	std::vector<uint64_t> uncompressed_sizes;

public:
	/// Stop sending packets to compression, useful if you want to keep pointers to written memory to keep working
	inline void freeze() { if (!holding) holding = true; }

	/// Start compressing packets again, any pointers to written data must now be assumed invalid
	inline void thaw()
	{
		if (!holding) return;
		chunk_mutex.lock();
		uncompressed_chunks.splice(uncompressed_chunks.begin(), held_chunks);
		chunk_mutex.unlock();
		holding = false;
	}

	// These are for test writing
	int count_held_chunks() { int r; chunk_mutex.lock(); r = held_chunks.size(); chunk_mutex.unlock(); return r; }
	int count_uncompressed_chunks() { int r; chunk_mutex.lock(); r = uncompressed_chunks.size(); chunk_mutex.unlock(); return r; }
	int count_compressed_chunks() { int r; chunk_mutex.lock(); r = compressed_chunks.size(); chunk_mutex.unlock(); return r; }

	uint64_t uncompressed_bytes = 0; // total amount of uncompressed bytes written so far

private:
	void compressor(); // runs in separate thread, moves chunks from uncompressed to compressed
	void serializer(); // runs in separate thread, moves chunks from compressed to disk
	buffer compress_chunk(buffer& uncompressed); // returns compressed buffer
	void write_chunk(buffer& active);

	int mTid = -1; // only used for logging
	bool multithreaded_compress = true;
	bool multithreaded_write = true;
	bool holding = false;
	lava::mutex chunk_mutex;
	FILE* fp = nullptr;
	size_t uncompressed_chunk_size = 1024 * 1024 * 64; // use 64mb chunks by default
	unsigned uidx = 0; // index into current uncompressed chunk
	buffer chunk; // current uncompressed chunk
	/// the first chunk in this list is current, the rest are waiting for compression
	std::list<buffer> held_chunks;
	std::list<buffer> uncompressed_chunks GUARDED_BY(chunk_mutex);
	std::list<buffer> compressed_chunks GUARDED_BY(chunk_mutex);
	std::atomic_bool done_feeding;
	std::atomic_bool done_compressing;
	std::string mFilename;
	std::thread compressor_thread;
	std::thread serializer_thread;
};
