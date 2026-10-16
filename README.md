# ps5-adblock

[![build](https://github.com/manuumv/adblock-ps5/actions/workflows/build.yml/badge.svg)](https://github.com/manuumv/adblock-ps5/actions/workflows/build.yml)
[![license: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)

An ad blocker that runs inside a jailbroken PS5, like a Pi-hole on the console. It answers the console's DNS lookups itself: ad, tracker and Sony update domains get `0.0.0.0`, everything else goes to your normal DNS. No computer or router changes needed.

> **Status: early.** Please report what you find.

## What it blocks

| ✅ Blocks | ❌ Cannot block |
|---|---|
| Ad networks and trackers (~200,000 domains, [HaGeZi](https://github.com/hagezi/dns-blocklists) lists) | Ads from the same domain as the content, such as YouTube video ads |
| Scripts that open pop-up ad tabs | A tab the site opens itself (it shows an error page instead) |
| Firmware updates and PSN (on by default) | The empty space where an ad was |

## Install

You need a jailbroken PS5 with an ELF loader on port **9021**.

1. Download `ps5-adblock.elf` from [Releases](https://github.com/manuumv/adblock-ps5/releases), or [build it](#development).
2. Send it to the loader: `nc -w 1 <ps5-ip> 9021 < ps5-adblock.elf`
3. Wait for **"blocking N domains and firmware updates, DNS 127.0.0.1"**, about 30 seconds on the first start, while the lists download.

**Autostart (optional, etaHEN or onionHEN):** upload the ELF over FTP to `/data/etaHEN/payloads/` (onionHEN: `/data/OnionHEN/payloads/`) next to an empty file named `ps5-adblock.elf.auto_start`. Or run `make install PS5_HOST=<ps5-ip>`. Without a HEN, send the payload after each reboot.

## The console's DNS

You do not need to change the network settings. While it runs, ps5-adblock points the console's DNS at itself. When it stops, or the console reboots, the console is back on exactly the DNS it had. The settings are never written.

**Recommended:** in the network settings, use a DNS that blocks Sony, so the console cannot update before the jailbreak either. List it as the upstream too:

```ini
[upstream]
server=<your Sony-blocking DNS>
```

If you get *"no lookup reached the blocker"*, this console did not accept the redirect. Set **Primary DNS** to `127.0.0.1` and leave **Secondary DNS** empty instead. Turn the redirect off with `dns_redirect=0`.

## Firmware updates

A firmware update removes the jailbreak. ps5-adblock blocks every name under `playstation.com`, `playstation.net` and `psndl.net`, with no exceptions. The store, game downloads, online play and the PSN network test do not work while it runs; that is expected.

To use PSN, set `block_updates=0` and turn off automatic update downloads in **Settings › System › System Software**.

## Lists and allowlist

- **Lists:** every `.txt` in `/data/ps5-adblock/lists/` (hosts files, plain domains or `||domain^` rules). Each domain also blocks its subdomains. Changes apply within 30 seconds.
- **Downloads:** the `[lists]` entries are fetched over HTTPS and refreshed every 24 hours. A failed download keeps the previous copy. Add your own with `url=<name>|<https url>`.
- **Allowlist:** domains in `/data/ps5-adblock/allowlist.txt` are never blocked. Use it when a site breaks.

## Configuration

`/data/ps5-adblock/config.ini` is created on the first start (see [`config.ini.example`](config.ini.example)). Changes apply when the payload starts again. Invalid lines are skipped and noted in `last.log`.

| Key | Default | Meaning |
|---|---|---|
| `[general] dns_redirect` | `1` | Point the console's DNS at ps5-adblock while it runs |
| `[general] autostart` | `1` | Keep the payload in the HEN autostart folders |
| `[general] quiet` / `debug` | `0` | Hide start notifications / log every query |
| `[general] bind` / `bind6` | `127.0.0.1` / `::1` | Listen addresses. `0.0.0.0` serves your LAN (private clients only) |
| `[upstream] server` | `1.1.1.1`, `9.9.9.9` | Real DNS servers, in order (up to 8) |
| `[upstream] timeout_ms` | `1500` | Time budget for one lookup |
| `[blocking] block_updates` | `1` | Block firmware updates and PSN |
| `[blocking] block_mode` | `null` | `null` answers `0.0.0.0`, `nxdomain` answers "no such domain" |
| `[blocking] max_domains` | `1500000` | Most domains loaded from `lists/` |
| `[lists] url` / `update_hours` | HaGeZi Pro + Pop-Up Ads / `24` | Lists to download, and hours between checks (`0` = never) |
| `[overrides] mask=IPv4` | | Answer matching names with this address |
| `[exceptions] mask` | | Names that are never blocked or overridden |

Masks accept `*`, `?` and `[a-z]`. `*.example.com` matches subdomains only.

## Troubleshooting

- **Nothing is blocked:** look for a *no lookup reached the blocker* or *nothing is blocked* notification and follow it.
- **Test pages score low:** on [adblock.turtlecute.org](https://adblock.turtlecute.org/), only the *Hosts* tests can pass with DNS blocking (126 of 128). The script and cosmetic tests need an in-browser blocker.
- **"cannot open DNS port 53":** another DNS payload such as nanoDNS is running. Stop it.
- **"Check the console date and time":** a wrong clock makes HTTPS certificates look invalid.
- **No internet after a crash:** reconnect the network or reboot to get your DNS back.
- **Logs:** `/data/ps5-adblock/last.log` (and `last.previous.log`). `debug=1` logs every query.

**Uninstall:** set `autostart=0` and send the payload once, or delete `ps5-adblock.elf*` from the HEN `payloads/` folders. Delete `/data/ps5-adblock/` to remove the config, lists and logs.

## Development

```sh
tools/ps5-build.sh                 # PS5 ELF in Docker -> build/ps5/ps5-adblock.elf
make deploy PS5_HOST=<ps5-ip>      # send it to port 9021
make host && make test             # macOS/Linux build and tests (ASan + UBSan)
ADBLOCK_DATA=./data ./build/host/ps5-adblock   # DNS on 127.0.0.1:5300
make fuzz FUZZ=dns                 # libFuzzer (dns or listparse)
```

**Releases:** merging `develop` into `master` publishes `v<VERSION>` (from the `Makefile`) with the ELF and its SHA-256. Bump `VERSION` before merging to release again.

Host builds need libcurl; the tests need python3 and openssl. Everything outside `platform/ps5/` builds on a computer: `src/core` (config, log), `src/dns` (server), `src/blocking` (lists), `src/console` (runtime), `src/updates` (downloads).

## Credits

[nanoDNS](https://github.com/drakmor/nanoDNS) (Drakmor) and the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (John Törnblom).

## License

GPLv3, see [LICENSE](LICENSE). Not affiliated with Sony. Use at your own risk.
