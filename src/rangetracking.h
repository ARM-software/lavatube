#pragma once

// Dirty range tracking

#include <algorithm>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <list>
#include <utility>

struct range
{
	uint64_t first;
	uint64_t last;
	inline bool operator==(const range& rhs) const { return first == rhs.first && last == rhs.last; }
};

struct exposure
{
	/// Add by offset + size
	inline void add_os(uint64_t offset, uint64_t size) { if (size > 0) add(offset, offset + size - 1); }

	/// Return span of overlapping elements
	range overlap(const exposure& e, uint64_t offset = 0) const
	{
		if (e.size() == 0 || r.size() == 0) return { 0, 0 };
		const uint64_t first = (e.r.front().first + offset > r.front().first) ? e.r.front().first + offset : r.front().first;
		const uint64_t last = (e.r.back().last + offset < r.back().last) ? e.r.back().last + offset : r.back().last;
		if (first > last) return { 0, 0 }; // no overlap
		return { first, last };
	}

	/// Return span of all contained elements
	range span() const { if (r.size() == 0) return { 0, 0 }; return { r.front().first, r.back().last }; }

	void add(uint64_t start, uint64_t end)
	{
		assert(start <= end);
		if (end == 0) return;
		if (r.empty())
		{
			r.push_back({start, end});
			return;
		}
		auto curr = r.end();
		for (auto iter = r.begin(); iter != r.end(); )
		{
			if (curr != r.end()) // already inserted, remove overlapping later ranges
			{
				// can we now also merge into the next?
				if (curr->last + 1 >= iter->first)
				{
					curr->last = iter->last;
					iter = r.erase(iter); // was overlapping or touching, now check next one
					continue;
				}
				else break; // not overlapping or touching, so we're done
			}
			else if (iter->first <= end + 1 && start <= iter->last + 1) // we can merge
			{
				iter->first = std::min(iter->first, start);
				iter->last = std::max(iter->last, end);
				curr = iter;
			}
			else if (iter->first > end) // whole fragment after us, insert before and trigger first check
			{
				curr = r.insert(iter, { start, end });
			}
			else if (std::next(iter) == r.end()) // at end of list, insert after
			{
				curr = r.insert(std::next(iter), { start, end });
				break; // nothing more to do
			}
			iter++;
		}
	}

	inline range fetch_os(uint64_t offset, uint64_t size, bool is_mapped) { return fetch(offset, offset + size - 1, is_mapped); }
	inline range fetch(range v, bool is_mapped) { return fetch(v.first, v.last, is_mapped); }

	/// Return the smallest exposed area inside the given region, or last=0 if none.
	/// is_mapped means is true if area is currently memory mapped, false if not. If not,
	/// we remove the returned range.
	range fetch(uint64_t start, uint64_t end, bool is_mapped)
	{
		uint64_t a = end;
		uint64_t b = start;
		for (auto iter = r.begin(); iter != r.end(); )
		{
			auto& s = *iter;
			if (s.first > end) break;
			else if ((s.last >= start && s.last <= end) || (s.first <= end && s.first >= start) || (s.first < start && s.last > end))
			{
				a = std::clamp(s.first, start, a);
				b = std::clamp(s.last, b, end);
				assert(s.last > 0);
				if (is_mapped)
				{
					// change nothing
				}
				else if (s.first >= start && s.last <= end) // consumed entirely
				{
					iter = r.erase(iter);
					continue;
				}
				else if (s.first < start && s.last > end) // split in two
				{
					iter = r.insert(std::next(iter), { end + 1, s.last });
					s.last = start - 1;
					continue;
				}
				else if (start == s.first) // remove from start
				{
					s.first = end + 1;
				}
				else if (end == s.last) // remove from end
				{
					s.last = start - 1;
				}
			}
			iter++;
		}
		if (a == end) return { 0, 0 };
#ifdef FULLDEBUG
		assert(b >= a);
		assert(r.empty() || r.front().first <= r.back().last);
		assert(span().first <= span().last);
		self_test();
#endif
		return range{ a, b };
	}

	void self_test() const
	{
		long prev = -1;
		for (auto& s : r)
		{
			assert(s.first <= s.last);
			assert((long)s.first > prev);
			prev = s.last;
			(void)prev; // silence compiler warning for release builds
		}
	}

	inline const std::list<range>& list() const { return r; }
	inline size_t bytes() const { size_t v = 0; for (auto& s : r) { v += 1 + s.last - s.first; } return v; }
	inline void clear() { r.clear(); }
	inline size_t size() const { return r.size(); }

private:
	std::list<range> r;
};
