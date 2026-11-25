#define _GNU_SOURCE
#include "procman.h"
#include "log.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

static service_t services[MAX_SERVICES];
static int nsvc = 0;

void procman_init(void) {
	memset(services, 0, sizeof(services));
	nsvc = 0;
}

int service_register(const char *name, char **argv, int argc,
					 restart_policy_t policy, int max_restarts, int critical) {
	if (nsvc >= MAX_SERVICES) {
		LOG_ERR("too many services");
		return -1;
	}
	service_t *s = &services[nsvc++];
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->argc = argc;
	for (int i = 0; i < argc && i < MAX_ARGS; i++) {
		s->argv[i] = strdup(argv[i]);
	}
	s->argv[argc] = NULL;
	s->func = NULL;
	s->builtin = 0;
	s->policy = policy;
	s->max_restarts = max_restarts;
	s->critical = critical;
	s->enabled = 1;
	s->pid = -1;
	s->backoff_sec = 1;
	return 0;
}

int service_register_builtin(const char *name, service_func_t func,
							 restart_policy_t policy, int max_restarts, int critical) {
	if (nsvc >= MAX_SERVICES) {
		LOG_ERR("too many services");
		return -1;
	}
	service_t *s = &services[nsvc++];
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->func = func;
	s->builtin = 1;
	s->argc = 0;
	s->argv[0] = NULL;
	s->policy = policy;
	s->max_restarts = max_restarts;
	s->critical = critical;
	s->enabled = 1;
	s->pid = -1;
	s->backoff_sec = 1;
	return 0;
}

service_t *service_find(const char *name) {
	for (int i = 0; i < nsvc; i++) {
		if (strcmp(services[i].name, name) == 0) return &services[i];
	}
	return NULL;
}

service_t *service_list(int *count) {
	if (count) *count = nsvc;
	return services;
}

int service_count(void) { return nsvc; }

int service_start(service_t *s) {
	if (!s || !s->enabled) return -1;
	if (s->running) return 0;

	/* backoff */
	time_t now = time(NULL);
	if (s->restart_count > 0 && s->last_start > 0) {
		time_t wait = s->backoff_sec;
		if (now - s->last_start < wait) {
			return 0; /* too soon */
		}
	}

	pid_t pid = fork();
	if (pid < 0) {
		LOG_ERR("fork %s: %s", s->name, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* child */
		/* restore default signal mask */
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);
		/* new session */
		setsid();
		/* redirect stdio to /dev/console unless builtin handles tty itself */
		if (!s->builtin) {
			int cfd = open("/dev/console", O_RDWR);
			if (cfd >= 0) {
				dup2(cfd, STDIN_FILENO);
				dup2(cfd, STDOUT_FILENO);
				dup2(cfd, STDERR_FILENO);
				if (cfd > 2) close(cfd);
			}
			execvp(s->argv[0], s->argv);
			fprintf(stderr, "exec %s failed: %s\n", s->argv[0], strerror(errno));
			_exit(127);
		} else {
			/* builtin: run function */
			if (s->func) s->func();
			_exit(0);
		}
	}
	s->pid = pid;
	s->running = 1;
	s->last_start = now;
	LOG_INFO("started service %s (pid %d)", s->name, (int)pid);
	return 0;
}

void service_stop(service_t *s) {
	if (!s || !s->running) return;
	s->enabled = 0; /* prevent auto-restart */
	kill(s->pid, SIGTERM);
	LOG_INFO("stopped service %s", s->name);
}

void reap_children(void) {
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		/* find service */
		service_t *s = NULL;
		for (int i = 0; i < nsvc; i++) {
			if (services[i].pid == pid) { s = &services[i]; break; }
		}
		if (s) {
			s->running = 0;
			s->pid = -1;
			if (WIFEXITED(status)) {
				s->exit_status = WEXITSTATUS(status);
				s->normal_exit = (s->exit_status == 0);
				LOG_INFO("service %s exited status %d", s->name, s->exit_status);
			} else if (WIFSIGNALED(status)) {
				s->exit_status = 128 + WTERMSIG(status);
				s->normal_exit = 0;
				LOG_WARN("service %s killed by signal %d", s->name, WTERMSIG(status));
			} else {
				s->normal_exit = 0;
			}
			/* restart policy */
			int should_restart = 0;
			if (s->policy == RESTART_ALWAYS) should_restart = 1;
			else if (s->policy == RESTART_ON_FAILURE && !s->normal_exit) should_restart = 1;

			if (should_restart) {
				if (s->max_restarts >= 0 && s->restart_count >= s->max_restarts) {
					LOG_ERR("service %s exceeded max restarts (%d)", s->name, s->max_restarts);
					s->enabled = 0;
					if (s->critical) {
						LOG_ERR("critical service %s failed permanently; rebooting", s->name);
						sync();
						reboot(RB_AUTOBOOT);
					}
				} else {
					s->restart_count++;
					/* exponential backoff capped at 30s */
					s->backoff_sec = 1 << (s->restart_count > 5 ? 5 : s->restart_count);
					if (s->backoff_sec > 30) s->backoff_sec = 30;
					LOG_INFO("will restart %s (attempt %d) after %ds",
							 s->name, s->restart_count, s->backoff_sec);
				}
			} else {
				s->enabled = 0;
			}
		} else {
			/* unknown child reaped */
			LOG_DEBUG("reaped unknown child %d", (int)pid);
		}
	}
	if (pid < 0 && errno != ECHILD) {
		LOG_WARN("waitpid: %s", strerror(errno));
	}
}

void restart_failed_services(void) {
	for (int i = 0; i < nsvc; i++) {
		service_t *s = &services[i];
		if (!s->enabled || s->running) continue;
		/* only restart if it had been started before and died */
		if (s->restart_count > 0 || s->last_start > 0) {
			/* check backoff elapsed */
			time_t now = time(NULL);
			if (s->last_start > 0 && (now - s->last_start) < s->backoff_sec) continue;
			/* reset? keep count */
			service_start(s);
		}
	}
	/* also start never-started enabled services */
	for (int i = 0; i < nsvc; i++) {
		service_t *s = &services[i];
		if (s->enabled && !s->running && s->last_start == 0) {
			service_start(s);
		}
	}
}