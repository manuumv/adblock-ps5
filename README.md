# ps5-adblock

A DNS-based ad and telemetry blocker that runs on a jailbroken PS5. It is a payload: the console's DNS points at itself (`127.0.0.1`) and ps5-adblock decides which names to answer locally and which to forward to a real resolver.

> **Status: 0.1, early.** This release is the DNS core: local override rules and reliable forwarding. Large blocklists, autostart, a web dashboard and an installer are on the way.

## What DNS blocking can and cannot do

- **Can** block domains that only serve ads, tracking or telemetry, and firmware update hosts.
- **Cannot** block ads served from the same domain as the content they sit in, such as ads inside YouTube videos or promotions on the PS5 home screen that come from the same Sony servers as real content.

## Requirements

- A jailbroken PS5 running etaHEN or onionHEN, with an ELF loader on port 9021.
- To build: Docker, or the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) with LLVM 18.

## Build

```sh
tools/ps5-build.sh            # PS5 payload in Docker -> build/ps5/ps5-adblock.elf
make ps5                      # same, with PS5_PAYLOAD_SDK installed locally
make host && make test        # development build and tests on macOS or Linux
```

## Run on the console

1. Send the payload to the loader:

   ```sh
   make deploy PS5_HOST=<ps5-ip>
   # or: nc -w 1 <ps5-ip> 9021 < build/ps5/ps5-adblock.elf
   ```

2. On the PS5: **Settings › Network › Settings › Set Up Internet Connection**, select your connection, **Advanced Settings › DNS Settings › Manual**, primary DNS `127.0.0.1`.

The payload creates `/data/ps5-adblock/config.ini` with defaults on first start.

> **If the console loses its network:** ps5-adblock is not running (for example after a reboot, before the jailbreak). Send the payload again, or set **DNS Settings** back to **Automatic**. In 0.1 the payload does not start by itself; autostart comes in a later release.

## Run on a computer

```sh
make host
ADBLOCK_DATA=./data ./build/host/ps5-adblock      # listens on 127.0.0.1:5300 and [::1]:5300
dig @127.0.0.1 -p 5300 example.com
```

`ADBLOCK_PORT` changes the port. Set `debug=1` in `./data/config.ini` to see every query on stderr.

## Configuration

`/data/ps5-adblock/config.ini` (see [`config.ini.example`](config.ini.example)). Lines starting with `#` or `;` are comments. An invalid line is skipped with a warning that names its line number; the rest of the file still applies.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[general]` | `debug` | `0` | `1` mirrors every query to the console log |
| | `bind` | `127.0.0.1` | IPv4 address to listen on; `0.0.0.0` serves the whole LAN. Only private and link-local clients are ever answered |
| | `bind6` | `::1` | IPv6 address to listen on, or `off` |
| `[upstream]` | `server` | `1.1.1.1`, `9.9.9.9` | Resolvers tried in order, up to 8. Listing any replaces the defaults |
| | `timeout_ms` | `1500` | Total time budget for one query (100–30000). Each upstream gets an equal share, so one dead server cannot use it all |
| `[overrides]` | `mask=IPv4` | | Answer names matching `mask` with this address (TTL 60). Other query types for that name get an empty answer. Up to 256 |
| `[exceptions]` | `mask` | | Names that skip overrides and always go upstream. Up to 256 |

Masks are case-insensitive and accept `*`, `?`, `[abc]`, `[a-z]` and `[!0-9]`. `*.example.com` matches subdomains of `example.com` but not `example.com` itself.

```ini
[overrides]
*.doubleclick.net=0.0.0.0
nas.home=192.168.1.10

[exceptions]
*.stun.playstation.net
```

## Credits

- [nanoDNS](https://github.com/drakmor/nanoDNS) by Drakmor: the DNS proxy and console integration this project grew from.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom.

## License

GPLv3, see [LICENSE](LICENSE). Not affiliated with Sony.
