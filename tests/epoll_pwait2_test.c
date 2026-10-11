#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "testutil.h"

int main(void)
{
    int (*ep2)(int, struct epoll_event *, int, const struct timespec *, const sigset_t *) =
        dlsym(RTLD_DEFAULT, "epoll_pwait2");
    
    if (!ep2) {
        wslcompat_test_require_or_skip(0, "glibc lacks epoll_pwait2");
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0, "epoll_create1");

    // 100ms timeout
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    struct epoll_event ev[1];

    // Track elapsed time to ensure timeout is respected
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int r = ep2(epfd, ev, 1, &ts, NULL);
    if (r < 0 && errno == ENOSYS) {
        wslcompat_test_require_or_skip(0, "epoll_pwait2 returns ENOSYS");
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    CHECK(r == 0, "epoll_pwait2 returns %d", r);
    
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    CHECK(elapsed_ms >= 90 && elapsed_ms <= 300, "timeout respected (elapsed %ld ms)", elapsed_ms);

    close(epfd);
    TEST_REPORT();
}
