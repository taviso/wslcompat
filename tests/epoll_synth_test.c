/* Two phases:
 *
 *   Baseline (runs always): exercise epoll_ctl ADD/MOD/DEL + epoll_wait
 *   on a real pipe fd, so the shim's canary path can't have broken plain
 *   epoll for native fds.
 *
 *   Synth (runs only when libwslcompat is loaded): the synth fd is an
 *   O_PATH descriptor; the kernel rejects O_PATH on EPOLL_CTL_ADD with
 *   EPERM, and MOD/DEL with ENOENT (the registration is keyed on the
 *   synth's SYNTH_OP_POLL backing, not the O_PATH the caller sees).
 *   epoll.c canaries on those errnos and retries with the passthru
 *   backing; ADD then succeeds, and MOD/DEL find the entry. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <err.h>

#include "../synth.h"

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int pipe_read_fd = -1;

static int pipe_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    (void)e;
    (void)kind;
    return pipe_read_fd;
}

static const struct synth_ops pipe_ops = {
    .passthru = pipe_passthru,
};

// Run ADD / wait / MOD / DEL against `watch_fd`. Label tags the output
// so the baseline and synth phases are distinguishable in the log.
static void run_epoll_cycle(const char *label, int watch_fd,
                            int pipe_write_fd)
{
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        err(EXIT_FAILURE, "epoll_create1");

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = watch_fd };
    int r = epoll_ctl(ep, EPOLL_CTL_ADD, watch_fd, &ev);
    CHECK(r == 0, "[%s] epoll_ctl(EPOLL_CTL_ADD) -> %d %s",
          label, r, r < 0 ? strerror(errno) : "");
    if (r != 0) {
        close(ep);
        return;
    }

    if (write(pipe_write_fd, "x", 1) != 1)
        err(EXIT_FAILURE, "pipe write");

    struct epoll_event got;
    int n = epoll_wait(ep, &got, 1, 1000);
    CHECK(n == 1 && (got.events & EPOLLIN),
          "[%s] epoll_wait -> n=%d events=0x%x",
          label, n, n > 0 ? got.events : 0);

    // Drain so the next phase's write starts edge-clean.
    char drain;
    if (read(pipe_read_fd, &drain, 1) != 1)
        err(EXIT_FAILURE, "pipe drain");

    struct epoll_event ev2 = { .events = EPOLLIN | EPOLLET, .data.fd = watch_fd };
    int rm = epoll_ctl(ep, EPOLL_CTL_MOD, watch_fd, &ev2);
    CHECK(rm == 0, "[%s] epoll_ctl(EPOLL_CTL_MOD) -> %d %s",
          label, rm, rm < 0 ? strerror(errno) : "");

    int rd = epoll_ctl(ep, EPOLL_CTL_DEL, watch_fd, NULL);
    CHECK(rd == 0, "[%s] epoll_ctl(EPOLL_CTL_DEL) -> %d %s",
          label, rd, rd < 0 ? strerror(errno) : "");

    close(ep);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int pfd[2];
    if (pipe(pfd) < 0)
        err(EXIT_FAILURE, "pipe");
    pipe_read_fd = pfd[0];

    // Phase 1: plain epoll on a real fd. Independent of the shim.
    run_epoll_cycle("baseline", pipe_read_fd, pfd[1]);

    // Phase 2: same cycle through a synth O_PATH wrap, only when the
    // shim is present to provide wslcompat_synth_open.
    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    if (synth_open == NULL) {
        printf("  SKIP: libwslcompat not loaded -- synth phase skipped\n");
    } else {
        int synth = synth_open(O_CLOEXEC, &pipe_ops, NULL);
        CHECK(synth >= 0, "synth_open -> %d", synth);
        if (synth >= 0) {
            run_epoll_cycle("synth", synth, pfd[1]);
            close(synth);
        }
    }

    close(pfd[0]);
    close(pfd[1]);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
