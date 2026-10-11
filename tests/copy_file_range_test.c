/* copy_file_range(2) tests.
 *
 * WSL1 doesn't implement the syscall (ENOSYS), so this is shimmed with a
 * pread/pwrite fallback. Tests cover explicit offsets, NULL offsets (file
 * cursor), non-zero starting offset, data correctness, offset pointer
 * advancement, and multi-buffer copies that exercise the 4096-byte loop. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../synth.h"

// Minimal synth ops: forward IO to the real fd stored in userptr.
static int file_passthru(struct synth_entry *e, enum synth_op_kind kind)
{
    if (kind == SYNTH_OP_IO)
        return (int)(intptr_t)e->userptr;
    return -1;
}

static const struct synth_ops file_ops = {
    .passthru = file_passthru,
};

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                          \
    subtests++;                                                             \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);                  \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }       \
} while (0)

static const char seed[] = "0123456789abcdef";
#define SEEDLEN ((ssize_t)(sizeof(seed) - 1))

static int make_tmp(void)
{
    char path[] = "/tmp/cfr_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        err(EXIT_FAILURE, "mkstemp");
    unlink(path);
    return fd;
}

/* Write seed to fd and seek back to 0. */
static void seed_fd(int fd)
{
    if (lseek(fd, 0, SEEK_SET) != 0)
        err(EXIT_FAILURE, "lseek");
    if (write(fd, seed, SEEDLEN) != SEEDLEN)
        err(EXIT_FAILURE, "seed write");
}

/* Read len bytes from fd at offset pos, compare to expected. */
static int verify_data(int fd, loff_t pos, const char *expected, ssize_t len,
                       const char *label)
{
    char buf[64];
    ssize_t r = pread(fd, buf, len, pos);
    CHECK(r == len, "%s: read back %zd bytes (want %zd)", label, r, len);
    if (r != len)
        return -1;
    CHECK(memcmp(buf, expected, len) == 0,
          "%s: data matches", label);
    return 0;
}

/* Repeating 0..255 pattern used for large-buffer tests. */
static void fill_pattern(char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)(i % 256);
}

/* Write n bytes of pattern to fd, truncate and seek to 0 first. */
static void seed_large(int fd, size_t n)
{
    char *buf = malloc(n);
    if (!buf) err(EXIT_FAILURE, "malloc");
    fill_pattern(buf, n);
    if (ftruncate(fd, 0) != 0) err(EXIT_FAILURE, "ftruncate");
    if (lseek(fd, 0, SEEK_SET) != 0) err(EXIT_FAILURE, "lseek");
    if (write(fd, buf, n) != (ssize_t)n) err(EXIT_FAILURE, "write");
    free(buf);
}

