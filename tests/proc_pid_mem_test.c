/* Tests for the /proc/pid/mem polyfill. The synthesized fd routes
 * read/write/lseek to process_vm_readv/writev with the file position
 * acting as the target virtual address.
 *
 * Each concern lives in its own run_*() so that future handler
 * coverage (writev, pread, ...) drops in as another run_*() called
 * from main without disturbing the existing checks. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#include "testutil.h"

static void run_open_close(void)
{
    STATUS("--- open/close lifecycle ---");
    char path[64];
    int fd;

    fd = open("/proc/self/mem", O_RDONLY);
    CHECK(fd >= 0, "open(/proc/self/mem, O_RDONLY): %s", strerror(errno));
    if (fd >= 0) close(fd);

    fd = open("/proc/self/mem", O_RDWR);
    CHECK(fd >= 0, "open(/proc/self/mem, O_RDWR): %s", strerror(errno));
    if (fd >= 0) close(fd);

    /* Numeric pid form should resolve to the same backing. */
    snprintf(path, sizeof(path), "/proc/%d/mem", getpid());
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open(%s): %s", path, strerror(errno));
    if (fd >= 0) close(fd);
}

static void run_lseek(void)
{
    STATUS("--- lseek semantics ---");
    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    off_t r = lseek(fd, 0x1000, SEEK_SET);
    CHECK(r == 0x1000, "SEEK_SET 0x1000 -> 0x%llx", (long long)r);

    r = lseek(fd, 0x100, SEEK_CUR);
    CHECK(r == 0x1100, "SEEK_CUR +0x100 -> 0x%llx", (long long)r);

    r = lseek(fd, 0, SEEK_CUR);
    CHECK(r == 0x1100, "SEEK_CUR 0 reports current pos (0x%llx)", (long long)r);

    errno = 0;
    r = lseek(fd, 0, SEEK_END);
    CHECK(r == -1 && errno == EINVAL,
          "SEEK_END rejected (r=%lld errno=%s)", (long long)r, strerror(errno));

    errno = 0;
    r = lseek(fd, -100, SEEK_SET);
    CHECK(r == -1 && errno == EINVAL,
          "SEEK_SET negative rejected (r=%lld errno=%s)", (long long)r, strerror(errno));

    errno = 0;
    r = lseek(fd, 0, 42);
    CHECK(r == -1 && errno == EINVAL,
          "invalid whence rejected (r=%lld errno=%s)", (long long)r, strerror(errno));

    lseek(fd, 0, SEEK_SET);
    errno = 0;
    r = lseek(fd, -1, SEEK_CUR);
    CHECK(r == -1 && errno == EINVAL,
          "SEEK_CUR negative out of bounds rejected (r=%lld errno=%s)", (long long)r, strerror(errno));

    close(fd);
}

static void run_self_read(void)
{
    STATUS("--- self-read via /proc/self/mem ---");
    char marker[16] = "MARKER-1234-5678";
    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    off_t addr = (off_t)(uintptr_t)marker;
    CHECK(lseek(fd, addr, SEEK_SET) == addr, "seek to marker");

    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(marker));
    CHECK(n == (ssize_t)sizeof(marker), "read returned %zd", n);
    CHECK(memcmp(buf, marker, sizeof(marker)) == 0,
          "buffer matches marker ('%.16s')", buf);

    /* Position should auto-advance past the read region. */
    off_t after = lseek(fd, 0, SEEK_CUR);
    CHECK(after == addr + (off_t)sizeof(marker),
          "position advanced by %zu bytes", sizeof(marker));

    close(fd);
}

static void run_self_write(void)
{
    STATUS("--- self-write via /proc/self/mem ---");
    char target[16] = "ORIGINAL-VALUE!";
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    off_t addr = (off_t)(uintptr_t)target;
    CHECK(lseek(fd, addr, SEEK_SET) == addr, "seek to target");

    static const char repl[16] = "REWRITTEN-VAL!!";
    ssize_t n = write(fd, repl, sizeof(repl));
    CHECK(n == (ssize_t)sizeof(repl), "write returned %zd", n);
    CHECK(memcmp(target, repl, sizeof(repl)) == 0,
          "target now '%.16s'", target);

    off_t after = lseek(fd, 0, SEEK_CUR);
    CHECK(after == addr + (off_t)sizeof(repl),
          "position advanced by %zu bytes", sizeof(repl));

    close(fd);
}

static void run_self_pread_pwrite(void)
{
    STATUS("--- self-pread/pwrite via /proc/self/mem ---");
    char target[16] = "PREAD-PWRITE-V!";
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    off_t addr = (off_t)(uintptr_t)target;
    char buf[16] = {0};
    ssize_t n = pread(fd, buf, sizeof(target), addr);
    CHECK(n == (ssize_t)sizeof(target), "pread returned %zd", n);
    CHECK(memcmp(buf, target, sizeof(target)) == 0,
          "pread buffer matches ('%.16s')", buf);

    /* Position should NOT auto-advance. */
    off_t after = lseek(fd, 0, SEEK_CUR);
    CHECK(after == 0, "position did not advance (remains %lld)", (long long)after);

    static const char repl[16] = "PWRITE-MODIFIED";
    n = pwrite(fd, repl, sizeof(repl), addr);
    CHECK(n == (ssize_t)sizeof(repl), "pwrite returned %zd", n);
    CHECK(memcmp(target, repl, sizeof(repl)) == 0,
          "target now '%.16s'", target);

    close(fd);
}

