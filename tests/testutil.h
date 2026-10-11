#ifndef WSLCOMPAT_TESTUTIL_H
#define WSLCOMPAT_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <dirent.h>

// Returns the number of open file descriptors for the current process
static inline int wslcompat_test_count_fds(void)
{
    int count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] >= '0' && de->d_name[0] <= '9') {
            count++;
        }
    }
    closedir(d);
    return count;
}

// Returns non-zero if we're running on a WSL kernel (uname release
// contains "Microsoft" / "microsoft").
static inline int wslcompat_test_on_wsl(void)
{
    struct utsname uts;
    if (uname(&uts) != 0)
        return 0;
    return strstr(uts.release, "Microsoft") != NULL ||
           strstr(uts.release, "microsoft") != NULL;
}

// For tests that rely on a WSL1-specific kernel quirk. Caller probes
// for the capability and passes the result. If the probe failed:
//   - on WSL: print FAIL with `desc` and exit(1) -- something regressed
//     in our environment
//   - elsewhere: print SKIP with `desc` and exit(0) -- expected on
//     stock Linux, keeps XPASS / linux: target green
// If the probe succeeded, returns and the test continues.
static inline void wslcompat_test_require_or_skip(int probe_ok, const char *desc)
{
    if (probe_ok)
        return;
    if (wslcompat_test_on_wsl()) {
        printf("FAIL: %s -- regression (on WSL but capability absent)\n", desc);
        exit(1);
    }
    printf("SKIP: %s (stock Linux kernel)\n", desc);
    exit(0);
}

static inline int wslcompat_test_verbose(void)
{
    static int cached = -1;
    if (__builtin_expect(cached < 0, 0)) {
        cached = (getenv("WSLCOMPAT_TEST_VERBOSE") != NULL);
    }
    return cached;
}

static int wslcompat_test_failures;
static int wslcompat_test_subtests;

#define STATUS(fmt, ...) do {                                       \
    if (wslcompat_test_verbose())                                   \
        printf(fmt "\n", ##__VA_ARGS__);                            \
} while (0)

#define CHECK(cond, fmt, ...) do {                                  \
    wslcompat_test_subtests++;                                      \
    if (cond) {                                                     \
        if (wslcompat_test_verbose())                               \
            printf("  PASS: " fmt "\n", ##__VA_ARGS__);             \
    } else {                                                        \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                 \
        wslcompat_test_failures++;                                  \
    }                                                               \
} while (0)

#define TEST_REPORT() do {                                          \
    printf("%d/%d subtests passed, %d failed\n",                    \
           wslcompat_test_subtests - wslcompat_test_failures,       \
           wslcompat_test_subtests, wslcompat_test_failures);       \
    return wslcompat_test_failures == 0 ? 0 : 1;                    \
} while (0)

#endif
