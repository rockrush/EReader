#define _GNU_SOURCE
#include "wifi.h"
#include "procman.h"
#include "log.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *find_bin(const char *name) {
	static char path[512];
	const char *dirs[] = {"/sbin", "/usr/sbin", "/bin", "/usr/bin", "/usr/local/sbin", "/usr/local/bin", NULL};
	for (int i = 0; dirs[i]; i++) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
		if (access(path, X_OK) == 0) return path;
	}
	/* fallback: maybe in PATH */
	return name;
}

static int file_exists(const char *p) { return access(p, R_OK) == 0; }

static const char *wifi_config_path(void) {
	return "/etc/init.conf";
}

static int config_value(const char *key, char *out, size_t outsz) {
	const char *path = wifi_config_path();
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

static int wifi_disabled(void) {
	/* Environment override */
	const char *env_enabled = getenv("WIFI_ENABLED");
	if (env_enabled) {
		return !is_truthy(env_enabled);
	}

	char val[128];
	if (config_value("WIFI_ENABLED", val, sizeof(val)) == 0) {
		return !is_truthy(val);
	}
	/* Default: enabled */
	return 0;
}

static void generate_wpa_conf(const char *wpa_conf) {
	/* read /etc/init/wifi.conf */
	FILE *f = fopen("/etc/init.conf", "r");
	if (!f) {
		LOG_WARN("no wifi config found (/etc/init/wifi.conf)");
		return;
	}
	char ssid[256] = {0}, psk[256] = {0}, key_mgmt[64] = "WPA-PSK";
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = line;
		char *val = eq + 1;
		/* trim newline */
		val[strcspn(val, "\r\n")] = '\0';
		/* trim spaces */
		while (*key == ' ' || *key == '\t') key++;
		char *ke = key + strlen(key) - 1;
		while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
		while (*val == ' ' || *val == '\t') val++;
		if (strcmp(key, "SSID") == 0) snprintf(ssid, sizeof(ssid), "%s", val);
		else if (strcmp(key, "PSK") == 0) snprintf(psk, sizeof(psk), "%s", val);
		else if (strcmp(key, "KEY_MGMT") == 0) snprintf(key_mgmt, sizeof(key_mgmt), "%s", val);
	}
	fclose(f);

	if (ssid[0] == '\0') {
		LOG_WARN("SSID not specified in wifi config");
		return;
	}

	mkdir("/etc", 0755);
	/* ensure parent dir */
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", wpa_conf);
	/* mkdir parent */
	char *slash = strrchr(dir, '/');
	if (slash) { *slash = '\0'; mkdir(dir, 0755); }

	FILE *out = fopen(wpa_conf, "w");
	if (!out) {
		LOG_ERR("cannot write %s: %s", wpa_conf, strerror(errno));
		return;
	}
	fprintf(out, "ctrl_interface=/run/wpa_supplicant\n");
	fprintf(out, "update_config=1\n\n");
	fprintf(out, "network={\n");
	fprintf(out, "	ssid=\"%s\"\n", ssid);
	if (psk[0]) {
		/* if psk looks like hex 64 chars, use as-is; else quoted */
		if (strlen(psk) == 64) {
			fprintf(out, "	psk=%s\n", psk);
		} else {
			fprintf(out, "	psk=\"%s\"\n", psk);
		}
	}
	fprintf(out, "	key_mgmt=%s\n", key_mgmt);
	fprintf(out, "}\n");
	fclose(out);
	chmod(wpa_conf, 0600);
	LOG_INFO("generated %s for SSID %s", wpa_conf, ssid);
}

void wifi_init(void) {
	if (wifi_disabled()) {
		LOG_INFO("wifi disabled by configuration");
		return;
	}
	const char *iface = getenv("WIFI_IFACE");
	if (!iface || !*iface) iface = "wlan0";
	const char *wpa_conf = getenv("WPA_CONF");
	if (!wpa_conf || !*wpa_conf) wpa_conf = "/etc/wpa_supplicant.conf";
	const char *dhcp = getenv("DHCP_CLIENT");
	/* auto pick */
	char dhcp_bin[128] = {0};
	if (dhcp && *dhcp) {
		snprintf(dhcp_bin, sizeof(dhcp_bin), "%s", dhcp);
	} else {
		/* prefer dhcpcd, then udhcpc, then dhclient */
		if (access(find_bin("dhcpcd"), X_OK) == 0) snprintf(dhcp_bin, sizeof(dhcp_bin), "dhcpcd");
		else if (access(find_bin("udhcpc"), X_OK) == 0) snprintf(dhcp_bin, sizeof(dhcp_bin), "udhcpc");
		else snprintf(dhcp_bin, sizeof(dhcp_bin), "dhclient");
	}

	/* ensure runtime dirs */
	mkdir("/run/wpa_supplicant", 0770);
	/* generate wpa conf if missing */
	if (!file_exists(wpa_conf)) {
		generate_wpa_conf(wpa_conf);
	}

	/* register wpa_supplicant */
	char *wpa_argv[] = {
		(char*)find_bin("wpa_supplicant"),
		"-i", (char*)iface,
		"-c", (char*)wpa_conf,
		NULL
	};
	/* argc count */
	int argc = 0;
	while (wpa_argv[argc]) argc++;
	/* need mutable copies? service_register strdup's */
	service_register("wpa_supplicant", wpa_argv, argc, RESTART_ON_FAILURE, 10, 0);

	/* register dhcp client */
	char dhcp_iface_arg[64];
	snprintf(dhcp_iface_arg, sizeof(dhcp_iface_arg), "%s", iface);
	char *dhcp_argv[8];
	int di = 0;
	dhcp_argv[di++] = (char*)find_bin(dhcp_bin);
	if (strcmp(dhcp_bin, "dhcpcd") == 0) {
		dhcp_argv[di++] = "-B"; /* background? no, foreground-ish; -B no background? Actually dhcpcd -B no daemon */
	}
	if (strcmp(dhcp_bin, "udhcpc") == 0) {
		dhcp_argv[di++] = "-i";
		dhcp_argv[di++] = dhcp_iface_arg;
		dhcp_argv[di++] = "-f"; /* foreground */
	} else if (strcmp(dhcp_bin, "dhclient") == 0) {
		dhcp_argv[di++] = "-d"; /* foreground */
		dhcp_argv[di++] = dhcp_iface_arg;
	} else {
		/* dhcpcd takes iface as positional */
		dhcp_argv[di++] = dhcp_iface_arg;
	}
	dhcp_argv[di] = NULL;
	int dargc = di;
	service_register("dhcp", dhcp_argv, dargc, RESTART_ON_FAILURE, 10, 0);

	LOG_INFO("wifi configured: iface=%s wpa_conf=%s dhcp=%s", iface, wpa_conf, dhcp_bin);
}