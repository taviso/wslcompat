/* Verify polyfilled syscalls return EFAULT for NULL pointer arguments
 * rather than crashing. Covers the shims that dereference user pointers
 * before reaching the kernel:
 *   - clock_nanosleep(request)
 *   - execveat(pathname) without AT_EMPTY_PATH
 */
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

/* Force NULL through a volatile so the compiler can't see through it
 * and decide our call has UB. */
static void *opaque_null(void)
{
    static void *volatile p;
    return p;
}

static void test_clock_nanosleep_null(void)
{
    printf("--- clock_nanosleep(REALTIME, 0, NULL, NULL) ---\n");
    int rc = clock_nanosleep(CLOCK_REALTIME, 0, opaque_null(), NULL);
    CHECK(rc == EFAULT,
          "rc=%d (%s) -- want EFAULT (positive)", rc, strerror(rc));
}

static void test_execveat_null(void)
{
    printf("--- execveat(AT_FDCWD, NULL, ...) ---\n");
    char *argv[] = { "true", NULL };
    char *envp[] = { NULL };
    int rc = execveat(AT_FDCWD, opaque_null(), argv, envp, 0);
    int saved = errno;
    /* execveat doesn't return on success; if we reach here, it failed. */
    CHECK(rc == -1 && saved == EFAULT,
          "rc=%d errno=%d (%s) -- want -1/EFAULT",
          rc, saved, strerror(saved));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_clock_nanosleep_null();
    test_execveat_null();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
