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

/// Track host write regions for post-process analysis
struct host_write_regions
{
public:
	struct stats
	{
		uint64_t segments = 0;
		uint64_t bytes = 0;

		bool empty() const { return segments == 0; }
	};

	host_write_regions() = default;

	host_write_regions(const host_write_regions& other)
	{
		std::unique_lock lock(other.mutex);
		data = other.data;
	}

	host_write_regions& operator=(const host_write_regions& other)
	{
		if (this == &other) return *this;
		std::unique_lock lock_this(mutex, std::defer_lock);
		std::unique_lock lock_other(other.mutex, std::defer_lock);
		std::lock(lock_this, lock_other);
		data = other.data;
		return *this;
	}

	host_write_regions(host_write_regions&& other) noexcept
	{
		std::unique_lock lock(other.mutex);
		data = std::move(other.data);
	}

	host_write_regions& operator=(host_write_regions&& other) noexcept
	{
		if (this == &other) return *this;
		std::unique_lock lock_this(mutex, std::defer_lock);
		std::unique_lock lock_other(other.mutex, std::defer_lock);
		std::lock(lock_this, lock_other);
		data = std::move(other.data);
		return *this;
	}

	change_source get_source(uint64_t address, uint32_t size) const
	{
		std::shared_lock lock(mutex);
		assert(size > 0);
		const uint64_t end = address + (uint64_t)size;
		assert(end >= address);

		auto it = data.lower_bound(address);
		if (it != data.begin())
		{
			auto prev = std::prev(it);
			if (prev->second.end > address)
			{
				it = prev;
			}
		}
		assert(it != data.end());
		assert(it->first <= address);
		assert(it->second.end > address);

		const change_source source = it->second.source;
		uint64_t pos = address;
		while (pos < end)
		{
			if (it == data.end() || it->first > pos)
			{
				assert(false && "host_write_regions missing coverage");
				return source;
			}
			if (!same_source(it->second.source, source))
			{
				assert(false && "host_write_regions mixed sources");
				return source;
			}
			const uint64_t seg_end = it->second.end;
			if (seg_end <= pos)
			{
				++it;
				continue;
			}
			pos = seg_end;
			if (pos < end)
			{
				++it;
			}
		}
		return source;
	}

	stats get_stats() const
	{
		std::shared_lock lock(mutex);
		stats out;
		out.segments = data.size();
		for (const auto& entry : data)
		{
			if (entry.second.end > entry.first)
			{
				out.bytes += (entry.second.end - entry.first);
			}
		}
		return out;
	}

	void register_source(uint64_t address, uint64_t size, change_source source, uint32_t elements = 1, uint32_t stride = 0)
	{
		if (size == 0) return;
		std::unique_lock lock(mutex);
		assert(elements > 0);
		source.self_test();

		if (elements == 1)
		{
			const uint64_t end = address + size;
			assert(end >= address);
			insert_range(address, end, source);
			return;
		}

		if (stride == 0)
		{
			const uint64_t total = size * (uint64_t)elements;
			assert(total / elements == size);
			const uint64_t end = address + total;
			assert(end >= address);
			insert_range(address, end, source);
			return;
		}

		for (uint32_t i = 0; i < elements; ++i)
		{
			const uint64_t start = address + (uint64_t)i * (uint64_t)stride;
			const uint64_t end = start + size;
			assert(end >= start);
			insert_range(start, end, source);
		}
	}

