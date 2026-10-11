/* Basic inotify smoke test. The synth-fd probe re-fire machinery
 * (see docs/PROBE_REFIRE.md) and inotify_path_close_test depend on
 * inotify working at all on WSL1; this one keeps us honest if the
 * platform ever regresses. Pure libc, no shim required. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static int drain_one(int ino, uint32_t *mask_out)
{
    char buf[4096] __attribute__((aligned(8)));
    ssize_t n = read(ino, buf, sizeof(buf));
    if (n < (ssize_t)sizeof(struct inotify_event))
        return -1;
    struct inotify_event *ev = (struct inotify_event *)buf;
    *mask_out = ev->mask;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(ino >= 0, "inotify_init1 -> fd=%d", ino);
    if (ino < 0) return 1;

    char path[] = "/tmp/inotify_basic_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) err(EXIT_FAILURE, "mkstemp");

    int wd = inotify_add_watch(ino, path, IN_MODIFY | IN_CLOSE_WRITE);
    CHECK(wd >= 0, "inotify_add_watch -> wd=%d", wd);

    // IN_MODIFY: write should fire it.
    if (write(fd, "x", 1) != 1) err(EXIT_FAILURE, "write");

    struct pollfd pfd = { .fd = ino, .events = POLLIN };
    int pr = poll(&pfd, 1, 1000);
    CHECK(pr == 1 && (pfd.revents & POLLIN),
          "poll(inotify) after write -> %d revents=0x%x", pr, pfd.revents);

    uint32_t mask = 0;
    int r = drain_one(ino, &mask);
    CHECK(r == 0 && (mask & IN_MODIFY),
          "IN_MODIFY delivered (mask=0x%x)", mask);

    // IN_CLOSE_WRITE: closing the writable fd should fire it.
    close(fd);

    pfd.revents = 0;
    pr = poll(&pfd, 1, 1000);
    CHECK(pr == 1 && (pfd.revents & POLLIN),
          "poll(inotify) after close -> %d revents=0x%x", pr, pfd.revents);

    mask = 0;
    r = drain_one(ino, &mask);
    CHECK(r == 0 && (mask & IN_CLOSE_WRITE),
          "IN_CLOSE_WRITE delivered (mask=0x%x)", mask);

    // Removing the watch should succeed and return IN_IGNORED.
    int rm = inotify_rm_watch(ino, wd);
    CHECK(rm == 0, "inotify_rm_watch -> %d", rm);

    mask = 0;
    r = drain_one(ino, &mask);
    CHECK(r == 0 && (mask & IN_IGNORED),
          "IN_IGNORED delivered after rm_watch (mask=0x%x)", mask);

    close(ino);
    unlink(path);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
