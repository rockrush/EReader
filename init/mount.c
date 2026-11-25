#define _GNU_SOURCE
#include "mount.h"
#include "log.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int mkdir_p(const char *path, mode_t mode) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if (len == 0) return -1;
	if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
				LOG_WARN("mkdir %s: %s", tmp, strerror(errno));
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
		LOG_WARN("mkdir %s: %s", tmp, strerror(errno));
		return -1;
	}
	return 0;
}

static int try_mount(const char *src, const char *dst, const char *type,
					 unsigned long flags, const char *data) {
	if (mkdir_p(dst, 0755) != 0) return -1;
	if (mount(src, dst, type, flags, data) != 0) {
		if (errno == EBUSY) {
			LOG_DEBUG("already mounted %s on %s", src ? src : type, dst);
			return 0;
		}
		LOG_WARN("mount %s on %s (%s): %s", src, dst, type ? type : "auto", strerror(errno));
		return -1;
	}
	LOG_INFO("mounted %s on %s", src ? src : type, dst);
	return 0;
}

int mount_pseudo_filesystems(void) {
	int rc = 0;
	/* /proc */
	rc |= try_mount("proc", "/proc", "proc",
					MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	/* /sys */
	rc |= try_mount("sysfs", "/sys", "sysfs",
					MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	/* /dev: prefer devtmpfs; do not overlay with tmpfs to avoid hiding existing device nodes */
	if (try_mount("devtmpfs", "/dev", "devtmpfs",
				  MS_NOSUID | MS_NOEXEC, "mode=0755") != 0) {
		LOG_WARN("devtmpfs unavailable; keeping existing /dev contents");
	}
	/* Ensure essential device nodes exist even if devtmpfs is unavailable */
	struct { const char *path; mode_t mode; dev_t dev; } nodes[] = {
		{"/dev/null", 0666, makedev(1, 3)},
		{"/dev/zero", 0666, makedev(1, 5)},
		{"/dev/random", 0666, makedev(1, 8)},
		{"/dev/urandom", 0666, makedev(1, 9)},
		{"/dev/console", 0600, makedev(5, 1)},
		{"/dev/tty", 0666, makedev(5, 0)},
		{"/dev/tty1", 0620, makedev(4, 1)},
		{"/dev/tty2", 0620, makedev(4, 2)},
		{"/dev/tty3", 0620, makedev(4, 3)},
		{"/dev/tty4", 0620, makedev(4, 4)},
	};
	for (size_t i = 0; i < sizeof(nodes)/sizeof(nodes[0]); i++) {
		if (access(nodes[i].path, F_OK) != 0) {
			if (mknod(nodes[i].path, S_IFCHR | nodes[i].mode, nodes[i].dev) != 0) {
				LOG_WARN("mknod %s: %s", nodes[i].path, strerror(errno));
			} else {
				chmod(nodes[i].path, nodes[i].mode);
			}
		}
	}
	/* /dev/pts */
	mkdir_p("/dev/pts", 0755);
	try_mount("devpts", "/dev/pts", "devpts",
			  MS_NOSUID | MS_NOEXEC, "gid=5,mode=0620");
	/* /dev/shm */
	mkdir_p("/dev/shm", 01777);
	try_mount("tmpfs", "/dev/shm", "tmpfs",
			  MS_NOSUID | MS_NODEV, "mode=1777");
	/* /run */
	try_mount("tmpfs", "/run", "tmpfs",
			  MS_NOSUID | MS_NODEV, "mode=0755");
	/* /tmp */
	try_mount("tmpfs", "/tmp", "tmpfs",
			  MS_NOSUID | MS_NODEV, "mode=1777");

	/* cgroup v2 (optional) */
	mkdir_p("/sys/fs/cgroup", 0755);
	try_mount("cgroup2", "/sys/fs/cgroup", "cgroup2",
			  MS_NOSUID | MS_NODEV | MS_NOEXEC, "nsdelegate");

	return rc;
}

/* parse_cmdline removed; read_cmdline_param handles parsing */

static int read_cmdline_param(const char *name, char *out, size_t outsz) {
	FILE *f = fopen("/proc/cmdline", "r");
	if (!f) return -1;
	static char buf[4096];
	if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
	fclose(f);
	/* tokenize by spaces */
	char *save = NULL;
	char *tok = strtok_r(buf, " \t\n", &save);
	size_t nlen = strlen(name);
	while (tok) {
		if (strncmp(tok, name, nlen) == 0 && tok[nlen] == '=') {
			snprintf(out, outsz, "%s", tok + nlen + 1);
			return 0;
		}
		tok = strtok_r(NULL, " \t\n", &save);
	}
	return -1;
}

int mount_root_filesystem(void) {
	char rootdev[1024] = {0};
	if (read_cmdline_param("root", rootdev, sizeof(rootdev)) != 0) {
		LOG_INFO("no root= on cmdline; keeping current root");
		remount_root_rw();
		return 0;
	}

	/* Resolve UUID= / PARTUUID= / LABEL= */
	char resolved[1024];
	if (strncmp(rootdev, "UUID=", 5) == 0) {
		snprintf(resolved, sizeof(resolved), "/dev/disk/by-uuid/%.255s", rootdev + 5);
		if (access(resolved, F_OK) == 0) snprintf(rootdev, sizeof(rootdev), "%.511s", resolved);
	} else if (strncmp(rootdev, "PARTUUID=", 9) == 0) {
		snprintf(resolved, sizeof(resolved), "/dev/disk/by-partuuid/%.255s", rootdev + 9);
		if (access(resolved, F_OK) == 0) snprintf(rootdev, sizeof(rootdev), "%.511s", resolved);
	} else if (strncmp(rootdev, "LABEL=", 6) == 0) {
		snprintf(resolved, sizeof(resolved), "/dev/disk/by-label/%.255s", rootdev + 6);
		if (access(resolved, F_OK) == 0) snprintf(rootdev, sizeof(rootdev), "%.511s", resolved);
	}
	/* If root is pseudo/rootfs, skip */
	if (strcmp(rootdev, "rootfs") == 0 || strcmp(rootdev, "/dev/root") == 0) {
		/* /dev/root may need resolving; attempt anyway below */
	}

	/* Guess filesystem type */
	const char *fstypes[] = {"ext4", "btrfs", "xfs", "ext3", "ext2", NULL};

	mkdir_p("/newroot", 0755);
	int ok = 0;
	for (int i = 0; fstypes[i]; i++) {
		if (mount(rootdev, "/newroot", fstypes[i], 0, NULL) == 0) {
			ok = 1;
			LOG_INFO("mounted root %s as %s", rootdev, fstypes[i]);
			break;
		}
	}
	if (!ok) {
		/* try with type NULL? mount requires type; try ext4 last error logged */
		LOG_ERR("failed to mount root %s", rootdev);
		return -1;
	}

	mkdir_p("/newroot/oldroot", 0755);

	/* pivot_root */
	if (syscall(SYS_pivot_root, "/newroot", "/newroot/oldroot") != 0) {
		LOG_ERR("pivot_root: %s", strerror(errno));
		/* fallback: chroot */
		if (chroot("/newroot") != 0) {
			LOG_ERR("chroot: %s", strerror(errno));
			return -1;
		}
		if (chdir("/") != 0) LOG_WARN("chdir: %s", strerror(errno));
		return 0;
	}
	if (chdir("/") != 0) LOG_WARN("chdir: %s", strerror(errno));
	/* detach old root */
	if (umount2("/oldroot", MNT_DETACH) != 0) {
		LOG_WARN("umount oldroot: %s", strerror(errno));
	}
	rmdir("/oldroot");
	LOG_INFO("root filesystem switched");
	remount_root_rw();
	return 0;
}

int remount_root_rw(void) {
	if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) != 0) {
		/* try with explicit rw option */
		if (mount(NULL, "/", NULL, MS_REMOUNT, "rw") != 0) {
			LOG_WARN("remount / rw: %s", strerror(errno));
			return -1;
		}
	}
	LOG_INFO("remounted / as read-write");
	return 0;
}

void setup_basic_environment(void) {
	/* hostname */
	FILE *f = fopen("/etc/hostname", "r");
	if (f) {
		char hn[256];
		if (fgets(hn, sizeof(hn), f)) {
			hn[strcspn(hn, "\n")] = '\0';
			if (hn[0]) { if (sethostname(hn, strlen(hn)) != 0) LOG_WARN("sethostname: %s", strerror(errno)); }
		}
		fclose(f);
	} else {
		if (sethostname("rockwork", 8) != 0) LOG_WARN("sethostname: %s", strerror(errno));
	}

	/* minimal /etc/resolv.conf? leave to dhcp */

	/* PATH */
	setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin", 1);
	setenv("HOME", "/root", 1);
}