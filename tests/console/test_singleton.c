#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/platform.h"
#include "console/runtime.h"

/* The host platform finds the "previous instance" through ADBLOCK_PEER_PID. */
static pid_t spawn(int ignore_term) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
        for (;;)
            pause();
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)pid);
    setenv("ADBLOCK_PEER_PID", buf, 1);
    usleep(50 * 1000); /* let the child install its handler */
    return pid;
}

int main(void) {
    /* No previous instance. */
    unsetenv("ADBLOCK_PEER_PID");
    assert(runtime_take_over(ADBLOCK_PROCESS_NAME, 500) == 0);

    /* A previous instance is stopped. */
    pid_t old = spawn(0);
    assert(runtime_take_over(ADBLOCK_PROCESS_NAME, 2000) == 0);
    int st;
    assert(waitpid(old, &st, WNOHANG) == -1 || WIFSIGNALED(st)); /* already reaped, or killed */

    /* One that refuses to go: give up after the timeout. */
    pid_t stuck = spawn(1);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(runtime_take_over(ADBLOCK_PROCESS_NAME, 600) == -1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    assert(ms >= 550 && ms < 1500);
    kill(stuck, SIGKILL);
    waitpid(stuck, &st, 0);

    puts("test_singleton: ok");
    return 0;
}
