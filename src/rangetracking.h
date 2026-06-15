#pragma once

// Dirty range tracking

#include <algorithm>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <utility>
#include <vector>

struct range
{
	uint64_t first;
	uint64_t last;
	inline bool operator==(const range& rhs) const { return first == rhs.first && last == rhs.last; }
	inline bool valid() const { return first <= last; }
	static inline range invalid() { return { 1, 0 }; }
};

struct exposure
{
	/// Add by offset + size
	inline void add_os(uint64_t offset, uint64_t size) { if (size > 0) add(offset, offset + size - 1); }

	/// Return span of overlapping elements
	range overlap(const exposure& e, uint64_t offset = 0) const
	{
		normalize();
		e.normalize();
		range retval = range::invalid();
		if (e.r.empty() || r.empty()) return retval;
		if (e.r.front().first > UINT64_MAX - offset) return retval;

		const uint64_t first_other = e.r.front().first + offset;
		const uint64_t last_other = (e.r.back().last > UINT64_MAX - offset) ? UINT64_MAX : e.r.back().last + offset;
		if (last_other < r.front().first || first_other > r.back().last) return retval;
		if (e.r.size() == 1 && r.size() == 1)
		{
			return {
				std::max(r.front().first, first_other),
				std::min(r.front().last, last_other)
			};
		}

		auto iter = r.begin();
		auto other = e.r.begin();
		while (iter != r.end() && other != e.r.end())
		{
			if (other->first > UINT64_MAX - offset) break;
			const uint64_t other_first = other->first + offset;
			const uint64_t other_last = (other->last > UINT64_MAX - offset) ? UINT64_MAX : other->last + offset;

			if (iter->last < other_first)
			{
				iter++;
				continue;
			}
			if (other_last < iter->first)
			{
				other++;
				continue;
			}

			const uint64_t first = std::max(iter->first, other_first);
			const uint64_t last = std::min(iter->last, other_last);
			if (!retval.valid())
			{
				retval = { first, last };
			}
			else
			{
				retval.first = std::min(retval.first, first);
				retval.last = std::max(retval.last, last);
			}

			if (iter->last < other_last) iter++;
			else other++;
		}
		return retval;
	}

	/// Return span of all contained elements
	range span() const
	{
		if (r.empty()) return range::invalid();
		if (reversed) return { r.back().first, r.front().last };
		return { r.front().first, r.back().last };
	}

	void add(uint64_t start, uint64_t end)
	{
		assert(start <= end);
		if (r.empty())
		{
			r.push_back({start, end});
			return;
		}
		if (reversed)
		{
			if (end != UINT64_MAX && end + 1 < r.back().first)
			{
				r.push_back({ start, end });
				return;
			}
			normalize();
		}
		if (end != UINT64_MAX && end + 1 < r.front().first)
		{
			if (r.size() == 1)
			{
				r.push_back({ start, end });
				reversed = true;
			}
			else
			{
				r.insert(r.begin(), { start, end });
			}
			return;
		}
		range& last = r.back();
		if (last.last != UINT64_MAX && start > last.last + 1)
		{
			r.push_back({ start, end });
			return;
		}
		if (start >= last.first && (last.last == UINT64_MAX || start <= last.last + 1))
		{
			last.last = std::max(last.last, end);
			return;
		}

		auto iter = std::lower_bound(r.begin(), r.end(), start, [](const range& s, uint64_t value)
		{
			return s.last != UINT64_MAX && s.last + 1 < value;
		});
		if (iter == r.end())
		{
			r.push_back({ start, end });
			return;
		}
		if (end != UINT64_MAX && end + 1 < iter->first)
		{
			r.insert(iter, { start, end });
			return;
		}

		size_t first = (size_t)(iter - r.begin());
		r[first].first = std::min(r[first].first, start);
		r[first].last = std::max(r[first].last, end);

		size_t current = first + 1;
		while (current < r.size() && (r[first].last == UINT64_MAX || r[first].last + 1 >= r[current].first))
		{
			r[first].last = std::max(r[first].last, r[current].last);
			current++;
		}
		r.erase(r.begin() + first + 1, r.begin() + current);
	}

