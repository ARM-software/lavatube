#include <assert.h>
#include <thread>
#include <atomic>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "util.h"

static inline uint64_t mygettime()
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

static unsigned char* shadow_memory = nullptr;
static long uffd = 0; // file descriptor for reading trapped memory writes using userfaultfd method
static std::atomic_bool run_pageguard { true };

static void memory_protect(uint64_t address, uint64_t size)
{
	struct uffdio_writeprotect uffdio_wp = {};
	uffdio_wp.range.start = address;
	uffdio_wp.range.len = size;
	uffdio_wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
	if (ioctl(uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) ABORT("Failed to write protect memory: %s", strerror(errno));
}

static void memory_register(uint64_t address, uint64_t size)
{
	// Need to make sure all pages are paged in before we can protect them. Otherwise we'll have no way of knowing whether they are or not, and the protect
	// will fail if any are not.
	uint64_t pagesize = getpagesize();
	for (uint64_t incr = address; incr < address + size; incr += pagesize) ((char*)address)[0] = '\0';

	// Register write protection
	struct uffdio_register uffdio_register = {};
	uffdio_register.range.start = address;
	uffdio_register.range.len = size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) ABORT("Failed to register write-protection for our memory at %lu size=%lu, uffd=%ld: %s", (unsigned long)address, (unsigned long)size, uffd, strerror(errno));

	memory_protect(address, size);
}

static void memory_unregister(uint64_t address, uint64_t size)
{
	struct uffdio_range range = {};
	range.start = address;
	range.len = size;
	if (ioctl(uffd, UFFDIO_UNREGISTER, &range) == -1)
	{
		ABORT("Failed to unregister write-protection for our memory: %s", strerror(errno));
	}
}

static void userfaultfd_thread_func()
{
	printf("entering pageguard thread\n");
	struct pollfd fd;
	fd.fd = uffd;
	fd.events = POLLIN;
	struct uffd_msg msg;
	int r;
	while (run_pageguard.load(std::memory_order_relaxed))
	{
		r = poll(&fd, 1, 100);
		if (r == 0) continue; // non-blocking loop
		else if (r == -1) ABORT("Failed poll on userfaultfd: %s", strerror(errno));
		//printf("\nfault_handler_thread(): r=%d POLLIN=%d POLLERR=%d uffd=%ld\n", r, fd.revents & POLLIN, fd.revents & POLLERR, uffd);
		r = read(uffd, &msg, sizeof(msg));
		if (r <= 0) ABORT("Failed read on userfaultfd (r=%d): %s", r, strerror(errno));
		if (msg.event != UFFD_EVENT_PAGEFAULT) ABORT("Unhandled pageguard event: %d", (int)msg.event); // should never happen!
		//printf("PAGEFAULT event flags=%lx address=%lx\n", (unsigned long)msg.arg.pagefault.flags, (unsigned long)msg.arg.pagefault.address);
		if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) // hitting write protected page
		{
			size_t size = getpagesize();
			uint64_t start = (msg.arg.pagefault.address & ~(size - 1));
			memcpy(shadow_memory, (void*)start, size); // TBD wrong address for page+1 and above!!
			struct uffdio_writeprotect uffdio_wp = {};
			uffdio_wp.range.start = start;
			uffdio_wp.range.len = size;
			uffdio_wp.mode = 0;
			if (ioctl(uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) ABORT("Failed to unprotect memory: %s", strerror(errno));
		}
	}
	printf("leaving pageguard thread\n");
}

int main()
{
	std::thread userfaultfd_thread = std::thread(&userfaultfd_thread_func);

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	if (uffd == -1) ABORT("Failed to open userfaultfd: %s", strerror(errno));

	struct uffdio_api uffdio_api = {};
	uffdio_api.api = UFFD_API;
	uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) ABORT("Failed feature enable userfaultfd write-protect: %s", strerror(errno));
	assert(uffdio_api.ioctls & (1 << _UFFDIO_REGISTER));
	assert(uffdio_api.ioctls & (1 << _UFFDIO_UNREGISTER));
	//assert(uffdio_api.ioctls & (1 << _UFFDIO_WRITEPROTECT));

	int len = 4096 * 10;
	if (posix_memalign((void**)&shadow_memory, getpagesize(), len) != 0) ABORT("Failed aligned shadow memory allocation!");
	assert(shadow_memory);
	unsigned char* mymem = nullptr;
	if (posix_memalign((void**)&mymem, getpagesize(), len) != 0) ABORT("Failed aligned memory allocation!");
	assert(mymem);
	memset(mymem, 0, len);
	for (int i = 0; i < len; i++) shadow_memory[i] = 255;
	memory_register((uint64_t)mymem, len);

	assert(mymem[0] == 0);
	assert(mymem[1] == 0);
	assert(shadow_memory[0] == 255);
	mymem[0] = 1;
	mymem[0] = 1;
	mymem[0] = 1;
	mymem[0] = 1;
	mymem[0] = 1;
	mymem[0] = 1;
	mymem[4099] = 1;
	assert(mymem[0] == 1);
	assert(mymem[1] == 0);
	assert(mymem[4099] == 1);
	assert(shadow_memory[0] == 0); // should not have the change nor 255
	assert(shadow_memory[1] == 0);

	printf("Test 1 - small writes, quick reset\n");
	uint64_t t1 = mygettime();
	for (int i = 0; i < 10000; i++)
	{
		memory_protect((uint64_t)mymem, len);
		mymem[1] = 1;
		mymem[4097] = 1;
		mymem[4096*3+2] = 1;
	}
	uint64_t t2 = mygettime();
	printf("Time spent: %lu\n", (unsigned long)(t2 - t1));

	printf("Test 2 - more writes\n");
	t1 = mygettime();
	for (int i = 0; i < 10000; i++)
	{
		memory_protect((uint64_t)mymem, len);
		for (int j = 0; j < len; j ++) mymem[j] = 2;
	}
	t2 = mygettime();
	printf("Time spent: %lu\n", (unsigned long)(t2 - t1));

	// wrapping up
	memory_unregister((uint64_t)mymem, len);
	free(mymem);
	run_pageguard.store(false);
	if (userfaultfd_thread.joinable()) userfaultfd_thread.join();
	close(uffd);
	uffd = 0;
	userfaultfd_thread = std::thread();

	return 0;
}
