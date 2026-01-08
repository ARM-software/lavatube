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
#include "util.h"

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
	return syscall(__NR_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags);
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(const int ruleset_fd,
					 const __u32 flags)
{
	return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}
#endif

void sandbox_level_one()
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) ABORT("Failed to restrict root privileges");
}

void sandbox_level_two()
{
	int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0)
	{
		if (errno == ENOSYS) ABORT("Landlock sandbox is not supported by the current kernel");
		else if (errno == EOPNOTSUPP) ABORT("Landlock sandbox is currently disabled");
		else ABORT("Landlock sandboxing not supported by your system: %s", strerror(errno));
	}
	struct landlock_ruleset_attr ruleset_attr = {};
	// Prohibit things we never need to do:
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_CHAR;
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
	ruleset_attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#endif
	int ruleset_fd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) ABORT("Failed to create landlock ruleset: %s", strerror(errno));
	if (landlock_restrict_self(ruleset_fd, 0) != 0) ABORT("Failed to enforce landlock ruleset: %s", strerror(errno));
	close(ruleset_fd);
}

static void sandbox_except_path(int ruleset_fd, const char* path, int access_flags)
{
	struct landlock_path_beneath_attr path_beneath = {};
	path_beneath.allowed_access = access_flags;
	path_beneath.parent_fd = open(path, O_PATH | O_CLOEXEC);
	if (path_beneath.parent_fd < 0) ABORT("Failed to add exception for an app path from sandbox restrictions: %s", strerror(errno));
	if (landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0) != 0) ABORT("Failed to add path exception to landlock sandbox ruleset: %s", strerror(errno));
	close(path_beneath.parent_fd);
}

void sandbox_level_three()
{
	// Prohibit things we don't need during replay
	struct landlock_ruleset_attr ruleset_attr = {};
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
	int ruleset_fd = landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) ABORT("Failed to create landlock ruleset: %s", strerror(errno));

	// Set up exception path from current working directory
	char path[255];
	memset(path, 0, sizeof(path));
	const char* exception_path = getcwd(path, sizeof(path));
	if (!exception_path) ABORT("Failed to get current working directory: %s\n", strerror(errno));
	sandbox_except_path(ruleset_fd, exception_path, LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR);
	sandbox_except_path(ruleset_fd, "/dev", LANDLOCK_ACCESS_FS_WRITE_FILE);

	if (landlock_restrict_self(ruleset_fd, 0) != 0) ABORT("Failed to enforce landlock ruleset: %s", strerror(errno));

	close(ruleset_fd);
}
