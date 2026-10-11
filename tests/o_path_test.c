/* Verify O_PATH works on WSL1.
 *
 * O_PATH (Linux 2.6.39+) opens a file as a "path reference" -- no
 * actual data fd, just an identifier you can use with the *at family
 * (openat, fstatat, unlinkat, etc.). Requires only search permission
 * on the path components, not read or write on the target itself.
 *
 * Useful if it works: lets the O_TMPFILE polyfill hold a dirfd to the
 * target directory without requiring read permission on it.
 *
 * Also exercises the canary-error dispatch idea: an O_PATH fd handed
 * back from open() makes every I/O op fail with EBADF, which the shim
 * can catch on the error path, fstat-confirm against a table of our
 * synthetic inodes, and then dispatch to a handler. This file
 * enumerates every op we might want to intercept so we know up front
 * which ones the trick covers and which ones leak past it. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <sys/statfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sched.h>
#include <signal.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/select.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>

#include "testutil.h"

static void test_multiplex(int pfd)
{
    STATUS("--- O_PATH multiplexing (poll/select/epoll) ---");

    /* Test poll(): WSL1 returns POLLIN, native Linux returns POLLNVAL. */
    struct pollfd pfd_poll = { .fd = pfd, .events = POLLIN };
    int pr = poll(&pfd_poll, 1, 0);
    CHECK(pr == 1 && (pfd_poll.revents & (POLLIN | POLLNVAL)),
          "poll(O_PATH) -> rc=%d revents=0x%x (POLLIN/POLLNVAL ok)",
          pr, pfd_poll.revents);

    /* Test select(): WSL1 returns EPERM, native Linux returns EBADF
     * or success (if treated as ready). */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(pfd, &rfds);
    struct timeval tv = {0, 0};
    errno = 0;
    int sr = select(pfd + 1, &rfds, NULL, NULL, &tv);
    CHECK((sr == -1 && (errno == EBADF || errno == EPERM)) || (sr == 1),
          "select(O_PATH) -> rc=%d errno=%s (Ready/EBADF/EPERM ok)",
          sr, sr == -1 ? strerror(errno) : "Ready");

    /* Test epoll: both return EPERM or EBADF. */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd >= 0) {
        struct epoll_event ev = { .events = POLLIN, .data.fd = pfd };
        errno = 0;
        int er = epoll_ctl(epfd, EPOLL_CTL_ADD, pfd, &ev);
        CHECK(er == -1 && (errno == EPERM || errno == EBADF),
              "epoll_ctl(ADD, O_PATH) -> rc=%d errno=%s (EPERM/EBADF ok)",
              er, strerror(errno));
        close(epfd);
    }
}

static void test_opath_directory(void)
{
    /* Can we open a directory with O_PATH at all? */
    int dirfd = open("/tmp", O_PATH | O_DIRECTORY);
    if (dirfd < 0) {
        CHECK(0, "open(/tmp, O_PATH | O_DIRECTORY): %s", strerror(errno));
        return;
    }
    CHECK(dirfd >= 0, "open(O_PATH | O_DIRECTORY) returned fd=%d", dirfd);

    /* Use it as a dirfd to create a file relative to it. This is the
     * exact pattern the O_TMPFILE polyfill needs. */
    char name[64];
    snprintf(name, sizeof(name), ".wslcompat-opath-%d", (int)getpid());
    int fd = openat(dirfd, name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        CHECK(0, "openat(O_PATH dirfd, ...): %s", strerror(errno));
        close(dirfd);
        return;
    }
    CHECK(fd >= 0, "openat(O_PATH dirfd, ...) returned fd=%d", fd);

    const char *payload = "ok";
    ssize_t w = write(fd, payload, strlen(payload));
    CHECK(w == (ssize_t)strlen(payload),
          "write through new fd returned %zd (want %zu)", w, strlen(payload));

    /* fstatat through the O_PATH dirfd. */
    struct stat st;
    int sr = fstatat(dirfd, name, &st, 0);
    CHECK(sr == 0 && st.st_size == w,
          "fstatat via O_PATH dirfd -> rc=%d st_size=%lld",
          sr, (long long)st.st_size);

    /* unlinkat through the O_PATH dirfd -- the cleanup path the
     * O_TMPFILE polyfill uses. */
    int ur = unlinkat(dirfd, name, 0);
    CHECK(ur == 0, "unlinkat via O_PATH dirfd -> rc=%d (errno=%s)",
          ur, ur ? strerror(errno) : "ok");

    close(fd);

    /* Confirm O_PATH is actually doing its job and not being silently
     * ignored. Per man open(2): read/write/fchmod/etc. on an O_PATH
     * fd return EBADF. A real O_RDONLY fd on a directory would return
     * EISDIR for read, not EBADF. */
    char ch;
    errno = 0;
    ssize_t rr = read(dirfd, &ch, 1);
    CHECK(rr == -1 && errno == EBADF,
          "read(O_PATH dirfd) -> rc=%zd errno=%s (want -1/EBADF)",
          rr, strerror(errno));

    close(dirfd);
}

