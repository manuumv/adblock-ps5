/* SPDX-License-Identifier: GPL-3.0-or-later
 * update-block takes the default rules of nanoDNS (c) Drakmor, GPLv3. */
#include "blocking/presets.h"

#include <string.h>

/* Firmware updates and PSN. Blocking these takes PSN features offline. */
static const char *const UPDATE_BLOCK[] = {
    "*.playstation.com", "*.playstation.com.*", "playstation.com",
    "*.playstation.net", "*.playstation.net.*", "playstation.net",
    "*.psndl.net",       "psndl.net",           NULL,
};

/* nanoDNS keeps feature.api, STUN, ena.net, post.net and gst.prod.dl reachable
 * for the network test; through them the console shows itself connected to
 * PSN. None is kept here, as Sony-blocking DNS servers do. */
static const char *const NONE[] = {NULL};

/* ponytail: empty until a real console query log classifies Sony telemetry
 * hosts (blocklist task 5.1); guessing would break PSN. */
static const char *const PS_TELEMETRY[] = {NULL};

static const preset_t PRESETS[] = {
    {"update-block", UPDATE_BLOCK, NONE},
    {"ps-telemetry", PS_TELEMETRY, NONE},
};

const preset_t *preset_find(const char *name) {
    for (size_t i = 0; i < sizeof PRESETS / sizeof *PRESETS; i++)
        if (!strcmp(PRESETS[i].name, name))
            return &PRESETS[i];
    return NULL;
}
