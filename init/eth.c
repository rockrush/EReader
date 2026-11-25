#define _GNU_SOURCE
#include "eth.h"
#include "log.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/route.h>
#include <strings.h>

static const char *config_path(void) { return "/etc/init.conf"; }

static int config_value(const char *key, char *out, size_t outsz) {
	FILE *f = fopen(config_path(), "r");
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
		   strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0;
}

static int parse_prefix(const char *netmask) {
	/* if netmask contains '.', treat as dotted mask; else parse as prefix */
	if (strchr(netmask, '.')) {
		struct in_addr m;
		if (inet_pton(AF_INET, netmask, &m) != 1) return -1;
		uint32_t mask = ntohl(m.s_addr);
		int prefix = 0;
		while (mask & 0x80000000) { prefix++; mask <<= 1; }
		return prefix;
	}
	char *end;
	long p = strtol(netmask, &end, 10);
	if (*end != '\0' || p < 0 || p > 32) return -1;
	return (int)p;
}

static uint32_t prefix_to_mask(int prefix) {
	if (prefix == 0) return 0;
	return htonl(~((1U << (32 - prefix)) - 1));
}

void eth_init(void) {
	char enabled[128];
	if (config_value("ETH_ENABLED", enabled, sizeof(enabled)) == 0 && !is_truthy(enabled)) {
		LOG_INFO("ethernet disabled by configuration");
		return;
	}

	char iface[64] = "eth0";
	char ip[64] = "", netmask[64] = "", gateway[64] = "", dns[256] = "";
	config_value("ETH_IFACE", iface, sizeof(iface));
	config_value("ETH_IP", ip, sizeof(ip));
	config_value("ETH_NETMASK", netmask, sizeof(netmask));
	config_value("ETH_PREFIX", netmask, sizeof(netmask)); /* alias */
	config_value("ETH_GATEWAY", gateway, sizeof(gateway));
	config_value("ETH_DNS", dns, sizeof(dns));

	if (ip[0] == '\0') {
		LOG_INFO("no ETH_IP configured; skipping static IP setup");
		return;
	}
	if (netmask[0] == '\0') snprintf(netmask, sizeof(netmask), "24");

	int prefix = parse_prefix(netmask);
	if (prefix < 0) {
		LOG_ERR("invalid ETH_NETMASK/PREFIX: %s", netmask);
		return;
	}

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		LOG_ERR("socket: %s", strerror(errno));
		return;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);

	/* set IP address */
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1) {
		LOG_ERR("invalid ETH_IP: %s", ip);
		close(sock);
		return;
	}
	if (ioctl(sock, SIOCSIFADDR, &ifr) != 0) {
		LOG_ERR("SIOCSIFADDR %s: %s", iface, strerror(errno));
		close(sock);
		return;
	}

	/* set netmask */
	memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
	struct sockaddr_in *nm = (struct sockaddr_in *)&ifr.ifr_netmask;
	nm->sin_family = AF_INET;
	nm->sin_addr.s_addr = prefix_to_mask(prefix);
	if (ioctl(sock, SIOCSIFNETMASK, &ifr) != 0) {
		LOG_WARN("SIOCSIFNETMASK %s: %s", iface, strerror(errno));
	}

	/* bring interface up */
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
			LOG_WARN("SIOCSIFFLAGS %s: %s", iface, strerror(errno));
		}
	} else {
		LOG_WARN("SIOCGIFFLAGS %s: %s", iface, strerror(errno));
	}

	/* default gateway */
	if (gateway[0]) {
		struct rtentry rt;
		memset(&rt, 0, sizeof(rt));
		struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
		struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
		struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;
		dst->sin_family = AF_INET;
		gw->sin_family = AF_INET;
		mask->sin_family = AF_INET;
		if (inet_pton(AF_INET, gateway, &gw->sin_addr) == 1) {
			rt.rt_flags = RTF_UP | RTF_GATEWAY;
			rt.rt_dev = iface;
			if (ioctl(sock, SIOCADDRT, &rt) != 0) {
				/* maybe already exists; try replace not supported via ioctl */
				LOG_WARN("SIOCADDRT default gw: %s", strerror(errno));
			}
		} else {
			LOG_ERR("invalid ETH_GATEWAY: %s", gateway);
		}
	}

	close(sock);

	/* DNS */
	if (dns[0]) {
		FILE *rf = fopen("/etc/resolv.conf", "w");
		if (rf) {
			/* support comma or space separated */
			char buf[256];
			snprintf(buf, sizeof(buf), "%s", dns);
			char *save = NULL;
			char *tok = strtok_r(buf, " ,", &save);
			while (tok) {
				fprintf(rf, "nameserver %s\n", tok);
				tok = strtok_r(NULL, " ,", &save);
			}
			fclose(rf);
		} else {
			LOG_WARN("cannot write /etc/resolv.conf: %s", strerror(errno));
		}
	}

	LOG_INFO("configured %s with static IP %s/%d", iface, ip, prefix);
}