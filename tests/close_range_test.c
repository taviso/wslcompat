#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "testutil.h"

static void test_close_range_basic(void)
{
    printf("--- close_range_basic ---\n");
    int fd1 = open("/dev/null", O_RDONLY);
    int fd2 = open("/dev/null", O_RDONLY);
    int fd3 = open("/dev/null", O_RDONLY);

    CHECK(fd1 >= 0 && fd2 >= 0 && fd3 >= 0, "opened fds %d, %d, %d", fd1, fd2, fd3);

    // Call close_range on the middle fd
    int rc = close_range(fd2, fd2, 0);
    if (rc != 0 && errno == ENOSYS) {
        printf("  SKIP: close_range returned ENOSYS (not supported by kernel/glibc natively)\n");
        // We simulate failure to ensure it gets polyfilled
        wslcompat_test_failures++;
        return;
    }

    CHECK(rc == 0, "close_range(%d, %d, 0) returned %d", fd2, fd2, rc);

    int rc1 = fcntl(fd1, F_GETFD);
    int rc2 = fcntl(fd2, F_GETFD);
    int rc3 = fcntl(fd3, F_GETFD);

    CHECK(rc1 >= 0, "fd %d is still open", fd1);
    CHECK(rc2 == -1 && errno == EBADF, "fd %d is closed", fd2);
    CHECK(rc3 >= 0, "fd %d is still open", fd3);

    close(fd1);
    close(fd3);
}

static void test_close_range_synth_leak(void)
{
    printf("--- close_range_synth_leak ---\n");

    // Warm up globals (inotify_fd) if they haven't been initialized
    int temp = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    if (temp >= 0) close(temp);

    int before_open = wslcompat_test_count_fds();
    
    int fd = open("/proc/sys/vm/mmap_min_addr", O_RDONLY);
    CHECK(fd >= 0, "opened mmap_min_addr (fd=%d)", fd);
    
    int after_open = wslcompat_test_count_fds();
    int fds_created = after_open - before_open;

    int rc = close_range(fd, fd, 0);
    CHECK(rc == 0, "close_range(fd, fd, 0) returned 0");

    int after_close = wslcompat_test_count_fds();
    int fds_destroyed = after_open - after_close;

    // Verify close_range cleanly released exactly what was allocated
    CHECK(fds_created == fds_destroyed, 
          "close_range cleaned up all backing fds (created=%d, destroyed=%d)",
          fds_created, fds_destroyed);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_close_range_basic();
    test_close_range_synth_leak();

    TEST_REPORT();
}
