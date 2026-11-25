#define _GNU_SOURCE
#include "log.h"
#include "mount.h"
#include "procman.h"
#include "wifi.h"
#include "ssh.h"
#include "eth.h"
#include "tty1.h"
#include "signal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	int running = 1;
	struct pollfd pfd;
	pfd.fd = signal_fd();
	pfd.events = POLLIN;

	log_init();
	LOG_INFO("init starting (pid %d)", (int)getpid());

	if (signal_setup() != 0) {
		LOG_ERR("signal setup failed");
		return 1;
	}

	mount_pseudo_filesystems();
	mount_root_filesystem();
	remount_root_rw();
	setup_basic_environment();

	log_init();	/* re-open log after /var maybe available (already) */
	procman_init();

	service_register_builtin("tty1-info", tty1_service, RESTART_ALWAYS, -1, 0);	// foreground ui
	wifi_init();
	ssh_init();
	eth_init();

	restart_failed_services();	/* start all services initially */

	while (running) {
		int ret = poll(&pfd, 1, 1000);
		if (ret < 0) {
			if (errno == EINTR) continue;
			LOG_ERR("poll: %s", strerror(errno));
			break;
		}
		if (ret > 0 && (pfd.revents & POLLIN)) {
			struct signalfd_siginfo si;
			ssize_t r;
			while ((r = read(signal_fd(), &si, sizeof(si))) == (ssize_t)sizeof(si)) {
				(void)r;
				if (si.ssi_signo == SIGCHLD) {
					reap_children();
				} else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
					LOG_INFO("received signal %d, shutting down", (int)si.ssi_signo);
					running = 0;
				} else if (si.ssi_signo == SIGHUP) {
					LOG_INFO("SIGHUP: reloading not implemented");
				} else if (si.ssi_signo == SIGUSR1) {
					LOG_INFO("SIGUSR1");
				}
			}
		}
		if (running)
			restart_failed_services();	// restart failed / pending
	}

	/* shutdown */
	int cnt = 0;
	service_t *list = service_list(&cnt);
	for (int i = 0; i < cnt; i++) {
		service_stop(&list[i]);
	}
	/* wait a bit for children */
	for (int i = 0; i < 20; i++) {
		reap_children();
		/* check if any running */
		int any = 0;
		for (int j = 0; j < cnt; j++) if (list[j].running) any = 1;
		if (!any) break;
		usleep(100000);
	}
	/* force kill remaining */
	for (int i = 0; i < cnt; i++) {
		if (list[i].running) kill(list[i].pid, SIGKILL);
	}
	sync();
	LOG_INFO("init exiting");
	if (getpid() == 1)
		reboot(RB_POWER_OFF);
	return 0;
}