	inline range fetch_os(uint64_t offset, uint64_t size, bool is_mapped) { if (size == 0) return range::invalid(); return fetch(offset, offset + size - 1, is_mapped); }
	inline range fetch(range v, bool is_mapped) { return fetch(v.first, v.last, is_mapped); }

	/// Return the smallest exposed area inside the given region, or an invalid range if none.
	/// is_mapped means is true if area is currently memory mapped, false if not. If not,
	/// we remove the returned range.
	range fetch(uint64_t start, uint64_t end, bool is_mapped)
	{
		assert(start <= end);
		normalize();
		range retval = range::invalid();
		if (r.empty() || end < r.front().first || start > r.back().last) return retval;
		if (r.size() == 1)
		{
			range& s = r.front();
			if (s.last < start || s.first > end) return retval;

			retval = { std::max(s.first, start), std::min(s.last, end) };
			if (!is_mapped)
			{
				if (s.first >= start && s.last <= end) // consumed entirely
				{
					r.clear();
				}
				else if (s.first < start && s.last > end) // split in two
				{
					const uint64_t old_last = s.last;
					s.last = start - 1;
					r.push_back({ end + 1, old_last });
				}
				else if (s.first < start) // remove from end
				{
					s.last = start - 1;
				}
				else if (s.last > end) // remove from start
				{
					s.first = end + 1;
				}
			}
#ifdef FULLDEBUG
			assert(r.empty() || r.front().first <= r.back().last);
			assert(r.empty() || span().valid());
			self_test();
#endif
			return retval;
		}

		for (size_t i = 0; i < r.size(); )
		{
			auto& s = r[i];
			if (s.first > end) break;
			else if (s.last >= start && s.first <= end)
			{
				const uint64_t first = std::max(s.first, start);
				const uint64_t last = std::min(s.last, end);
				if (!retval.valid())
				{
					retval = { first, last };
				}
				else
				{
					retval.first = std::min(retval.first, first);
					retval.last = std::max(retval.last, last);
				}
				if (is_mapped)
				{
					// change nothing
				}
				else if (s.first >= start && s.last <= end) // consumed entirely
				{
					r.erase(r.begin() + i);
					continue;
				}
				else if (s.first < start && s.last > end) // split in two
				{
					const uint64_t old_last = s.last;
					s.last = start - 1;
					r.insert(r.begin() + i + 1, { end + 1, old_last });
					break;
				}
				else if (s.first < start) // remove from end
				{
					s.last = start - 1;
				}
				else if (s.last > end) // remove from start
				{
					s.first = end + 1;
					break;
				}
			}
			i++;
		}
		if (!retval.valid()) return retval;
#ifdef FULLDEBUG
		assert(r.empty() || r.front().first <= r.back().last);
		assert(r.empty() || span().valid());
		self_test();
#endif
		return retval;
	}

	void self_test() const
	{
		normalize();
		uint64_t prev = 0;
		bool first = true;
		for (auto& s : r)
		{
			assert(s.first <= s.last);
			assert(first || s.first > prev);
			first = false;
			prev = s.last;
			(void)prev; // silence compiler warning for release builds
			(void)first; // ditto
		}
	}

	inline const std::vector<range>& list() const { normalize(); return r; }
	inline size_t bytes() const { size_t v = 0; for (auto& s : r) { v += 1 + s.last - s.first; } return v; }
	inline void clear() { r.clear(); reversed = false; }
	inline size_t size() const { return r.size(); }

private:
	void normalize() const
	{
		if (!reversed) return;
		std::reverse(r.begin(), r.end());
		reversed = false;
	}

	mutable std::vector<range> r;
	mutable bool reversed = false;
};
