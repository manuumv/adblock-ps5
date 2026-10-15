/* SPDX-License-Identifier: GPL-3.0-or-later
 * platform.h for the PS5 (etaHEN / onionHEN, ps5-payload-dev SDK).
 * Process naming, AuthID, libSceNet setup, the process list and notifications
 * adapted from nanoDNS (c) Drakmor. GPLv3. */
#include "core/platform.h"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PROCESS_NAME ADBLOCK_PROCESS_NAME
/* struct kinfo_proc offsets on the PS5 kernel (nanoDNS). */
#define KI_PID_OFFSET 72
#define KI_TDNAME_OFFSET 447

int sceKernelSendNotificationRequest(int, void *, size_t, int);

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request_t;
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
        plat_notify("ps5-adblock: cannot raise privileges (is the HEN running?)");
        return -1;
    }
    if (sceNetInit() != 0) {
        plat_notify("ps5-adblock: network init failed (sceNetInit)");
        return -1;
    }
    net_pool = sceNetPoolCreate("ps5-adblock", 64 * 1024, 0);
    if (net_pool < 0) {
        plat_notify("ps5-adblock: network init failed (sceNetPoolCreate)");
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

int plat_find_pid(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
        return PLAT_PID_UNKNOWN;
    size += size / 4; /* room for processes started in between */
    uint8_t *buf = malloc(size);
    if (!buf)
        return PLAT_PID_UNKNOWN;
    if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
        free(buf);
        return PLAT_PID_UNKNOWN;
    }

    pid_t self = getpid();
    int found = -1;
    for (uint8_t *p = buf; p + KI_TDNAME_OFFSET < buf + size;) {
        int structsize = *(int *)p;
        if (structsize <= KI_TDNAME_OFFSET) { /* layout not what we expect: do not guess */
            found = PLAT_PID_UNKNOWN;
            break;
        }
        pid_t pid = *(pid_t *)(p + KI_PID_OFFSET);
        const char *tdname = (const char *)(p + KI_TDNAME_OFFSET);
        if (pid != self && strncmp(tdname, name, (size_t)(structsize - KI_TDNAME_OFFSET)) == 0)
            found = pid;
        p += structsize;
    }
    free(buf);
    return found;
}

int plat_kill(int pid) { return kill((pid_t)pid, SIGKILL); }

void plat_notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    (void)sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* etaHEN and onionHEN both start payloads/<x>.elf when <x>.elf.auto_start
 * sits next to it. A HEN counts as installed when its data folder exists. */
const char *const *plat_hen_dirs(void) {
    static const char *list[3];
    size_t n = 0;
    if (is_dir("/data/etaHEN"))
        list[n++] = "/data/etaHEN/payloads";
    if (is_dir("/data/OnionHEN"))
        list[n++] = "/data/OnionHEN/payloads";
    list[n] = NULL;
    return list;
}

const char *const *plat_payload_copies(void) {
    static const char *const list[] = {"/data/pldmgr/payloads/ps5-adblock/ps5-adblock.elf", NULL};
    return list;
}
