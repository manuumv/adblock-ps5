/* SPDX-License-Identifier: GPL-3.0-or-later
 * Console-specific code. Each build links one platform/<name>/platform.c:
 * ps5 (etaHEN / onionHEN) or host (macOS and Linux, for development). */
#ifndef ADBLOCK_PLATFORM_H
#define ADBLOCK_PLATFORM_H

#define ADBLOCK_PROCESS_NAME "ps5-adblock.elf"

/* Names the process, raises privileges and starts networking. -1 on failure,
 * after telling the user why on screen. */
int plat_init(void);

/* Releases what plat_init acquired. */
void plat_fini(void);

/* Directory for config.ini, lists and logs. */
const char *plat_data_dir(void);

/* UDP port the DNS server listens on (53 on the console). */
int plat_dns_port(void);

/* PID of another live process called name, -1 if there is none, or
 * PLAT_PID_UNKNOWN when the process list cannot be read. */
#define PLAT_PID_UNKNOWN (-2)
int plat_find_pid(const char *name);

/* Asks that process to stop: SIGKILL on the console, SIGTERM on the host (so
 * a test can play a stuck instance). 0 on success. */
int plat_kill(int pid);

/* On-screen toast (host: a line on stderr). */
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* payloads/ folders of the installed HENs, NULL-terminated. */
const char *const *plat_hen_dirs(void);

/* Paths where other tools keep their own copy of the payload (a payload
 * manager's autoload), NULL-terminated. */
const char *const *plat_payload_copies(void);

#endif
