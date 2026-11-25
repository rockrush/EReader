#define _GNU_SOURCE
#include "tty1.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int read_config_value(const char *key, char *out, size_t outsz) {
	FILE *f = fopen("/etc/init.conf", "r");
	if (!f) return -1;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *k = line;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = '\0';
		while (*k == ' ' || *k == '\t') k++;
		char *ke = k + strlen(k) - 1;
		while (ke > k && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
		while (*v == ' ' || *v == '\t') v++;
		char *ve = v + strlen(v) - 1;
		while (ve > v && (*ve == ' ' || *ve == '\t')) *ve-- = '\0';
		if (strcmp(k, key) == 0) {
			snprintf(out, outsz, "%s", v);
			fclose(f);
			return 0;
		}
	}
	fclose(f);
	return -1;
}

static void get_ips(char *out, size_t outsz) {
	out[0] = '\0';
	struct ifaddrs *ifa, *p;
	if (getifaddrs(&ifa) != 0) { snprintf(out, outsz, "unknown"); return; }
	size_t pos = 0;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr) continue;
		if (p->ifa_addr->sa_family != AF_INET) continue;
		if (strcmp(p->ifa_name, "lo") == 0) continue;
		char ip[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
		inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
		int n = snprintf(out + pos, outsz - pos, "%s=%s ", p->ifa_name, ip);
		if (n < 0 || (size_t)n >= outsz - pos) break;
		pos += (size_t)n;
	}
	freeifaddrs(ifa);
	if (pos == 0) snprintf(out, outsz, "none");
}

void tty1_service(void) {
	int fd = open("/dev/tty1", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		/* cannot open tty1; exit */
		_exit(1);
	}

	struct termios orig, raw;
	if (tcgetattr(fd, &orig) == 0) {
		raw = orig;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_iflag &= ~(IXON | IXOFF | ICRNL);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &raw);
	}

	int first = 1;
	for (;;) {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		char timebuf[64];
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

		char ips[512];
		get_ips(ips, sizeof(ips));

		const char *prefix = first ? "\033[2J\033[H\033[?25l" : "\033[H\033[?25l";
		char screen[2048];
		int n = snprintf(screen, sizeof(screen), "%sTime:   %s\nIPs:	%s\n\n"
			"Press 'T' to open a login shell, 'R' to reboot.\n\033[J\033[?25h",
			prefix, timebuf, ips);
		if (n > 0) {
			size_t _w = write(fd, screen, (size_t)n); (void)_w;
		}
		first = 0;

		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		int ret = poll(&pfd, 1, 2000);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			char c;
			ssize_t r = read(fd, &c, 1);
			if (r != 1)
				continue;
			if (c == 'r' || c == 'R') {
				tcsetattr(fd, TCSANOW, &orig);
				ssize_t _w = write(fd, "\r\nRebooting...\r\n", 15); (void)_w;
				sync();
				reboot(RB_AUTOBOOT);
				_exit(0);
			} else if (c == 't' || c == 'T') {
				/* restore terminal settings */
				tcsetattr(fd, TCSANOW, &orig);
				close(fd);

				/* reopen tty1 and make it controlling terminal */
				int tty = open("/dev/tty1", O_RDWR | O_NOCTTY);
				if (tty >= 0) {
					ioctl(tty, TIOCSCTTY, 0);
					tcsetpgrp(tty, getpgrp());
					dup2(tty, STDIN_FILENO);
					dup2(tty, STDOUT_FILENO);
					dup2(tty, STDERR_FILENO);
					if (tty > 2) close(tty);
				}

				/* enter default path from init.conf if configured */
				char defpath[512] = "";
				read_config_value("DEFAULT_PATH", defpath, sizeof(defpath));
				if (defpath[0] == '\0') {
					defpath[0] = '~';
					defpath[1] = '\0';
				}

				if (chdir(defpath) != 0) {
					/* ignore */
				} else {
					setenv("PWD", defpath, 1);
				}

				const char *shell = NULL;
				const char *shells[] = {"/bin/bash", "/bin/sh", "/usr/bin/bash", "/usr/bin/sh", NULL};
				for (int i = 0; shells[i]; i++) {
					if (access(shells[i], X_OK) == 0) { shell = shells[i]; break; }
				}
				if (shell) {
					execl(shell, shell, "-l", (char *)NULL);
				}
				/* if exec fails */
				_exit(127);
			}
			// TODO: else if (c == 'e' || c == 'E')
			// 	/bin/EReader
		}
	}
}
