/* SPDX-License-Identifier: GPL-3.0-or-later
 * update-block reproduces the default rules of nanoDNS (c) Drakmor, GPLv3. */
#include "presets.h"

#include <string.h>

/* Firmware updates and PSN. Blocking these takes PSN features offline. */
static const char *const UPDATE_BLOCK[] = {
    "*.playstation.com", "*.playstation.com.*", "playstation.com",
    "*.playstation.net", "*.playstation.net.*", "playstation.net",
    "*.psndl.net",       "psndl.net",           NULL,
};

/* Kept reachable so the console's network test and NAT traversal still work. */
static const char *const UPDATE_BLOCK_ALLOW[] = {
    "feature.api.playstation.com", "*.stun.playstation.net",   "stun.*.playstation.net",
    "ena.net.playstation.net",     "post.net.playstation.net", "gst.prod.dl.playstation.net",
    NULL,
};

/* ponytail: empty until a real console query log classifies Sony telemetry
 * hosts (blocklist task 5.1); guessing would break PSN. */
static const char *const PS_TELEMETRY[] = {NULL};
static const char *const NONE[] = {NULL};

static const preset_t PRESETS[] = {
    {"update-block", UPDATE_BLOCK, UPDATE_BLOCK_ALLOW},
    {"ps-telemetry", PS_TELEMETRY, NONE},
};

const preset_t *preset_find(const char *name) {
    for (size_t i = 0; i < sizeof PRESETS / sizeof *PRESETS; i++)
        if (!strcmp(PRESETS[i].name, name))
            return &PRESETS[i];
    return NULL;
}
