#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <err.h>

static int failures;
static int subtests;
static char tmppath[] = "/tmp/wslcompat_filetime_XXXXXX";

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) {                                                     \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__);                 \
    } else {                                                        \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                 \
        failures++;                                                 \
    }                                                               \
} while (0)

static void cleanup(void)
{
    if (tmppath[0]) unlink(tmppath);
}

static int do_stat(struct stat *st, const char *where)
{
    if (stat(tmppath, st) != 0) {
        printf("  FAIL: %s: stat: %s\n", where, strerror(errno));
        failures++; subtests++;
        return -1;
    }
    return 0;
}

/* Windows NTFS native timestamp resolution is 100ns, so the kernel may
 * round the requested nanoseconds to the nearest 100ns. Accept that. */
#define NSEC_TOLERANCE 100L
static int nsec_close(long got, long want)
{
    long diff = got - want;
    if (diff < 0) diff = -diff;
    return diff <= NSEC_TOLERANCE;
}

static void test_utime(void)
{
    printf("--- utime() ---\n");
    struct utimbuf t = { .actime = 1000000000, .modtime = 1500000000 };
    if (utime(tmppath, &t) != 0) {
        printf("  FAIL: utime: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "utime") != 0) return;
    CHECK(st.st_atim.tv_sec == 1000000000,
          "atime sec = %ld (want 1000000000)", (long)st.st_atim.tv_sec);
    CHECK(st.st_mtim.tv_sec == 1500000000,
          "mtime sec = %ld (want 1500000000)", (long)st.st_mtim.tv_sec);
}

static void test_utimes(void)
{
    printf("--- utimes() ---\n");
    struct timeval tv[2] = {
        { .tv_sec = 1100000000, .tv_usec = 250000 }, /* atime */
        { .tv_sec = 1200000000, .tv_usec = 500000 }, /* mtime */
    };
    if (utimes(tmppath, tv) != 0) {
        printf("  FAIL: utimes: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "utimes") != 0) return;
    CHECK(st.st_atim.tv_sec == 1100000000,
          "atime sec = %ld", (long)st.st_atim.tv_sec);
    CHECK(st.st_mtim.tv_sec == 1200000000,
          "mtime sec = %ld", (long)st.st_mtim.tv_sec);
    CHECK(st.st_atim.tv_nsec == 250000L * 1000L,
          "atime nsec = %ld (want 250000000)", st.st_atim.tv_nsec);
    CHECK(st.st_mtim.tv_nsec == 500000L * 1000L,
          "mtime nsec = %ld (want 500000000)", st.st_mtim.tv_nsec);
}

static void test_utimensat_basic(void)
{
    printf("--- utimensat() basic ---\n");
    struct timespec ts[2] = {
        { .tv_sec = 1300000000, .tv_nsec = 123456789 },
        { .tv_sec = 1400000000, .tv_nsec = 987654321 },
    };
    if (utimensat(AT_FDCWD, tmppath, ts, 0) != 0) {
        printf("  FAIL: utimensat: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "utimensat") != 0) return;
    CHECK(st.st_atim.tv_sec == 1300000000,
          "atime sec = %ld", (long)st.st_atim.tv_sec);
    CHECK(nsec_close(st.st_atim.tv_nsec, 123456789),
          "atime nsec = %ld (want 123456789 +/- %ld)",
          st.st_atim.tv_nsec, NSEC_TOLERANCE);
    CHECK(st.st_mtim.tv_sec == 1400000000,
          "mtime sec = %ld", (long)st.st_mtim.tv_sec);
    CHECK(nsec_close(st.st_mtim.tv_nsec, 987654321),
          "mtime nsec = %ld (want 987654321 +/- %ld)",
          st.st_mtim.tv_nsec, NSEC_TOLERANCE);
}

static void test_utimensat_omit(void)
{
    printf("--- utimensat() UTIME_OMIT ---\n");
    /* Plant known values first. */
    struct timespec base[2] = {
        { .tv_sec = 1300000000, .tv_nsec = 100000000 },
        { .tv_sec = 1400000000, .tv_nsec = 200000000 },
    };
    if (utimensat(AT_FDCWD, tmppath, base, 0) != 0) {
        printf("  FAIL: setup: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct timespec ts[2] = {
        { .tv_sec = 0, .tv_nsec = UTIME_OMIT },
        { .tv_sec = 1500000000, .tv_nsec = 333000000 },
    };
    if (utimensat(AT_FDCWD, tmppath, ts, 0) != 0) {
        printf("  FAIL: utimensat UTIME_OMIT: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "UTIME_OMIT") != 0) return;
    CHECK(st.st_atim.tv_sec == 1300000000,
          "atime unchanged sec = %ld", (long)st.st_atim.tv_sec);
    CHECK(st.st_mtim.tv_sec == 1500000000,
          "mtime updated sec = %ld", (long)st.st_mtim.tv_sec);
}

static void test_utimensat_now(void)
{
    printf("--- utimensat() UTIME_NOW ---\n");
    time_t before = time(NULL);
    struct timespec ts[2] = {
        { .tv_sec = 0, .tv_nsec = UTIME_NOW },
        { .tv_sec = 0, .tv_nsec = UTIME_NOW },
    };
    if (utimensat(AT_FDCWD, tmppath, ts, 0) != 0) {
        printf("  FAIL: utimensat UTIME_NOW: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "UTIME_NOW") != 0) return;
    time_t after = time(NULL);
    CHECK(st.st_atim.tv_sec >= before - 1 && st.st_atim.tv_sec <= after + 1,
          "atime = %ld (within [%ld, %ld])",
          (long)st.st_atim.tv_sec, (long)before, (long)after);
    CHECK(st.st_mtim.tv_sec >= before - 1 && st.st_mtim.tv_sec <= after + 1,
          "mtime = %ld (within [%ld, %ld])",
          (long)st.st_mtim.tv_sec, (long)before, (long)after);
}

static void test_utimensat_null(void)
{
    /* NULL times => same as UTIME_NOW on both. */
    printf("--- utimensat() NULL ---\n");
    time_t before = time(NULL);
    if (utimensat(AT_FDCWD, tmppath, NULL, 0) != 0) {
        printf("  FAIL: utimensat NULL: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct stat st;
    if (do_stat(&st, "NULL times") != 0) return;
    time_t after = time(NULL);
    CHECK(st.st_atim.tv_sec >= before - 1 && st.st_atim.tv_sec <= after + 1,
          "atime = %ld (within [%ld, %ld])",
          (long)st.st_atim.tv_sec, (long)before, (long)after);
}

static void test_futimens(void)
{
    printf("--- futimens() ---\n");
    int fd = open(tmppath, O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: open: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    struct timespec ts[2] = {
        { .tv_sec = 1600000000, .tv_nsec = 555555555 },
        { .tv_sec = 1700000000, .tv_nsec = 666666666 },
    };
    int rc = futimens(fd, ts);
    if (rc != 0) {
        printf("  FAIL: futimens: %s\n", strerror(errno));
        failures++; subtests++;
        close(fd);
        return;
    }
    struct stat st;
    int strc = fstat(fd, &st);
    close(fd);
    if (strc != 0) {
        printf("  FAIL: fstat after futimens: %s\n", strerror(errno));
        failures++; subtests++;
        return;
    }
    CHECK(st.st_atim.tv_sec == 1600000000,
          "atime sec = %ld", (long)st.st_atim.tv_sec);
    CHECK(nsec_close(st.st_atim.tv_nsec, 555555555),
          "atime nsec = %ld (want 555555555 +/- %ld)",
          st.st_atim.tv_nsec, NSEC_TOLERANCE);
    CHECK(st.st_mtim.tv_sec == 1700000000,
          "mtime sec = %ld", (long)st.st_mtim.tv_sec);
    CHECK(nsec_close(st.st_mtim.tv_nsec, 666666666),
          "mtime nsec = %ld (want 666666666 +/- %ld)",
          st.st_mtim.tv_nsec, NSEC_TOLERANCE);
}

static void timeout(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: test timed out\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = mkstemp(tmppath);
    if (fd < 0) err(EXIT_FAILURE, "mkstemp");
    close(fd);
    atexit(cleanup);

    struct sigaction sa = { .sa_handler = timeout };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(3);

    test_utime();
    test_utimes();
    test_utimensat_basic();
    test_utimensat_omit();
    test_utimensat_now();
    test_utimensat_null();
    test_futimens();

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