/* Read n bytes from fd at pos, compare against fill_pattern. */
static void verify_large(int fd, loff_t pos, size_t n, const char *label)
{
    char *expected = malloc(n);
    char *got = malloc(n);
    ssize_t r;
    if (!expected || !got) err(EXIT_FAILURE, "malloc");
    fill_pattern(expected, n);
    r = pread(fd, got, n, pos);
    CHECK(r == (ssize_t)n, "%s: read back %zd bytes (want %zu)", label, r, n);
    if (r == (ssize_t)n)
        CHECK(memcmp(got, expected, n) == 0, "%s: data matches", label);
    free(expected);
    free(got);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(5);

    int in  = make_tmp();
    int out = make_tmp();

    /* --- Test 1: explicit offsets, full copy --- */
    printf("Test 1: explicit offsets, full copy\n");
    seed_fd(in);
    if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

    loff_t off_in = 0, off_out = 0;
    ssize_t r = copy_file_range(in, &off_in, out, &off_out, SEEDLEN, 0);

    CHECK(r == SEEDLEN,     "returned %zd bytes", r);
    CHECK(off_in  == SEEDLEN, "off_in advanced to %lld", (long long)off_in);
    CHECK(off_out == SEEDLEN, "off_out advanced to %lld", (long long)off_out);
    CHECK(lseek(in,  0, SEEK_CUR) == SEEDLEN, "in cursor unchanged at %lld", (long long)lseek(in, 0, SEEK_CUR));
    CHECK(lseek(out, 0, SEEK_CUR) == 0,       "out cursor unchanged at %lld", (long long)lseek(out, 0, SEEK_CUR));
    verify_data(out, 0, seed, SEEDLEN, "output data");

    /* --- Test 2: explicit non-zero starting offset --- */
    printf("Test 2: explicit non-zero starting offset\n");
    seed_fd(in);
    if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

    loff_t start = 4;
    ssize_t want = SEEDLEN - start;
    off_in = start; off_out = 0;
    r = copy_file_range(in, &off_in, out, &off_out, want, 0);

    CHECK(r == want,              "returned %zd bytes", r);
    CHECK(off_in  == SEEDLEN,     "off_in advanced to %lld", (long long)off_in);
    CHECK(off_out == want,        "off_out advanced to %lld", (long long)off_out);
    CHECK(lseek(in,  0, SEEK_CUR) == SEEDLEN, "in cursor unchanged at %lld", (long long)lseek(in, 0, SEEK_CUR));
    CHECK(lseek(out, 0, SEEK_CUR) == 0,       "out cursor unchanged at %lld", (long long)lseek(out, 0, SEEK_CUR));
    verify_data(out, 0, seed + start, want, "output data");

    /* --- Test 3: NULL offsets (file cursor) --- */
    printf("Test 3: NULL offsets\n");
    seed_fd(in);
    if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");
    if (lseek(in,  0, SEEK_SET) != 0) err(EXIT_FAILURE, "lseek in");
    if (lseek(out, 0, SEEK_SET) != 0) err(EXIT_FAILURE, "lseek out");

    r = copy_file_range(in, NULL, out, NULL, SEEDLEN, 0);

    CHECK(r == SEEDLEN, "returned %zd bytes", r);
    CHECK(lseek(in,  0, SEEK_CUR) == SEEDLEN, "in  cursor at %lld",
          (long long)lseek(in,  0, SEEK_CUR));
    CHECK(lseek(out, 0, SEEK_CUR) == SEEDLEN, "out cursor at %lld",
          (long long)lseek(out, 0, SEEK_CUR));
    verify_data(out, 0, seed, SEEDLEN, "output data");

    /* --- Test 4: exactly one full buffer (4096 bytes) --- */
    printf("Test 4: exactly one buffer (4096 bytes)\n");
    seed_large(in, 4096);
    if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

    off_in = 0; off_out = 0;
    r = copy_file_range(in, &off_in, out, &off_out, 4096, 0);

    CHECK(r == 4096,    "returned %zd bytes", r);
    CHECK(off_in  == 4096, "off_in advanced to %lld",  (long long)off_in);
    CHECK(off_out == 4096, "off_out advanced to %lld", (long long)off_out);
    verify_large(out, 0, 4096, "output data");

    /* --- Test 5: multi-buffer copy (4096*2 + 100 bytes) --- */
    size_t large = 4096 * 2 + 100;
    printf("Test 5: multi-buffer copy (%zu bytes)\n", large);
    seed_large(in, large);
    if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

    off_in = 0; off_out = 0;
    r = copy_file_range(in, &off_in, out, &off_out, large, 0);

    CHECK(r == (ssize_t)large,      "returned %zd bytes", r);
    CHECK(off_in  == (loff_t)large, "off_in advanced to %lld",  (long long)off_in);
    CHECK(off_out == (loff_t)large, "off_out advanced to %lld", (long long)off_out);
    verify_large(out, 0, large, "output data");

    /* --- Tests 6-7: synth fd as fd_in and fd_out --- */
    int (*synth_open)(int, const struct synth_ops *, void *) =
        dlsym(RTLD_DEFAULT, "wslcompat_synth_open");
    if (synth_open == NULL) {
        printf("Tests 6-7: SKIP (libwslcompat not loaded)\n");
    } else {
        int synth;

        printf("Test 6: synth fd as fd_in\n");
        seed_fd(in);
        if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

        synth = synth_open(O_RDONLY, &file_ops, (void *)(intptr_t)in);
        CHECK(synth >= 0, "synth_open -> %d", synth);
        if (synth >= 0) {
            off_in = 0; off_out = 0;
            r = copy_file_range(synth, &off_in, out, &off_out, SEEDLEN, 0);
            CHECK(r == SEEDLEN, "returned %zd bytes", r);
            verify_data(out, 0, seed, SEEDLEN, "output data");
            close(synth);
        }

        printf("Test 7: synth fd as fd_out\n");
        seed_fd(in);
        if (ftruncate(out, 0) != 0) err(EXIT_FAILURE, "ftruncate");

        synth = synth_open(O_WRONLY, &file_ops, (void *)(intptr_t)out);
        CHECK(synth >= 0, "synth_open -> %d", synth);
        if (synth >= 0) {
            off_in = 0; off_out = 0;
            r = copy_file_range(in, &off_in, synth, &off_out, SEEDLEN, 0);
            CHECK(r == SEEDLEN, "returned %zd bytes", r);
            verify_data(out, 0, seed, SEEDLEN, "output data");
            close(synth);
        }
    }

    close(in);
    close(out);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
