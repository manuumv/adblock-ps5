# ps5-adblock

A DNS-based ad and telemetry blocker that runs on a jailbroken PS5. It is a payload: the console's DNS points at itself (`127.0.0.1`) and ps5-adblock decides which names to answer locally and which to forward to a real resolver.

> **Status: early.** DNS core with blocklists of up to millions of domains, an allowlist and live reloads. Autostart, a web dashboard, an installer and automatic list downloads are on the way.

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

## Blocking

ps5-adblock blocks every domain listed in the `.txt` files under `/data/ps5-adblock/lists/`, and every subdomain of them. Copy lists there over FTP; changes are picked up within 30 seconds, with no restart and without interrupting DNS.

Recommended for the PS5 browser (ads on pages and pop-up/pop-under ad tabs):

| List | Domains | URL |
|---|---|---|
| HaGeZi Pro | ~200k | `https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro-onlydomains.txt` |
| HaGeZi Pop-Up Ads | ~46k | `https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/popupads-onlydomains.txt` |

Three line formats are understood: hosts file entries, one domain per line, and Adblock-style domain rules. Lines starting with `#` or `!` are comments.

```text
0.0.0.0 ads.example.com
tracker.example.net
||popups.example.org^
```

Adblock rules that need a browser (element hiding `##`, `$third-party`, paths, regexes) are counted and skipped.

**Allowlist.** Domains in `/data/ps5-adblock/allowlist.txt` (same formats) are never blocked, nor are their subdomains. Use it when a site breaks.

**How blocked names are answered.** `block_mode=null` (default) answers `0.0.0.0` for IPv4 and `::` for IPv6; `block_mode=nxdomain` answers "no such domain".

**Presets.** `preset=update-block` in `[blocking]` blocks firmware updates the way nanoDNS does: it blocks `playstation.com`, `playstation.net` and `psndl.net`, so **PSN features (store, online play) stop working**; STUN and the network test stay reachable. `preset=ps-telemetry` is reserved and empty for now.

**What DNS blocking cannot do in the browser.** It stops ad-network ads and the scripts that open pop-up tabs. It cannot stop a tab that the site itself opens (the tab shows an error page instead of the ad), hide the empty space where an ad was, or block ads served from the same domain as the page (for example YouTube video ads).

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
| `[blocking]` | `block_mode` | `null` | `null` answers `0.0.0.0` / `::`; `nxdomain` answers "no such domain" |
| | `max_domains` | `1500000` | Cap on domains loaded from `lists/` (1000–10000000). About 8 bytes each, twice that while reloading |
| | `preset` | | `update-block` (see [Blocking](#blocking)). Repeat the key to enable several |

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
