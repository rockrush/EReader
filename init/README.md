# Linux init (C)

A minimal Linux init (PID 1) written in C providing:

- Pseudo filesystems: `proc`, `sysfs`, `devtmpfs`, `devpts`, `tmpfs` (`/run`, `/tmp`, `/dev/shm`), cgroup2.
- Root filesystem mount from kernel cmdline (`root=`, supports `UUID=`, `LABEL=`, `PARTUUID=`) with `pivot_root`.
- Wireless networking: brings up `wpa_supplicant` + a DHCP client as managed services.
- Multi-process (service) management with restart-on-failure and exponential backoff.

## Build

```sh
make
```

## Usage

Install as `/sbin/init` (or pass `init=/sbin/init` to the kernel). Configure WiFi via `/etc/init.conf`:

```
SSID=MyNetwork
PSK=my-secret-passphrase
```

Or provide `/etc/wpa_supplicant.conf` directly. Override via environment: `WIFI_IFACE`, `WPA_CONF`, `DHCP_CLIENT`.

## Notes

- Designed to run as PID 1; running as a normal user will fail on privileged operations but still compiles/links.
- Services that exit are reaped and restarted according to their policy.
