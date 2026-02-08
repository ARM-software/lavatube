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
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <algorithm>

#include "tbb/concurrent_unordered_map.h"

struct change_source
{
	uint32_t call = UINT32_MAX;
	uint32_t frame = UINT32_MAX;
	uint16_t thread = UINT16_MAX;
	uint16_t call_id = UINT16_MAX;

	void self_test() const
	{
		assert(call != UINT32_MAX);
		assert(thread < 4096);
		assert(call_id != UINT16_MAX);
		assert(frame != UINT32_MAX);
	}
};

/// Defining the minimum interface to a concurrent map that we need.
template<typename T, typename U>
struct concurrent_unordered_map
{
public:
	U at(T key) const { return map.at(key); } // must be fast
	void clear() { map.clear(); } // can be unsafe
	void insert(T key, U value) { map[key] = value; }
	int count(T key) const { return map.count(key); }
	unsigned size() const { return map.size(); }

private:
	tbb::concurrent_unordered_map<T, U> map;
};

/// Limited thread safe remapping allocator for memory addresses used for replay. The payload needs to
/// store size in a 'size' member and its own remapped address in a 'device_address' member.
template<typename T>
class address_remapper
{
	struct candidate
	{
		T* obj = nullptr;
		uint64_t base = 0;
	};

	T* choose_best(const std::vector<T*>& entries, uint64_t base, uint64_t stored) const
	{
		T* best = nullptr;
		uint64_t best_size = 0;
		bool best_has_addr = false;
		for (T* obj : entries)
		{
			if (!obj) continue;
			const uint64_t offset = stored - base;
			if (obj->size == 0) continue;
			if (offset >= obj->size) continue;
			const bool has_addr = obj->device_address != 0;
			if (!best || (has_addr && !best_has_addr) ||
				(has_addr == best_has_addr && (obj->size > best_size || obj->size == best_size)))
			{
				best = obj;
				best_size = obj->size;
				best_has_addr = has_addr;
			}
		}
		return best;
	}

	candidate find_candidate(uint64_t stored) const
	{
		if (remapping.empty()) return {};
		auto iter = remapping.upper_bound(stored);
		while (iter != remapping.begin())
		{
			--iter;
			const uint64_t base = iter->first;
			if (base > stored) continue;
			if (T* obj = choose_best(iter->second, base, stored))
			{
				return { obj, base };
			}
		}
		return {};
	}

public:
	/// Get stored object. Thread safe.
	T* get_by_address(uint64_t stored) const
	{
		std::shared_lock lock(mutex);
		candidate found = find_candidate(stored);
		return found.obj;
        }

	/// Translate an address. Thread safe.
	uint64_t translate_address(uint64_t stored) const
	{
		std::shared_lock lock(mutex);
		candidate found = find_candidate(stored);
		if (!found.obj || found.obj->device_address == 0) return 0;
		return found.obj->device_address + (stored - found.base);
        }

	/// Check if a value is a candidate for being a stored memory address. Also checks 32bit swapped addresses.
	/// Thread safe as long as only used after any calls to add() or iter().
	bool is_candidate(uint64_t stored) const
	{
		return translate_address(stored) != 0 || translate_address((stored >> 32) | (stored << 32));
	}

	/// Add an address translation. 'addr' is the stored address. Thread safe.
	void add(uint64_t addr, T* obj)
	{
		if (!obj || addr == 0) return;
		std::unique_lock lock(mutex);
		auto& entries = remapping[addr];
		if (std::find(entries.begin(), entries.end(), obj) == entries.end())
		{
			entries.push_back(obj);
		}
	}

	/// Remove an address translation. Thread safe.
	void remove(uint64_t addr, T* obj)
	{
		if (!obj || addr == 0) return;
		std::unique_lock lock(mutex);
		auto it = remapping.find(addr);
		if (it == remapping.end()) return;
		auto& entries = it->second;
		entries.erase(std::remove(entries.begin(), entries.end(), obj), entries.end());
		if (entries.empty()) remapping.erase(it);
	}

	/// Get underlying container. Unsafe. Only use when externally synchronized.
	const std::map<uint64_t, std::vector<T*>>& iter() const { return remapping; }

private:
	std::map<uint64_t, std::vector<T*>> remapping;
	mutable std::shared_mutex mutex;
};

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

	template<typename T> __attribute__((alloc_size(2, 3)))
	inline T* allocate(uint32_t _count, const uint32_t _size = sizeof(T))
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

	template<typename T> __attribute__((alloc_size(2, 4)))
	inline T* allocate_aligned(uint32_t _count, size_t alignment, const uint32_t _size = sizeof(T))
	{
		if (_count == 0) return nullptr;
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
		reverse.insert(handle, index);
	}

	/// Needed in some rare cases. Caller must make sure nobody else calls this on the
	/// same index at the same time.
	inline void replace(uint32_t index, T handle)
	{
		remapping[index] = handle;
		reverse.insert(handle, index);
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
		reverse.insert(remapping.at(index), 0);
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
	concurrent_unordered_map<T, uint32_t> reverse;
};

template<typename T, typename U>
class trace_remap
{
public:
	trace_remap()
	{
		_size.store(0);
		lookup.insert(0, nullptr); // special case VK_NULL_HANDLE
	}
	~trace_remap() { clear(); }

	/// Might be called simultaneously with itself and at() but never with the same key.
	inline U* add(T key, const change_source& current)
	{
		mutex.lock();
		assert(key != 0);
		U* p = new U;
		p->index = _size++;
		p->creation = current;
		p->last_modified = current;
		lookup.insert(key, p);
		storage.push_back(p);
		mutex.unlock();
		return p;
	}

	inline U* unset(T key, change_source current)
	{
		if (key == 0) return nullptr;
		U* p = lookup.at(key);
		p->destroyed = current;
		lookup.insert(key, nullptr);
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
		lookup.insert(0, nullptr); // special case VK_NULL_HANDLE
		mutex.unlock();
	}

	/// This is _not_ thread-safe, only use once all other threads have stopped running.
	const std::vector<U*>& iterate() const { return storage; }

	uint32_t size() const { return _size.load(); }

private:
	std::mutex mutex;
	std::atomic_uint_least32_t _size;
	concurrent_unordered_map<T, U*> lookup;
	std::vector<U*> storage;
};