static void test_opath_readless_dir(void)
{
    /* The other half: a directory we have search+write but no read on
     * should be openable with O_PATH but not with O_RDONLY. */
    char dir_template[] = "/tmp/wslcompat_opath_XXXXXX";
    if (!mkdtemp(dir_template)) {
        CHECK(0, "mkdtemp: %s", strerror(errno));
        return;
    }
    if (chmod(dir_template, 0300) != 0) {
        CHECK(0, "chmod 0300: %s", strerror(errno));
        rmdir(dir_template);
        return;
    }

    int rdfd = open(dir_template, O_RDONLY | O_DIRECTORY);
    int rd_err = errno;
    CHECK(rdfd == -1 && rd_err == EACCES,
          "O_RDONLY on read-less dir -> rc=%d errno=%s (want -1/EACCES)",
          rdfd, strerror(rd_err));
    if (rdfd >= 0) close(rdfd);

    int pathfd = open(dir_template, O_PATH | O_DIRECTORY);
    CHECK(pathfd >= 0,
          "O_PATH on read-less dir -> fd=%d (errno=%s)",
          pathfd, pathfd < 0 ? strerror(errno) : "ok");
    if (pathfd >= 0) close(pathfd);

    chmod(dir_template, 0700);
    rmdir(dir_template);
}

