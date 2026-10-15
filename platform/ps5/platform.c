/* SPDX-License-Identifier: GPL-3.0-or-later
 * platform.h for the PS5 (etaHEN / onionHEN, ps5-payload-dev SDK).
 * Process naming, AuthID and libSceNet setup adapted from nanoDNS (c) Drakmor, GPLv3. */
#include "platform.h"

#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PROCESS_NAME "ps5-adblock.elf"
/* ShellCore-like credentials, needed to bind port 53 (same as nanoDNS, ftpsrv, websrv). */
#define PRIVILEGED_AUTHID 0x4801000000000013L

int sceNetInit(void);
int sceNetTerm(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int memid);

static int net_pool = -1;

int plat_init(void) {
    (void)syscall(SYS_thr_set_name, -1, PROCESS_NAME);

    if (kernel_set_ucred_authid(getpid(), PRIVILEGED_AUTHID) != 0) {
        fprintf(stderr, "[ps5-adblock] unable to change AuthID\n");
        return -1;
    }
    if (sceNetInit() != 0) {
        fprintf(stderr, "[ps5-adblock] sceNetInit failed\n");
        return -1;
    }
    net_pool = sceNetPoolCreate("ps5-adblock", 64 * 1024, 0);
    if (net_pool < 0) {
        fprintf(stderr, "[ps5-adblock] sceNetPoolCreate failed\n");
        (void)sceNetTerm();
        return -1;
    }
    return 0;
}

void plat_fini(void) {
    if (net_pool >= 0) {
        (void)sceNetPoolDestroy(net_pool);
        net_pool = -1;
        (void)sceNetTerm();
    }
}

const char *plat_data_dir(void) { return "/data/ps5-adblock"; }

int plat_dns_port(void) { return 53; }
