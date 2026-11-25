#define _GNU_SOURCE
#include "ssh.h"
#include "procman.h"
#include "log.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <strings.h>
#include <signal.h>

static int file_exists(const char *p) { return access(p, R_OK) == 0; }

static const char *ssh_config_path(void) {
	return "/etc/init.conf";
}

static int config_value(const char *key, char *out, size_t outsz) {
	const char *path = ssh_config_path();
	FILE *f = fopen(path, "r");
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

static int is_truthy(const char *v) {
	if (!v) return 0;
	return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
		   strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0 ||
		   strcasecmp(v, "enable") == 0 || strcasecmp(v, "enabled") == 0;
}

static int ssh_disabled(void) {
	const char *env_enabled = getenv("SSH_ENABLED");
	if (env_enabled) return !is_truthy(env_enabled);
	char val[128];
	if (config_value("SSH_ENABLED", val, sizeof(val)) == 0) return !is_truthy(val);
	/* Default: disabled unless explicitly enabled */
	return 1;
}

static const char *find_bin(const char *name) {
	static char path[512];
	const char *dirs[] = {"/usr/sbin", "/sbin", "/usr/bin", "/bin", "/usr/local/sbin", "/usr/local/bin", NULL};
	for (int i = 0; dirs[i]; i++) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
		if (access(path, X_OK) == 0) return path;
	}
	return name;
}

static void ensure_host_keys(void) {
	if (file_exists("/etc/ssh/ssh_host_rsa_key") || file_exists("/etc/ssh/ssh_host_ed25519_key")) return;
	const char *keygen = find_bin("ssh-keygen");
	if (access(keygen, X_OK) != 0) {
		LOG_WARN("ssh-keygen not found; skipping host key generation");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) { LOG_WARN("fork ssh-keygen: %s", strerror(errno)); return; }
	if (pid == 0) {
		/* restore signal mask */
		sigset_t empty; sigemptyset(&empty); sigprocmask(SIG_SETMASK, &empty, NULL);
		execl(keygen, keygen, "-A", (char *)NULL);
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		LOG_INFO("generated SSH host keys");
	} else {
		LOG_WARN("ssh-keygen -A failed");
	}
}

void ssh_init(void) {
	if (ssh_disabled()) {
		LOG_INFO("ssh disabled by configuration");
		return;
	}

	const char *sshd = find_bin("sshd");
	if (access(sshd, X_OK) != 0) {
		LOG_WARN("sshd not found; skipping SSH service");
		return;
	}

	mkdir("/var/empty", 0755);
	mkdir("/run", 0755);
	mkdir("/run/sshd", 0755);
	chmod("/run/sshd", 0755);
	ensure_host_keys();

	char *argv[] = {(char *)sshd, "-D", NULL};
	service_register("sshd", argv, 2, RESTART_ON_FAILURE, 10, 0);
	LOG_INFO("ssh enabled");
}