static void test_opath_regular_file(void)
{
    /* ============================================================
     * O_PATH on a regular file: the case that matters for the
     * canary-error dispatch trick. Every I/O op should return EBADF
     * so the shim can detect a synthetic fd via errno, and fstat /
     * fcntl / dup must work so the shim can confirm and dispatch.
     * ============================================================ */
    STATUS("--- O_PATH on a regular file ---");

    char file_template[] = "/tmp/wslcompat_opath_file_XXXXXX";
    int seedfd = mkstemp(file_template);
    if (seedfd < 0) {
        CHECK(0, "mkstemp: %s", strerror(errno));
        return;
    }

    const char *content = "hello\n";
    const size_t clen = 6;
    ssize_t cw = write(seedfd, content, clen);
    close(seedfd);
    if (cw != (ssize_t)clen) {
        CHECK(0, "seed write -> %zd", cw);
        unlink(file_template);
        return;
    }

    int pfd = open(file_template, O_PATH);
    CHECK(pfd >= 0,
          "open(O_PATH) on regular file -> fd=%d (errno=%s)",
          pfd, pfd < 0 ? strerror(errno) : "ok");

    if (pfd >= 0) {
        test_multiplex(pfd);

        char b[16] = {0};
        struct iovec iov = { .iov_base = b, .iov_len = sizeof(b) };
        ssize_t r;
        int ir;

        /* --- I/O operations that MUST return EBADF.
         * These are the ops the shim wants to intercept; the kernel's
         * distinctive errno is what makes the trick work. --- */

        errno = 0; r = read(pfd, b, sizeof(b));
        CHECK(r == -1 && errno == EBADF,
              "read -> rc=%zd errno=%s (want -1/EBADF)",
              r, strerror(errno));

        errno = 0; r = write(pfd, "x", 1);
        CHECK(r == -1 && errno == EBADF,
              "write -> rc=%zd errno=%s (want -1/EBADF)",
              r, strerror(errno));

        errno = 0; r = pread(pfd, b, sizeof(b), 0);
        CHECK(r == -1 && errno == EBADF,
              "pread -> rc=%zd errno=%s (want -1/EBADF)",
              r, strerror(errno));

        errno = 0; r = pwrite(pfd, "x", 1, 0);
        CHECK(r == -1 && errno == EBADF,
              "pwrite -> rc=%zd errno=%s (want -1/EBADF)",
              r, strerror(errno));

        /* INTERCEPT GAP: on WSL1, readv/writev on an O_PATH fd return
         * EPERM, not EBADF. The canary-error trick can't distinguish
         * this from a real EPERM, so we cannot intercept these via the
         * usual errno-check fast path. Native Linux returns EBADF per
         * the manpage -- accept both so the test passes on both. */
        errno = 0; r = readv(pfd, &iov, 1);
        CHECK(r == -1 && (errno == EBADF || errno == EPERM),
              "readv -> rc=%zd errno=%s (EPERM on WSL1 = INTERCEPT GAP)",
              r, strerror(errno));

        errno = 0; r = writev(pfd, &iov, 1);
        CHECK(r == -1 && (errno == EBADF || errno == EPERM),
              "writev -> rc=%zd errno=%s (EPERM on WSL1 = INTERCEPT GAP)",
              r, strerror(errno));

        errno = 0; r = preadv(pfd, &iov, 1, 0);
        CHECK(r == -1 && (errno == EBADF || errno == EPERM),
              "preadv -> rc=%zd errno=%s (EPERM on WSL1 = INTERCEPT GAP)",
              r, strerror(errno));

        errno = 0; r = pwritev(pfd, &iov, 1, 0);
        CHECK(r == -1 && (errno == EBADF || errno == EPERM),
              "pwritev -> rc=%zd errno=%s (EPERM on WSL1 = INTERCEPT GAP)",
              r, strerror(errno));

        errno = 0;
        off_t off = lseek(pfd, 0, SEEK_SET);
        CHECK(off == (off_t)-1 && errno == EBADF,
              "lseek -> rc=%lld errno=%s (want -1/EBADF)",
              (long long)off, strerror(errno));

        errno = 0;
        void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, pfd, 0);
        int mmap_err = errno;
        CHECK(m == MAP_FAILED && mmap_err == EBADF,
              "mmap(PROT_READ, MAP_PRIVATE) -> %s errno=%s (want MAP_FAILED/EBADF)",
              m == MAP_FAILED ? "MAP_FAILED" : "mapped", strerror(mmap_err));
        if (m != MAP_FAILED) munmap(m, 4096);

        /* TCGETS on a regular file would normally fail with ENOTTY.
         * With O_PATH the kernel must reject before getting that far. */
        struct termios t;
        errno = 0; ir = ioctl(pfd, TCGETS, &t);
        CHECK(ir == -1 && errno == EBADF,
              "ioctl(TCGETS) -> rc=%d errno=%s (want -1/EBADF, not ENOTTY)",
              ir, strerror(errno));

        /* INTERCEPT GAP: WSL1 returns EINVAL on O_PATH ftruncate.
         * EINVAL is too generic to safely hijack. */
        errno = 0; ir = ftruncate(pfd, 0);
        CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
              "ftruncate -> rc=%d errno=%s (EINVAL on WSL1 = INTERCEPT GAP)",
              ir, strerror(errno));

        errno = 0; ir = fsync(pfd);
        CHECK(ir == -1 && errno == EBADF,
              "fsync -> rc=%d errno=%s (want -1/EBADF)",
              ir, strerror(errno));

        errno = 0; ir = fdatasync(pfd);
        CHECK(ir == -1 && errno == EBADF,
              "fdatasync -> rc=%d errno=%s (want -1/EBADF)",
              ir, strerror(errno));

        /* INTERCEPT GAP: WSL1 silently returns 0 from syncfs on an
         * O_PATH fd. No canary -> the shim cannot detect the call.
         * Native Linux returns EBADF. */
        errno = 0; ir = syncfs(pfd);
        CHECK(ir == 0 || (ir == -1 && errno == EBADF),
              "syncfs -> rc=%d errno=%s (SILENT success on WSL1 = INTERCEPT GAP)",
              ir, strerror(errno));

        /* sync_file_range is in WSL1's syscall list but unimplemented:
         * always ENOSYS regardless of fd type. Test as a regular
         * unsupported syscall, not an O_PATH canary. */
        errno = 0; ir = sync_file_range(pfd, 0, 0, 0);
        CHECK(ir == -1 && (errno == EBADF || errno == ENOSYS),
              "sync_file_range -> rc=%d errno=%s (ENOSYS on WSL1)",
              ir, strerror(errno));

        /* INTERCEPT GAP: WSL1 returns SUCCESS for flock on an O_PATH
         * fd -- the kernel silently no-ops. Apps relying on flock as
         * a mutex would think they hold a lock they don't. Not
         * relevant to /proc-style synthetic files but a sharp edge
         * worth knowing. Native Linux returns EBADF. */
        errno = 0; ir = flock(pfd, LOCK_EX | LOCK_NB);
        CHECK(ir == 0 || (ir == -1 && errno == EBADF),
              "flock(LOCK_EX|LOCK_NB) -> rc=%d errno=%s (SILENT no-op on WSL1)",
              ir, strerror(errno));

        errno = 0; ir = fchmod(pfd, 0644);
        CHECK(ir == -1 && errno == EBADF,
              "fchmod -> rc=%d errno=%s (want -1/EBADF)",
              ir, strerror(errno));

        errno = 0; ir = fchown(pfd, (uid_t)-1, (gid_t)-1);
        CHECK(ir == -1 && errno == EBADF,
              "fchown -> rc=%d errno=%s (want -1/EBADF)",
              ir, strerror(errno));

        /* INTERCEPT GAP: WSL1 returns EOPNOTSUPP for fallocate on an
         * O_PATH-wrapped regular file. We use that as our canary, but
         * it's also the legitimate error from a real fs that doesn't
         * support the requested fallocate mode -- so a careful caller
         * could be misled if they probe modes. */
        errno = 0; ir = fallocate(pfd, 0, 0, 4096);
        CHECK(ir == -1 && (errno == EBADF || errno == EOPNOTSUPP),
              "fallocate -> rc=%d errno=%s (EOPNOTSUPP on WSL1)",
              ir, strerror(errno));

        /* On an O_PATH fd, fgetxattr must return EBADF, not ENODATA. */
        errno = 0;
        ssize_t gxr = fgetxattr(pfd, "user.wslcompat.nope", b, sizeof(b));
        CHECK(gxr == -1 && errno == EBADF,
              "fgetxattr -> rc=%zd errno=%s (want -1/EBADF, not ENODATA)",
              gxr, strerror(errno));

        errno = 0; ir = fsetxattr(pfd, "user.x", "v", 1, 0);
        CHECK(ir == -1 && errno == EBADF,
              "fsetxattr -> rc=%d errno=%s (want -1/EBADF)",
              ir, strerror(errno));

        errno = 0;
        ssize_t lxr = flistxattr(pfd, b, sizeof(b));
        CHECK(lxr == -1 && errno == EBADF,
              "flistxattr -> rc=%zd errno=%s (want -1/EBADF)",
              lxr, strerror(errno));

        /* INTERCEPT GAP: WSL1 returns ENODATA for fremovexattr on
         * O_PATH, indistinguishable from "attribute not present" on a
         * real fd. The canary trick can't catch this. */
        errno = 0; ir = fremovexattr(pfd, "user.wslcompat.nope");
        CHECK(ir == -1 && (errno == EBADF || errno == ENODATA),
              "fremovexattr -> rc=%d errno=%s "
              "(ENODATA on WSL1 = INTERCEPT GAP)",
              ir, strerror(errno));

        /* INTERCEPT GAP: WSL1 returns ENOTDIR for fchdir on an O_PATH
         * regular-file fd, indistinguishable from a wrong-type fd. We
         * never wrap directories today, so this is moot, but it'd
         * block directory-synth support if we ever add it. */
        errno = 0; ir = fchdir(pfd);
        CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
              "fchdir -> rc=%d errno=%s (ENOTDIR on WSL1 = INTERCEPT GAP)",
              ir, strerror(errno));

        errno = 0;
        long dr = syscall(SYS_getdents64, pfd, b, sizeof(b));
        CHECK(dr == -1 && (errno == EBADF || errno == ENOTDIR),
              "getdents64 -> rc=%ld errno=%s (want EBADF/ENOTDIR)",
              dr, strerror(errno));

        /* INTERCEPT GAP: fstatfs on O_PATH silently succeeds, returning
         * stats of the backing filesystem. No canary to hijack. */
        struct statfs sfs;
        errno = 0; ir = fstatfs(pfd, &sfs);
        CHECK(ir == 0 || (ir == -1 && errno == EBADF),
              "fstatfs -> rc=%d errno=%s (SILENT success on WSL1 = INTERCEPT GAP)",
              ir, strerror(errno));

        /* ============================================================
         * Socket-family ops on a non-socket fd. We don't synthesize
         * sockets today, but if we ever do, this is the canary
         * surface. Every op below should uniformly return ENOTSOCK
         * (the kernel's "this fd isn't a socket" signal) -- a clean,
         * unambiguous canary for SYNTH_SHIM_E(call, ENOTSOCK, ...).
         * Tests accept EBADF as well so they still pass on a kernel
         * that does the per-spec O_PATH check first.
         * ============================================================ */
        STATUS("--- Socket family on O_PATH (future synth socket canary) ---");
        {
            struct sockaddr_un sa = { .sun_family = AF_UNIX };
            strcpy(sa.sun_path, "/tmp/wslcompat-no-such");
            socklen_t sl = sizeof(sa);
            struct iovec siov = { .iov_base = b, .iov_len = sizeof(b) };
            struct msghdr mh = { .msg_iov = &siov, .msg_iovlen = 1 };
            int sov;

            errno = 0; ir = accept(pfd, NULL, NULL);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "accept -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = accept4(pfd, NULL, NULL, SOCK_CLOEXEC);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "accept4 -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = bind(pfd, (struct sockaddr *)&sa, sizeof(sa));
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "bind -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = connect(pfd, (struct sockaddr *)&sa, sizeof(sa));
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "connect -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = listen(pfd, 5);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "listen -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = shutdown(pfd, SHUT_RDWR);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "shutdown -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = getsockname(pfd, (struct sockaddr *)&sa, &sl);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "getsockname -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = getpeername(pfd, (struct sockaddr *)&sa, &sl);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "getpeername -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            sl = sizeof(sov);
            errno = 0; ir = getsockopt(pfd, SOL_SOCKET, SO_TYPE, &sov, &sl);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "getsockopt -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            sov = 1;
            errno = 0; ir = setsockopt(pfd, SOL_SOCKET, SO_REUSEADDR,
                                       &sov, sizeof(sov));
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "setsockopt -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            ssize_t sr;
            errno = 0; sr = recvfrom(pfd, b, sizeof(b), 0, NULL, NULL);
            CHECK(sr == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "recvfrom -> rc=%zd errno=%s (ENOTSOCK = canary)",
                  sr, strerror(errno));

            errno = 0; sr = sendto(pfd, b, 1, 0, NULL, 0);
            CHECK(sr == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "sendto -> rc=%zd errno=%s (ENOTSOCK = canary)",
                  sr, strerror(errno));

            errno = 0; sr = recvmsg(pfd, &mh, 0);
            CHECK(sr == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "recvmsg -> rc=%zd errno=%s (ENOTSOCK = canary)",
                  sr, strerror(errno));

            errno = 0; sr = sendmsg(pfd, &mh, 0);
            CHECK(sr == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "sendmsg -> rc=%zd errno=%s (ENOTSOCK = canary)",
                  sr, strerror(errno));

            struct mmsghdr mmh = { .msg_hdr = mh };
            errno = 0; ir = recvmmsg(pfd, &mmh, 1, 0, NULL);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "recvmmsg -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));

            errno = 0; ir = sendmmsg(pfd, &mmh, 1, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTSOCK),
                  "sendmmsg -> rc=%d errno=%s (ENOTSOCK = canary)",
                  ir, strerror(errno));
        }

        /* ============================================================
         * *at family on a regular-file O_PATH dirfd. All return
         * ENOTDIR uniformly on WSL1 (the "dirfd isn't a directory"
         * error). Same caveat as fchdir: ENOTDIR is a legitimate
         * error from real code, so this can't be cleanly intercepted
         * -- not relevant today since we don't wrap directories.
         * ============================================================ */
        STATUS("--- *at family on regular-file O_PATH (ENOTDIR = INTERCEPT GAP) ---");
        {
            struct stat ast;
            errno = 0; ir = openat(pfd, "x", O_RDONLY);
            if (ir >= 0) close(ir);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "openat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = fstatat(pfd, "x", &ast, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "fstatat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = faccessat(pfd, "x", F_OK, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "faccessat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = unlinkat(pfd, "x", 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "unlinkat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = linkat(pfd, "x", pfd, "y", 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "linkat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = mkdirat(pfd, "x", 0755);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "mkdirat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = symlinkat("z", pfd, "x");
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "symlinkat -> rc=%d errno=%s", ir, strerror(errno));

            ssize_t rl;
            errno = 0; rl = readlinkat(pfd, "x", b, sizeof(b));
            CHECK(rl == -1 && (errno == EBADF || errno == ENOTDIR),
                  "readlinkat -> rc=%zd errno=%s", rl, strerror(errno));

            errno = 0; ir = fchmodat(pfd, "x", 0600, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "fchmodat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = fchownat(pfd, "x", (uid_t)-1, (gid_t)-1, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "fchownat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = utimensat(pfd, "x", NULL, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "utimensat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = renameat(pfd, "x", pfd, "y");
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "renameat -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = mknodat(pfd, "x", S_IFREG | 0600, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == ENOTDIR),
                  "mknodat -> rc=%d errno=%s", ir, strerror(errno));
        }

        /* ============================================================
         * Fd-typed ops on a non-matching fd type. timerfd/signalfd/
         * inotify_add_watch all return EINVAL on a "wrong kind" of fd
         * -- another INTERCEPT GAP since EINVAL is too generic to use
         * as a canary. Not relevant today since we don't synthesize
         * any of these fd types.
         * ============================================================ */
        STATUS("--- Fd-typed ops on O_PATH (EINVAL = INTERCEPT GAP) ---");
        {
            struct itimerspec its;
            errno = 0; ir = timerfd_gettime(pfd, &its);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "timerfd_gettime -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = inotify_add_watch(pfd, "/tmp", IN_CLOSE);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "inotify_add_watch -> rc=%d errno=%s", ir, strerror(errno));

            errno = 0; ir = inotify_rm_watch(pfd, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "inotify_rm_watch -> rc=%d errno=%s", ir, strerror(errno));

            struct itimerspec its2 = {{0,0},{0,0}};
            errno = 0; ir = timerfd_settime(pfd, 0, &its2, NULL);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "timerfd_settime -> rc=%d errno=%s", ir, strerror(errno));

            sigset_t sfmask;
            sigemptyset(&sfmask);
            errno = 0; ir = signalfd(pfd, &sfmask, 0);
            if (ir >= 0) close(ir);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "signalfd -> rc=%d errno=%s", ir, strerror(errno));

            // setns: we don't actually want to switch namespaces here;
            // EBADF/EINVAL means the kernel rejected the fd before
            // doing anything. CLONE_NEWNS arg is irrelevant in that
            // path.
            errno = 0; ir = setns(pfd, 0);
            CHECK(ir == -1 && (errno == EBADF || errno == EINVAL),
                  "setns -> rc=%d errno=%s", ir, strerror(errno));
        }

        /* ============================================================
         * Two-fd data movers (sendfile/splice/tee). The kernel takes
         * one input and one output fd; either being an O_PATH is
         * expected to fail. The canary here is interesting because it
         * tells us whether we could route sendfile/splice/tee through
         * the synth machinery for the non-O_PATH fd.
         * ============================================================ */
        STATUS("--- Two-fd data movers on O_PATH ---");
        {
            int devnull_r = open("/dev/null", O_RDONLY);
            int devnull_w = open("/dev/null", O_WRONLY);

            off_t soff = 0;
            errno = 0; ssize_t sf = sendfile(devnull_w, pfd, &soff, 1);
            CHECK(sf == -1 && (errno == EBADF || errno == EINVAL),
                  "sendfile(out=real, in=O_PATH) -> rc=%zd errno=%s",
                  sf, strerror(errno));

            soff = 0;
            errno = 0; sf = sendfile(pfd, devnull_r, &soff, 1);
            CHECK(sf == -1 && (errno == EBADF || errno == EINVAL),
                  "sendfile(out=O_PATH, in=real) -> rc=%zd errno=%s",
                  sf, strerror(errno));

            int p1[2], p2[2];
            if (pipe(p1) == 0 && pipe(p2) == 0) {
                // Pre-fill the input pipe so the read side doesn't
                // EAGAIN before the kernel ever checks the O_PATH
                // out fd. SPLICE_F_NONBLOCK as belt-and-suspenders
                // in case the kernel falls through to the
                // data-mover.
                //
                // splice on O_PATH returns EPERM on WSL1 (same canary
                // as poll/select). Distinguishable from EBADF, but
                // EPERM is a plausible kernel error for unprivileged
                // splice() ops, so this is an INTERCEPT GAP.
                if (write(p1[1], "x", 1) != 1)
                    err(EXIT_FAILURE, "p1 prime");
                errno = 0; sf = splice(pfd, NULL, p1[1], NULL, 1, SPLICE_F_NONBLOCK);
                CHECK(sf == -1 && (errno == EBADF || errno == EINVAL || errno == EPERM),
                      "splice(in=O_PATH, out=pipe) -> rc=%zd errno=%s",
                      sf, strerror(errno));

                errno = 0; sf = splice(p1[0], NULL, pfd, NULL, 1, SPLICE_F_NONBLOCK);
                CHECK(sf == -1 && (errno == EBADF || errno == EINVAL || errno == EPERM),
                      "splice(in=pipe, out=O_PATH) -> rc=%zd errno=%s",
                      sf, strerror(errno));

                errno = 0; sf = tee(pfd, p2[1], 1, SPLICE_F_NONBLOCK);
                CHECK(sf == -1 && (errno == EBADF || errno == EINVAL || errno == EPERM),
                      "tee(in=O_PATH, out=pipe) -> rc=%zd errno=%s",
                      sf, strerror(errno));

                errno = 0; sf = tee(p1[0], pfd, 1, SPLICE_F_NONBLOCK);
                CHECK(sf == -1 && (errno == EBADF || errno == EINVAL || errno == EPERM),
                      "tee(in=pipe, out=O_PATH) -> rc=%zd errno=%s",
                      sf, strerror(errno));

                close(p1[0]); close(p1[1]);
                close(p2[0]); close(p2[1]);
            }

            close(devnull_r);
            close(devnull_w);
        }

        /* --- Operations that MUST work, so the shim can confirm
         * and dispatch. --- */

        struct stat fst;
        errno = 0; ir = fstat(pfd, &fst);
        CHECK(ir == 0 && S_ISREG(fst.st_mode) && fst.st_size == (off_t)clen,
              "fstat -> rc=%d mode=0%o size=%lld (want 0/regular/%zu)",
              ir, fst.st_mode & 07777, (long long)fst.st_size, clen);

        int fd_flags = fcntl(pfd, F_GETFD);
        CHECK(fd_flags >= 0,
              "fcntl(F_GETFD) -> %d", fd_flags);

        ir = fcntl(pfd, F_SETFD, FD_CLOEXEC);
        CHECK(ir == 0, "fcntl(F_SETFD, FD_CLOEXEC) -> rc=%d", ir);

        /* Caveat: F_GETFL on an O_PATH fd reports the O_PATH bit. A
         * paranoid app that inspects this would see we lied about the
         * file's mode. Static-content polyfills should stay on the
         * prewritten-O_TMPFILE path instead. */
        int fl_flags = fcntl(pfd, F_GETFL);
        CHECK(fl_flags >= 0 && (fl_flags & O_PATH),
              "fcntl(F_GETFL) -> 0x%x (O_PATH bit visible -- caveat)",
              fl_flags);

        /* ----- fcntl sweep on O_PATH: baseline of what's allowed /
         * denied / quirky. Per fcntl(2), only F_DUPFD,
         * F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL are permitted on
         * an O_PATH fd; everything else should EBADF. WSL1 diverges
         * for F_SETFL -- the main quirk being documented here. ----- */
        STATUS("--- O_PATH fcntl behavior sweep ---");

        /* F_DUPFD: allowed, returns another O_PATH fd. */
        int dupbase = fcntl(pfd, F_DUPFD, 3);
        CHECK(dupbase >= 0,
              "fcntl(F_DUPFD, 3) -> fd=%d (errno=%s)",
              dupbase, dupbase < 0 ? strerror(errno) : "ok");
        if (dupbase >= 0) {
            /* The dup must inherit O_PATH OFD semantics. */
            errno = 0;
            ssize_t dr = read(dupbase, b, sizeof(b));
            CHECK(dr == -1 && errno == EBADF,
                  "  read on F_DUPFD result -> EBADF (O_PATH inherited)");
            close(dupbase);
        }

        /* F_DUPFD_CLOEXEC: allowed; new fd has FD_CLOEXEC set. */
        int dupcl = fcntl(pfd, F_DUPFD_CLOEXEC, 3);
        CHECK(dupcl >= 0,
              "fcntl(F_DUPFD_CLOEXEC, 3) -> fd=%d (errno=%s)",
              dupcl, dupcl < 0 ? strerror(errno) : "ok");
        if (dupcl >= 0) {
            int got = fcntl(dupcl, F_GETFD);
            CHECK(got >= 0 && (got & FD_CLOEXEC),
                  "  dup'd fd has FD_CLOEXEC set (F_GETFD=0x%x)", got);
            close(dupcl);
        }

        /* INTERCEPT GAP: F_SETFL on O_PATH. Per fcntl(2) this should
         * EBADF; WSL1 silently succeeds AND strips the O_PATH bit
         * from subsequent F_GETFL reporting. The fd remains O_PATH
         * for I/O purposes either way (read still EBADF below). */
        errno = 0;
        int sf = fcntl(pfd, F_SETFL, O_NONBLOCK);
        int sf_err = errno;
        CHECK((sf == -1 && sf_err == EBADF) || sf == 0,
              "F_SETFL(O_NONBLOCK) on O_PATH -> rc=%d errno=%s "
              "(EBADF per spec, silent success on WSL1)",
              sf, strerror(sf_err));

        int after = fcntl(pfd, F_GETFL);
        if (sf == 0) {
            /* WSL1 stomp: O_PATH bit gone from F_GETFL even though
             * the fd is still O_PATH-equivalent for I/O. */
            CHECK(after >= 0 && !(after & O_PATH),
                  "  F_GETFL after WSL1 F_SETFL -> 0x%x (O_PATH STRIPPED "
                  "from report; canary based on F_GETFL is one-shot)",
                  after);

            /* Try to put O_PATH back via F_SETFL -- WSL1 filters it
             * out of the input flags, so it stays stripped. */
            fcntl(pfd, F_SETFL, O_NONBLOCK | O_PATH);
            int retry = fcntl(pfd, F_GETFL);
            CHECK(retry >= 0 && !(retry & O_PATH),
                  "  F_SETFL(... | O_PATH) doesn't restore -> 0x%x "
                  "(WSL1 filters O_PATH from F_SETFL input)", retry);
        } else {
            CHECK(after >= 0 && (after & O_PATH),
                  "  F_GETFL unchanged after EBADF F_SETFL -> 0x%x",
                  after);
        }

        /* Regardless of the stomp, the fd MUST still be O_PATH for
         * I/O: this is what makes the canary-error dispatch trick
         * safe (read on a wrapped fd still returns EBADF). */
        errno = 0;
        ssize_t rr2 = read(pfd, b, sizeof(b));
        CHECK(rr2 == -1 && errno == EBADF,
              "read after F_SETFL -> EBADF (O_PATH I/O semantics survive)");

        /* INTERCEPT GAP: F_GETOWN should EBADF per spec; WSL1 quietly
         * returns 0 (no owner set). Apps that probe ownership get a
         * misleading "no owner" answer instead of a clean error. */
        errno = 0;
        int own = fcntl(pfd, F_GETOWN);
        CHECK((own == -1 && errno == EBADF) || own == 0,
              "F_GETOWN on O_PATH -> rc=%d errno=%s "
              "(EBADF per spec, 0 on WSL1)", own, strerror(errno));

        /* INTERCEPT GAP: F_GETLK on O_PATH should EBADF; WSL1 succeeds.
         * The shim wraps F_GETLK separately via flock_status_query so
         * this caveat doesn't bite our lock semantics, but raw apps
         * would see a misleading lock state. */
        struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
        errno = 0;
        int gl = fcntl(pfd, F_GETLK, &fl);
        CHECK((gl == -1 && errno == EBADF) || gl == 0,
              "F_GETLK on O_PATH -> rc=%d errno=%s "
              "(EBADF per spec, succeeds on WSL1)", gl, strerror(errno));

        int dfd2 = dup(pfd);
        CHECK(dfd2 >= 0, "dup -> fd=%d", dfd2);
        if (dfd2 >= 0) {
            /* The dup'd fd is also O_PATH, so EBADF still triggers --
             * which is what we want for fork/dup semantics. */
            errno = 0; r = read(dfd2, b, sizeof(b));
            CHECK(r == -1 && errno == EBADF,
                  "read on dup'd O_PATH -> rc=%zd errno=%s",
                  r, strerror(errno));
            close(dfd2);
        }

        /* --- Escape hatch: opening /proc/self/fd/N from an O_PATH fd
         * returns a real, fully-opened fd. Any read through THAT fd
         * sees whatever bytes are in the backing file, bypassing our
         * canary entirely. The shim still sees the open(), but the
         * caller can ask for the kernel to upgrade the fd type. --- */

        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", pfd);

        char linkbuf[256];
        ssize_t lnk = readlink(procpath, linkbuf, sizeof(linkbuf) - 1);
        CHECK(lnk > 0,
              "readlink(%s) -> %zd (errno=%s)",
              procpath, lnk, lnk < 0 ? strerror(errno) : "ok");
        if (lnk > 0) {
            linkbuf[lnk] = '\0';
            CHECK(strcmp(linkbuf, file_template) == 0,
                  "readlink target -> %s (want %s)",
                  linkbuf, file_template);
        }

        int realfd = open(procpath, O_RDONLY);
        CHECK(realfd >= 0,
              "open(%s) upgrades O_PATH to real fd -> %d (escape hatch)",
              procpath, realfd);
        if (realfd >= 0) {
            char ebuf[16] = {0};
            ssize_t er = read(realfd, ebuf, sizeof(ebuf) - 1);
            CHECK(er == (ssize_t)clen && memcmp(ebuf, content, clen) == 0,
                  "read via escape fd -> rc=%zd buf=\"%s\" (want %zu/\"hello\")",
                  er, ebuf, clen);
            close(realfd);
        }

        close(pfd);
    }

    unlink(file_template);
}

