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
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <algorithm>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "vulkan/vulkan.h"

#include "lavamutex.h"
#include "unordered_dense/include/ankerl/unordered_dense.h"
#include "spirv-simulator/framework/memory_flag_tracker.hpp"

struct change_source
{
	uint32_t call = UINT32_MAX;
	uint32_t frame = UINT32_MAX; // global frame
	uint8_t thread = UINT8_MAX;
	uint8_t packet_type = UINT8_MAX;
	uint16_t call_id = UINT16_MAX; // type of call

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
	concurrent_unordered_map() = default;

	concurrent_unordered_map(const concurrent_unordered_map& other)
	{
		std::lock_guard<std::mutex> lock(other.mutex);
		map = other.map;
	}

	concurrent_unordered_map& operator=(const concurrent_unordered_map& other)
	{
		if (this == &other) return *this;
		std::scoped_lock lock(mutex, other.mutex);
		map = other.map;
		return *this;
	}

	concurrent_unordered_map(concurrent_unordered_map&& other) noexcept
	{
		std::lock_guard<std::mutex> lock(other.mutex);
		map = std::move(other.map);
	}

	concurrent_unordered_map& operator=(concurrent_unordered_map&& other) noexcept
	{
		if (this == &other) return *this;
		std::scoped_lock lock(mutex, other.mutex);
		map = std::move(other.map);
		return *this;
	}

	U at(T key) const { std::lock_guard<std::mutex> lock(mutex); return map.at(key); } // must be fast
	void clear() { std::lock_guard<std::mutex> lock(mutex); map.clear(); } // can be unsafe
	void insert(T key, U value) { std::lock_guard<std::mutex> lock(mutex); map.insert_or_assign(key, value); }
	void reserve(size_t count) { std::lock_guard<std::mutex> lock(mutex); map.reserve(count); }
	int count(T key) const { std::lock_guard<std::mutex> lock(mutex); return map.count(key); }
	unsigned size() const { std::lock_guard<std::mutex> lock(mutex); return static_cast<unsigned>(map.size()); }

private:
	mutable std::mutex mutex;
	ankerl::unordered_dense::map<T, U> map;
};

/// Track host write regions for post-process analysis
struct host_write_regions
{
public:
	using tracker_type = SPIRVSimulator::MemoryFlagTracker;
	using fragment_id_type = tracker_type::FragmentId;

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
		tracker = other.tracker;
		fragment_sources = other.fragment_sources;
	}

	host_write_regions& operator=(const host_write_regions& other)
	{
		if (this == &other) return *this;
		std::unique_lock lock_this(mutex, std::defer_lock);
		std::unique_lock lock_other(other.mutex, std::defer_lock);
		std::lock(lock_this, lock_other);
		tracker = other.tracker;
		fragment_sources = other.fragment_sources;
		return *this;
	}

	host_write_regions(host_write_regions&& other) noexcept
	{
		std::unique_lock lock(other.mutex);
		tracker = std::move(other.tracker);
		fragment_sources = std::move(other.fragment_sources);
	}

	host_write_regions& operator=(host_write_regions&& other) noexcept
	{
		if (this == &other) return *this;
		std::unique_lock lock_this(mutex, std::defer_lock);
		std::unique_lock lock_other(other.mutex, std::defer_lock);
		std::lock(lock_this, lock_other);
		tracker = std::move(other.tracker);
		fragment_sources = std::move(other.fragment_sources);
		return *this;
	}

	change_source get_source(uint64_t address, uint32_t size) const
	{
		change_source source;
		if (!try_get_source(address, size, source))
		{
			assert(false && "host_write_regions missing coverage");
			return {};
		}
		return source;
	}

	bool try_get_source(uint64_t address, uint32_t size, change_source& source) const
	{
		std::shared_lock lock(mutex);
		assert(size > 0);
		std::vector<source_span> spans;
		if (!collect_spans_unlocked(tracker, fragment_sources, address, size, spans)) return false;
		assert(!spans.empty());
		source = spans.front().source;
		for (const source_span& span : spans)
		{
			if (!same_source(span.source, source)) return false;
		}
		return true;
	}

	stats get_stats() const
	{
		std::shared_lock lock(mutex);
		stats out;
		const auto spans = tracker.queryRangeDetailed(0, std::numeric_limits<uint64_t>::max());
		change_source previous = {};
		bool have_previous = false;
		uint64_t previous_end = 0;
		for (const auto& span : spans)
		{
			change_source source;
			if (!lookup_source_unlocked(fragment_sources, span.fragment_id, source))
			{
				assert(false && "host_write_regions missing source mapping");
				continue;
			}
			if (!have_previous || previous_end != span.start || !same_source(previous, source))
			{
				out.segments++;
				previous = source;
				have_previous = true;
			}
			previous_end = span.end;
			out.bytes += (span.end - span.start);
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
			register_source_unlocked(address, size, source);
			return;
		}

		if (stride == 0)
		{
			const uint64_t total = size * (uint64_t)elements;
			assert(total / elements == size);
			register_source_unlocked(address, total, source);
			return;
		}

		for (uint32_t i = 0; i < elements; ++i)
		{
			const uint64_t start = address + (uint64_t)i * (uint64_t)stride;
			register_source_unlocked(start, size, source);
		}
	}

	void copy_sources(const host_write_regions& regions, uint64_t dst_address, uint64_t src_address, uint64_t src_size)
	{
		if (src_size == 0) return;
		checked_end(dst_address, src_size);
		std::vector<source_span> pending;

		if (this == &regions)
		{
			std::unique_lock lock(mutex);
			collect_spans_unlocked(tracker, fragment_sources, src_address, src_size, pending, false);
			for (const source_span& entry : pending)
			{
				const uint64_t dst_start = dst_address + (entry.start - src_address);
				register_source_unlocked(dst_start, entry.end - entry.start, entry.source);
			}
			return;
		}

		{
			std::shared_lock lock(regions.mutex);
			collect_spans_unlocked(regions.tracker, regions.fragment_sources, src_address, src_size, pending, false);
		}

		if (pending.empty()) return;

		std::unique_lock lock(mutex);
		for (const source_span& entry : pending)
		{
			const uint64_t dst_start = dst_address + (entry.start - src_address);
			register_source_unlocked(dst_start, entry.end - entry.start, entry.source);
		}
	}

