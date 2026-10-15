/* SPDX-License-Identifier: GPL-3.0-or-later
 * Console-specific code. Each build links one platform/<name>/platform.c:
 * ps5 (etaHEN / onionHEN) or host (macOS and Linux, for development). */
#ifndef ADBLOCK_PLATFORM_H
#define ADBLOCK_PLATFORM_H

/* Names the process, raises privileges and starts networking. -1 on failure. */
int plat_init(void);

/* Releases what plat_init acquired. */
void plat_fini(void);

/* Directory for config.ini, lists and logs. */
const char *plat_data_dir(void);

/* UDP port the DNS server listens on (53 on the console). */
int plat_dns_port(void);

#endif
