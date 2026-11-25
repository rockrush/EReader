#define _GNU_SOURCE
#include "signal.h"
#include "log.h"

#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static int sfd = -1;

int signal_setup(void) {
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
		LOG_ERR("sigprocmask: %s", strerror(errno));
		return -1;
	}
	sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0) {
		LOG_ERR("signalfd: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int signal_fd(void) { return sfd; }