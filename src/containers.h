#pragma once

// This header implements our special container interfaces. This has a dual purpose:
// Both to simplify the interface for code that uses it, and to make it easier to
// change the underlying implementation to something faster later on.

#define CONTAINER_INVALID_INDEX (UINT32_MAX)
#define CONTAINER_NULL_VALUE (UINT32_MAX-1)

#include <assert.h>
#include <vector>
#include <atomic>
#include <stdint.h>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "tbb/concurrent_unordered_map.h"

/// A very limited RCU-based lockless concurrent vector implementation. Compared to a
/// TBB concurrent_vector, the push_back() is slower but the at() is ~2x faster and we
/// push data to it much more rarely than we read it.
template<typename T>
class trace_data
{
public:
	using value_type = T;
	trace_data() : msize(0), current(new std::vector<T*>(64, nullptr)) {}
	~trace_data() { clear(); delete current.load(std::memory_order_relaxed); }

	/// This is thread safe against already added index values.
	inline T& at(uint32_t index) const { return *current.load(std::memory_order_relaxed)->at(index); }

	// The functions below must hold a mutex to protect against simultaneous calls
	// from other functions below or you must otherwise be sure that no such access
	// may happen.

	inline size_t size() const { return msize.load(std::memory_order_relaxed); } // iteration up to size is not safe! use a mutex
	template<class... Args>	inline void emplace_back(Args&&... args)
	{
		auto* vec = current.load(std::memory_order_relaxed);
		if (vec->size() == msize.load(std::memory_order_relaxed))
		{
			old.push_back(vec);
			std::vector<T*>* make = new std::vector<T*>(vec->size() * 2, nullptr);
			memcpy(make->data(), vec->data(), sizeof(T*) * vec->size());
			make->at(msize++) = new value_type(std::forward<Args>(args)...);
			current.store(make, std::memory_order_relaxed);
		}
		else vec->at(msize++) = new value_type(std::forward<Args>(args)...);
	}
	inline void push_back(const T& t) { emplace_back(t); }

	/// This cannot be called simultaneously with any other method here, including itself.
	inline void clear()
	{
		int i = msize.load(std::memory_order_relaxed) - 1;
		while (i >= 0) delete current.load(std::memory_order_relaxed)->at(i--);
		for (auto* t : old) delete t;
		old.clear();
		msize.store(0, std::memory_order_relaxed); // keep old capacity
	}

private:
	std::atomic_uint_least32_t msize;
	std::atomic<std::vector<T*>*> current;
	std::vector<const std::vector<T*>*> old;
};

/// Fast memory pool for function arguments
class memory_pool
{
public:
	/// Create a 4mb size memory pool
	memory_pool(unsigned mbs = 4) : pool(mbs * 1024 * 1024) {}

	template<typename T>
	inline T* allocate(uint32_t _count)
	{
		if (_count == 0) return nullptr;
		const size_t alignment = std::max<size_t>(alignof(T), 2);
		const uint_fast32_t allocsize = _count * sizeof(T);
		size_t space = pool.size() - (index + allocsize);
		void* retval = pool.data() + index;
		if (!std::align(alignment, allocsize, retval, space))
		{
			return nullptr;
		}
		index = pool.size() - space;
		return (T*)retval;
	}

	// these are useful for making custom utility functions; do not call directly
	template<typename T> inline T* pointer() { return (T*)(pool.data() + index); }
	inline void spend(uint32_t _count) { index += _count; }

	inline void reset()
	{
		index = 0;
	}

private:
	std::vector<char> pool;
	uint_fast32_t index = 0; // index to next allocation
};

template<typename T>
class replay_remap
{
public:
	/// Will only be called once, before any of the other functions below are called.
	inline void resize(uint32_t _size)
	{
		remapping.resize(_size);
	}

	/// Will only be called once for each index. Caller must make sure nobody else calls
	/// this or replace on the same value at the same time.
	inline void set(uint32_t index, T handle)
	{
		assert(remapping.at(index) == 0);
		assert(handle != 0);
		remapping[index] = handle;
		reverse[handle] = index;
	}

	/// Needed in some rare cases. Caller must make sure nobody else calls this on the
	/// same index at the same time.
	inline void replace(uint32_t index, T handle)
	{
		remapping[index] = handle;
		reverse[handle] = index;
	}

	inline T at(uint32_t index) const
	{
		if (index == CONTAINER_NULL_VALUE) return 0;
		T v = remapping.at(index);
		assert(v != 0);
		return v;
	}

	inline uint32_t index(T handle) const
	{
		if (handle == 0) return 0;
		return reverse.at(handle);
	}

	inline bool contains(uint32_t index) const
	{
		if (index == CONTAINER_NULL_VALUE) return false;
		return remapping.at(index) != 0;
	}

	inline void unset(uint32_t index)
	{
		if (index == CONTAINER_NULL_VALUE) return;
		assert(remapping.at(index) != 0);
		reverse[remapping.at(index)] = 0;
		remapping[index] = 0;
	}

	inline void clear() // not thread-safe
	{
		remapping.clear();
		reverse.clear();
	}

	inline size_t size() const
	{
		return reverse.size();
	}

private:
	std::vector<T> remapping;
	std::unordered_map<T, uint32_t> reverse;
};

template<typename T, typename U>
class trace_remap
{
public:
	trace_remap()
	{
		_size.store(0);
		lookup[0] = nullptr; // special case VK_NULL_HANDLE
	}
	~trace_remap() { clear(); }

	/// Might be called simultaneously with itself and at() but never with the same key.
	inline U* add(T key, int frame = 0)
	{
		mutex.lock();
		assert(key != 0);
		U* p = new U;
		p->index = _size++;
		p->frame_created = frame;
		lookup[key] = p;
		storage.push_back(p);
		mutex.unlock();
		return p;
	}

	inline U* unset(T key, int frame = 0)
	{
		if (key == 0) return nullptr;
		U* p = lookup.at(key);
		p->frame_destroyed = frame;
		lookup[key] = nullptr;
		return p;
	}

	/// Thread safe and must be very fast. Might be called simultaneously with
	/// itself, contains(), unset() and add() with different keys, and simultaneously
	/// with itself with the same key.
	inline U* at(const T key) const
	{
		return lookup.at(key);
	}

	inline bool contains(const T key) const
	{
		return (key != 0 && lookup.count(key) > 0 && lookup.at(key) != nullptr);
	}

	/// Must not be called simultaneously with any other function in this API.
	inline void clear()
	{
		mutex.lock();
		lookup.clear();
		for (auto* p : storage) delete p;
		storage.clear();
		_size.store(0);
		lookup[0] = nullptr; // special case VK_NULL_HANDLE
		mutex.unlock();
	}

	/// This is _not_ thread-safe, only use once all other threads have stopped running.
	const std::vector<U*>& iterate() const { return storage; }

	uint32_t size() const { return _size.load(); }

private:
	std::mutex mutex;
	std::atomic_uint_least32_t _size;
	tbb::concurrent_unordered_map<T, U*> lookup;
	std::vector<U*> storage;
};
