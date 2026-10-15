/* SPDX-License-Identifier: GPL-3.0-or-later
 * Built-in rule sets enabled with preset=<name> in [blocking]. */
#ifndef ADBLOCK_PRESETS_H
#define ADBLOCK_PRESETS_H

typedef struct {
    const char *name;
    const char *const *block; /* masks to block, NULL-terminated */
    const char *const *allow; /* masks that always go upstream, NULL-terminated */
} preset_t;

/* The preset called name, or NULL. */
const preset_t *preset_find(const char *name);

#endif
