#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef __SANE_USERSPACE_TYPES__
#define __SANE_USERSPACE_TYPES__
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <linux/socket.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdbool.h>

#include "sandbox.h"

#ifndef landlock_create_ruleset
static inline int
landlock_create_ruleset(const struct landlock_ruleset_attr *const attr,
			const size_t size, const __u32 flags)
{
	return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(const int ruleset_fd,
				    const enum landlock_rule_type rule_type,
				    const void *const rule_attr,
				    const __u32 flags)
{
	return syscall(__NR_landlock_add_rule, ruleset_fd, rule_type, rule_attr,
		       flags);
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(const int ruleset_fd,
					 const __u32 flags)
{
	return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}
#endif

const char* sandbox_tool_init()
{
	int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0)
	{
		if (errno == ENOSYS) return "Landlock sandbox is not supported by the current kernel";
		else if (errno == EOPNOTSUPP) return "Landlock sandbox is currently disabled";
		else return "Landlock sandboxing not supported by your system for unknown reason";
	}
	struct landlock_ruleset_attr ruleset_attr;
	// Prohibit things we never need to do:
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
		LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_CHAR;
	int ruleset_fd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) return "Failed to create landlock sandbox ruleset";
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) { close(ruleset_fd); return "Failed to restrict system privileges"; }
	if (landlock_restrict_self(ruleset_fd, 0)) { close(ruleset_fd); return "Failed to enforce landlock sandbox ruleset"; }
	close(ruleset_fd);
	return nullptr;
}

const char* sandbox_replay_start()
{
	// error would have been spammed during init, do not need to repeat it; if we got here, we decided to ignore it
	if (landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION) < 0) return nullptr;
	struct landlock_ruleset_attr ruleset_attr;
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
		LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE;
	// LANDLOCK_ACCESS_FS_WRITE_FILE needed for validation layers for some reason, somewhat random errors will occur without it
	int ruleset_fd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) return "Failed to create landlock sandbox ruleset";
	if (landlock_restrict_self(ruleset_fd, 0)) { close(ruleset_fd); return "Failed to enforce landlock sandbox ruleset"; }
	close(ruleset_fd);
	return nullptr;
}
