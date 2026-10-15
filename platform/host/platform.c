/* SPDX-License-Identifier: GPL-3.0-or-later
 * platform.h for macOS and Linux development builds. ADBLOCK_DATA and
 * ADBLOCK_PORT override the data directory and the DNS port. */
#include "platform.h"

#include <stdlib.h>

int plat_init(void) { return 0; }

void plat_fini(void) {}

const char *plat_data_dir(void) {
    const char *dir = getenv("ADBLOCK_DATA");
    return dir && *dir ? dir : "./data";
}

int plat_dns_port(void) {
    const char *port = getenv("ADBLOCK_PORT");
    int p = port ? atoi(port) : 0;
    return p > 0 && p < 65536 ? p : 5300;
}