	void copy_sources(const host_write_regions& regions, uint64_t dst_address, uint64_t src_address, uint64_t src_size)
	{
		if (src_size == 0) return;
		const uint64_t src_end = src_address + src_size;
		assert(src_end >= src_address);

		struct pending_range
		{
			uint64_t start = 0;
			uint64_t end = 0;
			change_source source;
		};

		std::vector<pending_range> pending;

		if (this == &regions)
		{
			std::unique_lock lock(mutex);
			auto it = data.lower_bound(src_address);
			if (it != data.begin())
			{
				auto prev = std::prev(it);
				if (prev->second.end > src_address)
				{
					it = prev;
				}
			}

			uint64_t pos = src_address;
			while (pos < src_end)
			{
				if (it == data.end() || it->first > pos)
				{
					assert(false && "host_write_regions missing coverage");
					return;
				}
				const uint64_t seg_start = std::max<uint64_t>(it->first, src_address);
				const uint64_t seg_end = std::min<uint64_t>(it->second.end, src_end);
				if (seg_start > pos)
				{
					assert(false && "host_write_regions missing coverage");
					return;
				}
				const uint64_t dst_start = dst_address + (seg_start - src_address);
				const uint64_t dst_end = dst_start + (seg_end - seg_start);
				if (!pending.empty() && pending.back().end == dst_start && same_source(pending.back().source, it->second.source))
				{
					pending.back().end = dst_end;
				}
				else
				{
					pending.push_back({ dst_start, dst_end, it->second.source });
				}
				pos = seg_end;
				if (seg_end >= it->second.end)
				{
					++it;
				}
			}

			for (const auto& entry : pending)
			{
				insert_range(entry.start, entry.end, entry.source);
			}
			return;
		}

		{
			std::shared_lock lock(regions.mutex);
			auto it = regions.data.lower_bound(src_address);
			if (it != regions.data.begin())
			{
				auto prev = std::prev(it);
				if (prev->second.end > src_address)
				{
					it = prev;
				}
			}

			uint64_t pos = src_address;
			while (pos < src_end)
			{
				if (it == regions.data.end() || it->first > pos)
				{
					assert(false && "host_write_regions missing coverage");
					return;
				}
				const uint64_t seg_start = std::max<uint64_t>(it->first, src_address);
				const uint64_t seg_end = std::min<uint64_t>(it->second.end, src_end);
				if (seg_start > pos)
				{
					assert(false && "host_write_regions missing coverage");
					return;
				}
				const uint64_t dst_start = dst_address + (seg_start - src_address);
				const uint64_t dst_end = dst_start + (seg_end - seg_start);
				if (!pending.empty() && pending.back().end == dst_start && same_source(pending.back().source, it->second.source))
				{
					pending.back().end = dst_end;
				}
				else
				{
					pending.push_back({ dst_start, dst_end, it->second.source });
				}
				pos = seg_end;
				if (seg_end >= it->second.end)
				{
					++it;
				}
			}
		}

		if (pending.empty()) return;

		std::unique_lock lock(mutex);
		for (const auto& entry : pending)
		{
			insert_range(entry.start, entry.end, entry.source);
		}
	}

private:
	struct segment
	{
		uint64_t end = 0;
		change_source source;
	};

	static bool same_source(const change_source& a, const change_source& b)
	{
		return a.call == b.call && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
	}

	void erase_range(uint64_t start, uint64_t end)
	{
		if (start >= end) return;

		auto it = data.lower_bound(start);
		if (it != data.begin())
		{
			auto prev = std::prev(it);
			if (prev->second.end > start)
			{
				it = prev;
			}
		}

		while (it != data.end() && it->first < end)
		{
			const uint64_t seg_start = it->first;
			const uint64_t seg_end = it->second.end;
			const change_source seg_source = it->second.source;

			if (seg_start < start && seg_end > end)
			{
				it->second.end = start;
				data.emplace(end, segment{ seg_end, seg_source });
				return;
			}

			if (seg_start < start && seg_end > start)
			{
				it->second.end = start;
				++it;
				continue;
			}

			if (seg_start < end && seg_end > end)
			{
				data.erase(it++);
				data.emplace(end, segment{ seg_end, seg_source });
				return;
			}

			it = data.erase(it);
		}
	}

	void insert_range(uint64_t start, uint64_t end, const change_source& source)
	{
		if (start >= end) return;

		erase_range(start, end);

		uint64_t new_start = start;
		uint64_t new_end = end;

		auto it = data.lower_bound(start);
		if (it != data.begin())
		{
			auto prev = std::prev(it);
			if (prev->second.end == start && same_source(prev->second.source, source))
			{
				new_start = prev->first;
				data.erase(prev);
			}
		}

		auto next = data.lower_bound(end);
		if (next != data.end() && next->first == end && same_source(next->second.source, source))
		{
			new_end = next->second.end;
			data.erase(next);
		}

		data.emplace(new_start, segment{ new_end, source });
	}

	std::map<uint64_t, segment> data;
	mutable std::shared_mutex mutex;
};

struct host_write_totals
{
	uint64_t objects = 0;
	uint64_t objects_with_data = 0;
	uint64_t segments = 0;
	uint64_t bytes = 0;
	uint64_t max_segments = 0;
	uint32_t max_index = CONTAINER_INVALID_INDEX;
};

template <typename T>
inline host_write_totals gather_host_write_stats(const std::vector<T>& objects)
{
	host_write_totals totals;
	for (const auto& obj : objects)
	{
		const host_write_regions::stats stats = obj.source.get_stats();
		totals.objects++;
		if (!stats.empty())
		{
			totals.objects_with_data++;
			totals.segments += stats.segments;
			totals.bytes += stats.bytes;
			if (stats.segments > totals.max_segments)
			{
				totals.max_segments = stats.segments;
				totals.max_index = obj.index;
			}
		}
	}
	return totals;
}

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

	inline uint32_t index_or_null(T key) const
	{
		if (key == 0) return CONTAINER_NULL_VALUE;
		U* p = lookup.at(key);
		assert(p != nullptr);
		return p->index;
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
