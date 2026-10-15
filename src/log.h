/* SPDX-License-Identifier: GPL-3.0-or-later
 * In-memory log: the last LOG_LINES lines, never written to disk per event.
 * With debug on, every line is mirrored to stderr (klog on the console). */
#ifndef ADBLOCK_LOG_H
#define ADBLOCK_LOG_H

#include <stddef.h>

#define LOG_LINES 256
#define LOG_LINE_LEN 160

void log_set_debug(int on);
int log_debug_enabled(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies line i (0 = oldest kept) into out. Returns the number of lines kept;
 * out is untouched when i is out of range. */
size_t log_line(size_t i, char *out, size_t n);

/* Lines written since start, including those already dropped from the ring. */
size_t log_total(void);

#endif