static void run_self_vectored(void)
{
    STATUS("--- self-preadv/pwritev via /proc/self/mem ---");
    char target[32] = "VECTORED-PWRITE-VECTORED-PWRITE";
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    off_t addr = (off_t)(uintptr_t)target;
    char buf1[16] = {0};
    char buf2[16] = {0};
    struct iovec iov_read[2] = {
        { .iov_base = buf1, .iov_len = 16 },
        { .iov_base = buf2, .iov_len = 16 }
    };
    ssize_t n = preadv(fd, iov_read, 2, addr);
    CHECK(n == 32, "preadv returned %zd", n);
    CHECK(memcmp(buf1, target, 16) == 0 && memcmp(buf2, target + 16, 16) == 0,
          "preadv buffer matches ('%.16s%.16s')", buf1, buf2);

    static const char repl1[16] = "MODIFIED-VECTOR1";
    static const char repl2[16] = "MODIFIED-VECTOR2";
    struct iovec iov_write[2] = {
        { .iov_base = (void *)repl1, .iov_len = 16 },
        { .iov_base = (void *)repl2, .iov_len = 16 }
    };
    n = pwritev(fd, iov_write, 2, addr);
    CHECK(n == 32, "pwritev returned %zd", n);
    CHECK(memcmp(target, "MODIFIED-VECTOR1MODIFIED-VECTOR2", 32) == 0,
          "target now '%.32s'", target);

    /* Test readv/writev too */
    lseek(fd, addr, SEEK_SET);
    memset(buf1, 0, 16);
    memset(buf2, 0, 16);
    n = readv(fd, iov_read, 2);
    CHECK(n == 32, "readv returned %zd", n);
    CHECK(memcmp(buf1, "MODIFIED-VECTOR1", 16) == 0 && memcmp(buf2, "MODIFIED-VECTOR2", 16) == 0,
          "readv buffer matches ('%.16s%.16s')", buf1, buf2);

    lseek(fd, addr, SEEK_SET);
    struct iovec iov_write2[2] = {
        { .iov_base = (void *)"FINAL-MOD-VECTO1", .iov_len = 16 },
        { .iov_base = (void *)"FINAL-MOD-VECTO2", .iov_len = 16 }
    };
    n = writev(fd, iov_write2, 2);
    CHECK(n == 32, "writev returned %zd", n);
    CHECK(memcmp(target, "FINAL-MOD-VECTO1FINAL-MOD-VECTO2", 32) == 0,
          "target now '%.32s'", target);

    close(fd);
}

static void run_bad_addr(void)
{
    STATUS("--- read from unmapped address ---");
    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) { CHECK(0, "open: %s", strerror(errno)); return; }

    /* mmap_min_addr keeps the low pages unmapped. Either EFAULT
     * (process_vm_readv) or EIO (kernel /proc/pid/mem) is acceptable
     * -- only the failure matters. */
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek to addr 0");
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf));
    CHECK(n == -1, "read at NULL fails (n=%zd errno=%s)", n, strerror(errno));

    close(fd);
}

static void run_cross_process(void)
{
    STATUS("--- cross-process read/write of a forked child ---");
    int up[2];
    int down[2];
    if (pipe(up) < 0 || pipe(down) < 0) {
        CHECK(0, "pipe: %s", strerror(errno));
        return;
    }

    pid_t child = fork();
    if (child < 0) { CHECK(0, "fork: %s", strerror(errno)); return; }

    if (child == 0) {
        /* Drop the ends we don't use. Without this, if the parent
         * aborts early the child still holds down[1] and its read on
         * down[0] never sees EOF -- parent's waitpid would hang. */
        close(up[0]);
        close(down[1]);

        /* Satisfy yama so the parent's process_vm_{readv,writev} is
         * permitted. EINVAL just means yama isn't loaded -- harmless. */
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

        char target[16] = "CHILD-INITIAL!!";

        uintptr_t ptr = (uintptr_t)target;
        if (write(up[1], &ptr, sizeof(ptr)) != sizeof(ptr)) _exit(2);

        char go;
        if (read(down[0], &go, 1) != 1) _exit(3);

        /* Echo current value so parent can verify the write landed. */
        if (write(up[1], target, sizeof(target)) != sizeof(target)) _exit(4);
        _exit(0);
    }

    close(up[1]);
    close(down[0]);

    uintptr_t child_addr = 0;
    CHECK(read(up[0], &child_addr, sizeof(child_addr)) == sizeof(child_addr),
          "received child target addr (0x%lx)", (unsigned long)child_addr);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", child);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0, "open(%s): %s", path, strerror(errno));
    if (fd < 0) goto cleanup;

    off_t addr = (off_t)child_addr;
    CHECK(lseek(fd, addr, SEEK_SET) == addr, "seek to child target");

    char readbuf[16] = {0};
    ssize_t n = read(fd, readbuf, sizeof(readbuf));
    CHECK(n == 16, "read 16 from child (n=%zd errno=%s)", n, strerror(errno));
    CHECK(memcmp(readbuf, "CHILD-INITIAL!!", 16) == 0,
          "child memory matches initial value ('%.16s')", readbuf);

    CHECK(lseek(fd, addr, SEEK_SET) == addr, "seek back for write");
    static const char poke[16] = "CHILD-MODIFIED!";
    n = write(fd, poke, sizeof(poke));
    CHECK(n == 16, "write 16 into child (n=%zd errno=%s)", n, strerror(errno));

    close(fd);

    /* Tell the child to echo back its (now-modified) target. */
    write(down[1], "g", 1);

    char echo[16] = {0};
    CHECK(read(up[0], echo, sizeof(echo)) == 16, "received echo from child");
    CHECK(memcmp(echo, poke, sizeof(poke)) == 0,
          "child observed write ('%.16s')", echo);

cleanup:
    close(up[0]);
    close(down[1]);
    int status;
    waitpid(child, &status, 0);
}

