// Stripped down mutex using std::mutex with thread annotations

#pragma once

#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   // no-op
#endif
#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(capability(x))
#define SCOPED_CAPABILITY THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
#define ACQUIRE(...) THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))
#define RELEASE(...) THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))
#define NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)
#define REQUIRES(...) THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))
#define REQUIRES_SHARED(...) THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

#include <mutex>

namespace lava
{

class CAPABILITY("mutex") mutex
{
private:
	std::mutex mMutex;
public:
	inline void lock() ACQUIRE() { mMutex.lock(); }
	inline void unlock() RELEASE() { mMutex.unlock(); }
};

class SCOPED_CAPABILITY lock_guard
{
private:
	mutex* mMutex;
	bool locked;

public:
	lock_guard(mutex &mu) ACQUIRE(mu) : mMutex(&mu), locked(true) { mMutex->lock(); }
	void unlock() RELEASE() { locked = false; mMutex->unlock(); }
	~lock_guard() RELEASE() { if (locked) mMutex->unlock(); }
	lock_guard(const lock_guard&) = delete;
};

};