private:
	struct source_span
	{
		uint64_t start = 0;
		uint64_t end = 0;
		change_source source;
	};

	static bool same_source(const change_source& a, const change_source& b)
	{
		return a.call == b.call && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
	}

	static uint64_t checked_end(uint64_t address, uint64_t size)
	{
		const uint64_t end = address + size;
		assert(end >= address);
		return end;
	}

	static bool lookup_source_unlocked(const std::map<fragment_id_type, change_source>& sources, fragment_id_type fragment_id, change_source& out)
	{
		auto it = sources.find(fragment_id);
		if (it == sources.end()) return false;
		out = it->second;
		return true;
	}

	static bool collect_spans_unlocked(const tracker_type& tracker, const std::map<fragment_id_type, change_source>& sources, uint64_t address, uint64_t size, std::vector<source_span>& out, bool require_full_coverage = true)
	{
		out.clear();
		if (size == 0) return true;

		const uint64_t end = checked_end(address, size);
		const auto spans = tracker.queryRangeDetailed(address, size);
		uint64_t pos = address;
		for (const auto& span : spans)
		{
			if (require_full_coverage && span.start > pos)
			{
				return false;
			}
			const uint64_t start = std::max<uint64_t>(span.start, require_full_coverage ? pos : address);
			const uint64_t span_end = std::min<uint64_t>(span.end, end);
			if (start >= span_end) continue;

			change_source source;
			if (!lookup_source_unlocked(sources, span.fragment_id, source))
			{
				if (require_full_coverage) return false;
				continue;
			}

			if (!out.empty() && out.back().end == start && same_source(out.back().source, source))
			{
				out.back().end = span_end;
			}
			else
			{
				out.push_back({ start, span_end, source });
			}
			if (require_full_coverage)
			{
				pos = span_end;
				if (pos == end) break;
			}
		}
		return require_full_coverage ? pos == end : true;
	}

	void register_source_unlocked(uint64_t address, uint64_t size, const change_source& source)
	{
		if (size == 0) return;
		tracker.write(address, size, 0);
		const auto result = tracker.queryDetailed(address);
		assert(result.has_value());
		assert(result->address == address);
		fragment_sources[result->fragment_id] = source;
	}

	tracker_type tracker;
	std::map<fragment_id_type, change_source> fragment_sources;
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
		bool best_is_bound = false;
		bool best_is_destroyed = true;
		for (T* obj : entries)
		{
			if (!obj) continue;
			bool is_bound = false;
			bool is_destroyed = false;
			if constexpr (requires(const T& value) { value.is_state(T::states::destroyed); })
			{
				is_destroyed = obj->is_state(T::states::destroyed);
			}
			if constexpr (requires(const T& value) { value.object_type; })
			{
				if (is_destroyed && obj->object_type != VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR) continue;
			}
			if constexpr (requires(const T& value) { value.is_state(T::states::bound); })
			{
				is_bound = obj->is_state(T::states::bound);
			}
			const uint64_t offset = stored - base;
			if (obj->size == 0) continue;
			if (offset >= obj->size) continue;
			const bool has_addr = obj->device_address != 0;
			if (!best || (has_addr && !best_has_addr) ||
				(has_addr == best_has_addr && !is_destroyed && best_is_destroyed) ||
				(has_addr == best_has_addr && is_bound && !best_is_bound) ||
				(has_addr == best_has_addr && is_destroyed == best_is_destroyed && is_bound == best_is_bound &&
				 (obj->size > best_size || obj->size == best_size)))
			{
				best = obj;
				best_size = obj->size;
				best_has_addr = has_addr;
				best_is_bound = is_bound;
				best_is_destroyed = is_destroyed;
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
	/// Create a moderately sized memory pool
	memory_pool(unsigned mbs = 32) : pool(mbs * 1024 * 1024) {}

	template<typename T> __attribute__((alloc_size(2, 3)))
	inline T* allocate(size_t _count, const size_t _size = sizeof(T))
	{
		if (_count == 0) return nullptr;
		const size_t alignment = std::max<size_t>(alignof(T), 2);
		const size_t allocsize = _count * sizeof(T);
		if (index + allocsize > pool.size()) return nullptr;
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
	inline T* allocate_aligned(size_t _count, size_t alignment, const size_t _size = sizeof(T))
	{
		if (_count == 0) return nullptr;
		const size_t allocsize = _count * sizeof(T);
		if (index + allocsize > pool.size()) return nullptr;
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
	inline void spend(size_t _count) { index += _count; }

	inline void reset()
	{
		index = 0;
	}

private:
	std::vector<char> pool;
	size_t index = 0; // index to next allocation
};

template<typename T>
class replay_remap
{
public:
	/// Will only be called once, before any of the other functions below are called.
	inline void resize(uint32_t _size)
	{
		remapping.resize(_size);
		reverse.reserve(_size);
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

	inline uint32_t index_or_invalid(T handle) const
	{
		if (handle == 0) return CONTAINER_NULL_VALUE;
		if (reverse.count(handle) == 0) return CONTAINER_INVALID_INDEX;
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
	inline U* add(T key, const change_source& current, uint32_t desired_index = CONTAINER_INVALID_INDEX)
	{
		std::lock_guard<std::mutex> lock(mutex);
		assert(key != 0);
		const bool uses_desired_index = desired_index != CONTAINER_INVALID_INDEX;
		const index_mode requested_mode = uses_desired_index ? index_mode::desired : index_mode::automatic;
		if (mode == index_mode::unknown) mode = requested_mode;
		assert(mode == requested_mode);
		U* p = new U;
		p->index = uses_desired_index ? desired_index : _size++;
		p->creation = current;
		p->last_modified = current;
		lookup.insert(key, p);
		const auto it = std::lower_bound(storage.begin(), storage.end(), p->index, [](const U* lhs, uint32_t rhs) { return lhs->index < rhs; });
		assert(it == storage.end() || (*it)->index != p->index);
		storage.insert(it, p);
		if (uses_desired_index) _size.store(std::max(_size.load(), desired_index + 1));
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
		std::lock_guard<std::mutex> lock(mutex);
		lookup.clear();
		for (auto* p : storage) delete p;
		storage.clear();
		_size.store(0);
		mode = index_mode::unknown;
		lookup.insert(0, nullptr); // special case VK_NULL_HANDLE
	}

	/// This is _not_ thread-safe, only use once all other threads have stopped running.
	const std::vector<U*>& iterate() const { return storage; }

	uint32_t size() const { return _size.load(); }

private:
	enum class index_mode : uint8_t { unknown, automatic, desired };

	std::mutex mutex;
	std::atomic_uint_least32_t _size;
	index_mode mode = index_mode::unknown;
	concurrent_unordered_map<T, U*> lookup;
	std::vector<U*> storage;
};
