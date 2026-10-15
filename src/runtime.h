/* SPDX-License-Identifier: GPL-3.0-or-later
 * Process lifecycle on the console: single instance, autostart with the HENs,
 * on-screen notices and persisting the in-memory log. */
#ifndef ADBLOCK_RUNTIME_H
#define ADBLOCK_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define AUTOSTART_ELF "ps5-adblock.elf"
#define AUTOSTART_MARKER "ps5-adblock.elf.auto_start"
#define LOG_FLUSH_MS (10 * 60 * 1000)

/* Stops any other process called name and waits up to timeout_ms for it to
 * go. 0 when no other copy is left, -1 if one survives. */
int runtime_take_over(const char *name, int timeout_ms);

/* enable: makes every HEN payload folder in hen_dirs hold the newest copy of
 * the ELF plus its .auto_start marker, and refreshes the existing paths in
 * copies (never creating them). The newest copy is looked for in the HEN
 * folders, copies and own_copy. Copies keep the source's mtime, so a second
 * run changes nothing. !enable: removes the markers only.
 * Returns the number of files written or removed, AUTOSTART_NO_COPY when no
 * copy exists on disk yet (sent over the network), or -1 on an error (no HEN
 * folder, I/O); msg gets a one-line summary. Errors in one folder do not stop
 * the others. */
#define AUTOSTART_NO_COPY (-2)
int autostart_sync(int enable, const char *const *hen_dirs, const char *const *copies,
                   const char *own_copy, char *msg, size_t n);

/* "ps5-adblock <version>: blocking <n> domains, DNS <bind>", unless quiet. */
void runtime_ready_notice(const config_t *c, size_t domains);
/* Always shown: the DNS port could not be opened. */
void runtime_port_error_notice(int port);

/* Moves dir/last.log to dir/last.previous.log; later flushes write last.log. */
void runtime_log_start(const char *dir, int64_t now);
/* Writes last.log if there are new log lines and LOG_FLUSH_MS passed since the
 * last write. */
void runtime_log_tick(int64_t now);
/* Writes last.log now if there are new lines (orderly exit). */
void runtime_log_flush(void);

#endif
