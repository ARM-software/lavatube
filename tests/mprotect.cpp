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
#include <poll.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <signal.h>
#include <atomic>

#include "util.h"

static std::atomic_bool done { false };

static inline uint64_t mygettime()
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

static unsigned char* shadow_memory = nullptr;

static void sighandler(int id, siginfo_t* info, void* data)
{
	assert(!done.load());
	if (id == SIGSEGV && info->si_addr != nullptr)
	{
		size_t size = getpagesize();
		uint64_t start = (uint64_t)info->si_addr & ~(size - 1);
		//printf("Trapped address %lu -> %lu\n", (unsigned long)info->si_addr, (unsigned long)start);
		memcpy(shadow_memory, (void*)start, size);
		mprotect((void*)start, size, PROT_READ | PROT_WRITE | PROT_EXEC);
	}
}

static void memory_protect(uint64_t address, uint64_t size)
{
	mprotect((void*)address, size, PROT_READ);
}

static void memory_register(uint64_t address, uint64_t size)
{
	struct sigaction sa = {};
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = sighandler;
	int r = sigaction(SIGSEGV, &sa, nullptr);
	if (r != 0) ABORT("Failed to setup signal handler: %s", strerror(errno));

	memory_protect(address, size);
}

static void memory_unregister(uint64_t address, uint64_t size)
{
	mprotect((void*)address, size, PROT_READ | PROT_WRITE | PROT_EXEC); // just in case the driver tries to reuse allocated memory or something
}

int main()
{
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
	done.store(true);
	mymem[100] = 111; // this should _not_ trigger signal handler
	mymem[1] = 1;

	// test again
	done.store(false);
	memory_protect((uint64_t)mymem, len);
	memory_unregister((uint64_t)mymem, len);
	done.store(true);
	mymem[100] = 113; // this should _not_ trigger signal handler
	mymem[1] = 13;

	free(mymem);

	return 0;
}
