#define _GNU_SOURCE
#include "log.h"

#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

static int console_fd = -1;
static int log_fd = -1;

void log_init(void) {
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (console_fd < 0) console_fd = STDERR_FILENO;

	mkdir("/var", 0755);
	mkdir("/var/log", 0755);
	log_fd = open("/var/log/init.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (log_fd < 0) log_fd = -1;
}

void log_msg(const char *level, const char *fmt, ...) {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + 0, sizeof(buf), fmt, ap);
	va_end(ap);

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char line[4608];
	int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
					 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
					 tm.tm_hour, tm.tm_min, tm.tm_sec, level, buf);
	if (n < 0) return;

	ssize_t _w;
	if (console_fd >= 0) { _w = write(console_fd, line, (size_t)n); (void)_w; }
	else { _w = write(STDERR_FILENO, line, (size_t)n); (void)_w; }
	if (log_fd >= 0) { _w = write(log_fd, line, (size_t)n); (void)_w; }
}