/* The per-thread paths /proc/<pid>/task/<tid>/mem (and self/task/<tid>/
 * mem) address the same VM as /proc/<pid>/mem -- all threads in a
 * process share an mm. Verify the polyfill accepts the path shape
 * and self-reads return the marker just like the bare /mem form. */
static void run_task_form(void)
{
    STATUS("--- per-thread /proc/<pid>/task/<tid>/mem ---");
    char marker[16] = "TASK-MARKER-AB!";
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/proc/self/task/%d/mem", tid);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open(%s): %s", path, strerror(errno));
    if (fd < 0) return;

    off_t addr = (off_t)(uintptr_t)marker;
    CHECK(lseek(fd, addr, SEEK_SET) == addr, "seek to marker");
    char buf[16] = {0};
    CHECK(read(fd, buf, sizeof(marker)) == (ssize_t)sizeof(marker),
          "read returned marker bytes");
    CHECK(memcmp(buf, marker, sizeof(marker)) == 0,
          "buffer matches ('%.16s')", buf);
    close(fd);

    /* Numeric pid + tid form should resolve to the same backing. */
    snprintf(path, sizeof(path), "/proc/%d/task/%d/mem", getpid(), tid);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open(%s): %s", path, strerror(errno));
    if (fd >= 0) close(fd);
}

static void run_bogus_pid(void)
{
    STATUS("--- nonexistent pid ---");
    /* Well above /proc/sys/kernel/pid_max on stock kernels. */
    int fd = open("/proc/2147480000/mem", O_RDONLY);
    if (fd < 0) {
        /* Native Linux without shim: the pid dir doesn't exist. */
        CHECK(errno == ENOENT, "open fails (errno=%s)", strerror(errno));
        return;
    }

    /* Shim path: open synthesizes the fd unconditionally; the failure
     * surfaces on the actual read against the nonexistent process. */
    CHECK(lseek(fd, 0x1000, SEEK_SET) == 0x1000, "seek bogus pid");
    char buf[4];
    ssize_t n = read(fd, buf, sizeof(buf));
    CHECK(n == -1 && errno == ESRCH,
          "read on bogus pid -> ESRCH (n=%zd errno=%s)", n, strerror(errno));
    close(fd);
}

/* yama (Documentation/admin-guide/LSM/Yama.rst) gates the cross-
 * process process_vm_readv/writev paths this test depends on:
 *   0 = classic permissions (anything goes within uid)
 *   1 = restricted: only descendants, unless PR_SET_PTRACER
 *   2 = admin-only
 *   3 = no attach
 * The self-mode subtests don't need ptrace, but skipping the whole
 * file keeps the CI signal clean -- a partial pass under restrictive
 * yama would mask real regressions in the cross-process polyfill.
 * /proc/sys/kernel/yama/ptrace_scope missing means yama isn't loaded,
 * which is equivalent to scope 0. */
static int yama_blocks_ptrace(void)
{
    FILE *f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
    int v;
    int matched;

    if (f == NULL) return 0;
    matched = fscanf(f, "%d", &v);
    fclose(f);
    if (matched != 1) return 0;
    return v != 0;
}

int main(void)
{
    if (yama_blocks_ptrace()) {
        printf("SKIP: kernel.yama.ptrace_scope != 0\n");
        return 0;
    }

    /* Watchdog: the cross-process subtest uses pipe sync + fork, and a
     * future change that leaves a descriptor open in the wrong process
     * could deadlock waitpid. Bound the whole run so a regression
     * shows up as a failure rather than a hung test. */
    alarm(5);

    run_open_close();
    run_lseek();
    run_self_read();
    run_self_write();
    run_self_pread_pwrite();
    run_self_vectored();
    run_bad_addr();
    run_cross_process();
    run_task_form();
    run_bogus_pid();

    TEST_REPORT();
}
