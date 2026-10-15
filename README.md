# ps5-adblock

A DNS-based ad and telemetry blocker that runs on a jailbroken PS5. It is a payload: the console's DNS points at itself (`127.0.0.1`) and ps5-adblock decides which names to answer locally and which to forward to a real resolver.

> **Status: early.** DNS blocking with lists of up to millions of domains, an allowlist, live reloads and autostart with etaHEN/onionHEN. A web dashboard, an installer and automatic list downloads are on the way.

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

## Install

### 1. Put the payload in the HEN's autostart folder (once)

etaHEN and onionHEN start `payloads/<name>.elf` whenever `<name>.elf.auto_start` sits next to it. From a computer:

```sh
make install PS5_HOST=<ps5-ip>                                   # etaHEN (FTP on port 2121)
make install PS5_HOST=<ps5-ip> PS5_FTP=ftp://<ps5-ip>:1337 PS5_HEN_DIR=/data/OnionHEN   # onionHEN
```

Without a computer, any FTP app on a phone works:

1. Connect to the HEN's FTP server (etaHEN: port 2121, onionHEN: port 1337).
2. Upload `ps5-adblock.elf` to `/data/etaHEN/payloads/` (or `/data/OnionHEN/payloads/`).
3. Create an empty file next to it named `ps5-adblock.elf.auto_start`.
4. Send `ps5-adblock.elf` to the loader on port 9021 once, or reboot and jailbreak.

On every start the payload copies itself into the other HEN's folder too (and refreshes a `pldmgr` copy if you have one), so switching HENs keeps it running. It shows a notification once the lists are loaded: *ps5-adblock 0.1.0: blocking N domains, DNS 127.0.0.1*.

### 2. Point the console's DNS at it

**Settings › Network › Settings › Set Up Internet Connection**, select your connection, **Advanced Settings › DNS Settings › Manual**:
- **Primary DNS:** `127.0.0.1`
- **Secondary DNS:** leave it empty for full blocking. A public resolver there (for example `1.1.1.1`) keeps the console online before the jailbreak, but whether the PS5 then sends some queries past the blocker is not verified yet.

### After a reboot

The console has no DNS until the HEN runs ps5-adblock. Jailbreak first; the blocker starts with the HEN and notifies when it is ready. If your jailbreak chain needs the internet before the HEN loads, set **DNS Settings** to **Automatic** for that step.

### If the console loses its network

ps5-adblock is not running. Send the payload to port 9021 again, or set **DNS Settings** back to **Automatic**. If a notification says it *cannot open DNS port 53*, another DNS payload (for example nanoDNS) is using the port: stop it or remove it from the HEN's autostart.

### Uninstall

Set `autostart=0` in `/data/ps5-adblock/config.ini` and send the payload once (it removes its `.auto_start` markers), or delete `ps5-adblock.elf*` from the HEN `payloads/` folders over FTP. Then set **DNS Settings** to **Automatic**. `/data/ps5-adblock/` holds your config, lists and logs (`last.log`, `last.previous.log`).

### One-off run without autostart

```sh
make deploy PS5_HOST=<ps5-ip>        # or: nc -w 1 <ps5-ip> 9021 < build/ps5/ps5-adblock.elf
```

Sending it again replaces the running copy.

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
| | `quiet` | `0` | `1` hides the start notification; errors are always shown |
| | `autostart` | `1` | Keep the payload in every HEN's autostart folder; `0` removes the markers |
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