static void test_opath_flock_liveness(void)
{
    /* ============================================================
     * O_PATH flock liveness cycle. Tests whether we can use flock
     * on an O_PATH fd as a "caller still alive" signal:
     *   1. open O_PATH on a file (the "synthetic" caller fd)
     *   2. flock LOCK_SH on it (silent success on WSL1 per manpage,
     *      but our probe below tells us if the lock is real)
     *   3. open /proc/self/fd/N as a separate OFD (the "probe")
     *   4. probe LOCK_EX -> EWOULDBLOCK means caller-fd is alive
     *   5. close the O_PATH fd (simulates caller close)
     *   6. probe LOCK_EX -> 0 means caller is gone (cleanup signal)
     * If any step misbehaves, the cleanup-via-flock design is dead.
     * Note: real Linux returns EBADF on flock(O_PATH) so this test
     * is WSL1-specific by design and any cleanup machinery built on
     * it would have to be conditional on that quirk.
     * ============================================================ */
    STATUS("--- O_PATH flock liveness cycle ---");
    {
        char ltmp[] = "/tmp/wslcompat_opath_lock_XXXXXX";
        int seed = mkstemp(ltmp);
        if (seed < 0) {
            CHECK(0, "mkstemp for lock cycle: %s", strerror(errno));
        } else {
            close(seed);

            int caller_fd = open(ltmp, O_PATH);
            CHECK(caller_fd >= 0,
                  "open(O_PATH) for liveness cycle -> fd=%d (errno=%s)",
                  caller_fd, caller_fd < 0 ? strerror(errno) : "ok");

            if (caller_fd >= 0) {
                errno = 0;
                int sl = flock(caller_fd, LOCK_SH | LOCK_NB);

                if (sl == -1 && errno == EBADF) {
                    /* Real Linux refuses flock on an O_PATH fd per
                     * the manpage. Nothing to test here -- the
                     * liveness cycle relies on WSL1's quirk of
                     * accepting and enforcing the lock anyway, so
                     * record that flock behaves per-spec and move on
                     * without exercising the rest of the cycle. */
                    printf("  INFO: flock(O_PATH)=EBADF on this kernel; "
                           "liveness cycle is WSL1-only, skipping.\n");
                    close(caller_fd);
                    caller_fd = -1;
                } else {
                    CHECK(sl == 0,
                          "flock(LOCK_SH) on O_PATH -> rc=%d errno=%s",
                          sl, strerror(errno));

                    char probepath[64];
                    snprintf(probepath, sizeof(probepath),
                             "/proc/self/fd/%d", caller_fd);
                    int probe_fd = open(probepath, O_RDONLY);
                    CHECK(probe_fd >= 0,
                          "open(%s) for probe OFD -> fd=%d (errno=%s)",
                          probepath, probe_fd,
                          probe_fd < 0 ? strerror(errno) : "ok");

                    if (probe_fd >= 0) {
                        /* Step 4: caller-alive -> probe must conflict. */
                        errno = 0;
                        int xl = flock(probe_fd, LOCK_EX | LOCK_NB);
                        CHECK(xl == -1 && errno == EWOULDBLOCK,
                              "probe LOCK_EX with caller-fd live -> "
                              "rc=%d errno=%s (want EWOULDBLOCK)",
                              xl, strerror(errno));
                        if (xl == 0) flock(probe_fd, LOCK_UN);

                        /* Step 5: simulate caller close. */
                        close(caller_fd);
                        caller_fd = -1;

                        /* Step 6: caller-gone -> probe must succeed. */
                        errno = 0;
                        int xl2 = flock(probe_fd, LOCK_EX | LOCK_NB);
                        CHECK(xl2 == 0,
                              "probe LOCK_EX after caller close -> "
                              "rc=%d errno=%s (want 0 = cleanup signal)",
                              xl2, strerror(errno));
                        if (xl2 == 0) flock(probe_fd, LOCK_UN);
                        close(probe_fd);
                    }
                }

                if (caller_fd >= 0) close(caller_fd);
            }
            unlink(ltmp);
        }
    }

}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);

    test_opath_directory();
    test_opath_readless_dir();
    test_opath_regular_file();
    test_opath_flock_liveness();

    TEST_REPORT();
}
