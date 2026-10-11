/* Probe F_SETOWN_EX / F_GETOWN_EX behavior with F_OWNER_PID and friends.
 *
 * On native Linux the kernel resolves owner.pid through find_vpid() at
 * SETOWN time and returns -ESRCH if the pid doesn't refer to a live
 * task; pid==0 is the "clear owner" form, and F_OWNER_TID/F_OWNER_PGRP
 * work the same way against TID/PGID lookups. WSL1's implementation is
 * partial, so the bulk of this file is informational probes covering:
 *   - every (type, pid-sign) combination
 *   - several fd types (pipe, UDP/TCP/Unix socket, regular file, /dev/null)
 *   - the legacy F_SETOWN form (pid<0 = pgrp convention)
 *   - cross-uid live pids, and a pid that dies between SET and GET
 * The strict CHECK assertions at the top still encode Linux's contract
 * so the test fails unpatched on WSL (XFAIL) and passes on real Linux.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <dirent.h>
#include <err.h>

static int failures;
static int subtests;

#define CHECK(cond, fmt, ...) do {                                  \
    subtests++;                                                     \
    if (cond) printf("  PASS: " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; }\
} while (0)

static const char *type_name(int t)
{
    if (t == F_OWNER_TID) return "F_OWNER_TID";
    if (t == F_OWNER_PID) return "F_OWNER_PID";
    if (t == F_OWNER_PGRP) return "F_OWNER_PGRP";
    return "?";
}

/* Issue a SETOWN_EX attempt and then read back via GETOWN_EX. Logs
 * the result. Returns nothing — purely informational. */
static void probe(int fd, int type, pid_t pid)
{
    struct f_owner_ex owner = { .type = type, .pid = pid };
    errno = 0;
    int rc = fcntl(fd, F_SETOWN_EX, &owner);
    int saved = errno;

    printf("  SET {%-12s pid=%11d} -> ",
           type_name(type), pid);

    if (rc != 0) {
        printf("FAIL errno=%d (%s)\n", saved, strerror(saved));
        return;
    }

    struct f_owner_ex got;
    memset(&got, 0xa5, sizeof(got));
    errno = 0;
    if (fcntl(fd, F_GETOWN_EX, &got) != 0) {
        printf("OK; GET failed errno=%d (%s)\n", errno, strerror(errno));
        return;
    }
    printf("OK; GET {%-12s pid=%11d}\n",
           type_name(got.type), got.pid);
}

/* Same for legacy F_SETOWN/F_GETOWN, which takes a bare pid_t with the
 * sign convention: >0 = process, <0 = pgrp (|pid|), 0 = clear. */
static void probe_legacy(int fd, pid_t pid)
{
    errno = 0;
    int rc = fcntl(fd, F_SETOWN, pid);
    int saved = errno;

    printf("  F_SETOWN pid=%11d -> ", pid);
    if (rc != 0) {
        printf("FAIL errno=%d (%s)\n", saved, strerror(saved));
        return;
    }

    errno = 0;
    int got = fcntl(fd, F_GETOWN);
    if (got == -1 && errno != 0) {
        /* Real ambiguity: F_GETOWN returns -1 both for error and for
         * a pgrp of 1. Distinguish via GETOWN_EX. */
        struct f_owner_ex ex;
        memset(&ex, 0, sizeof(ex));
        if (fcntl(fd, F_GETOWN_EX, &ex) == 0)
            printf("OK; GETOWN ambiguous, GETOWN_EX={%s pid=%d}\n",
                   type_name(ex.type), ex.pid);
        else
            printf("OK; GETOWN failed errno=%d\n", errno);
        return;
    }
    printf("OK; GETOWN returned %d\n", got);
}

/* Walk /proc and return the first pid that lives under a different uid
 * than us, or -1 if none found. */
static pid_t find_foreign_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    uid_t our_uid = getuid();
    pid_t self = getpid();
    pid_t found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long n = strtol(e->d_name, &end, 10);
        if (*end != '\0' || n < 1) continue;
        if ((pid_t)n == self) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/status", n);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned int uid;
            if (sscanf(line, "Uid:\t%u", &uid) == 1) {
                if (uid != our_uid) found = (pid_t)n;
                break;
            }
        }
        fclose(f);
        if (found != -1) break;
    }
    closedir(d);
    return found;
}

/* SETOWN_EX to a live pid we don't own (different uid), then GETOWN_EX. */
static void probe_foreign_owner(int fd)
{
    pid_t foreign = find_foreign_pid();
    if (foreign == -1) {
        printf("  SKIP: no foreign-uid pid found in /proc\n");
        return;
    }
    printf("  found foreign-uid pid=%d\n", foreign);

    struct f_owner_ex owner = { .type = F_OWNER_PID, .pid = foreign };
    errno = 0;
    int rc = fcntl(fd, F_SETOWN_EX, &owner);
    int saved = errno;
    if (rc != 0) {
        printf("  SET pid=%d -> FAIL errno=%d (%s)\n",
               foreign, saved, strerror(saved));
        return;
    }

    struct f_owner_ex got;
    memset(&got, 0xa5, sizeof(got));
    if (fcntl(fd, F_GETOWN_EX, &got) == 0)
        printf("  SET pid=%d -> OK; GET {%s pid=%d}\n",
               foreign, type_name(got.type), got.pid);
    else
        printf("  SET pid=%d -> OK; GET failed errno=%d\n",
               foreign, errno);
}

/* Set owner to a soon-dead pid, then read back after it's gone.
 *
 * WSL only accepts SETOWN_EX(F_OWNER_PID) when the pid equals the
 * caller's own pid, so we have the child do the SET on the shared
 * struct file before exiting, and then the parent does GETOWN_EX
 * after reaping. */
static void probe_set_then_die(int fd)
{
    int sp[2];
    if (pipe(sp) == -1) {
        printf("  SKIP: pipe failed errno=%d\n", errno);
        return;
    }

    pid_t child = fork();
    if (child == -1) {
        printf("  SKIP: fork failed errno=%d\n", errno);
        close(sp[0]); close(sp[1]);
        return;
    }
    if (child == 0) {
        close(sp[0]);
        struct f_owner_ex o = { .type = F_OWNER_PID, .pid = getpid() };
        char b = (fcntl(fd, F_SETOWN_EX, &o) == 0) ? 'Y' : 'N';
        (void)!write(sp[1], &b, 1);
        _exit(b == 'Y' ? 0 : 1);
    }
    close(sp[1]);

    char b = 0;
    (void)!read(sp[0], &b, 1);
    close(sp[0]);
    int status;
    waitpid(child, &status, 0);

    if (b != 'Y') {
        printf("  SKIP: child SETOWN_EX failed (child status=%d)\n", status);
        return;
    }
    printf("  child %d set owner to itself and exited\n", child);

    struct f_owner_ex got;
    memset(&got, 0xa5, sizeof(got));
    errno = 0;
    if (fcntl(fd, F_GETOWN_EX, &got) == 0)
        printf("  after reap: GET {%s pid=%d}\n",
               type_name(got.type), got.pid);
    else
        printf("  after reap: GET failed errno=%d (%s)\n",
               errno, strerror(errno));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* A pipe is a natural fd for F_SETOWN — it can deliver SIGIO. */
    int pipefd[2];
    if (pipe(pipefd) == -1) err(EXIT_FAILURE, "pipe");
    int fd = pipefd[0];

    pid_t self = getpid();
    pid_t tid  = syscall(SYS_gettid);
    pid_t pgrp = getpgrp();
    struct f_owner_ex owner;
    int rc, saved;

    printf("self pid=%d tid=%d pgrp=%d\n\n", self, tid, pgrp);

    /* ---- Strict Linux-spec assertions ---------------------------- */

    printf("--- F_SETOWN_EX(F_OWNER_PID, getpid()) ---\n");
    owner = (struct f_owner_ex){ .type = F_OWNER_PID, .pid = self };
    errno = 0;
    rc = fcntl(fd, F_SETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == 0,
          "rc=%d errno=%d (%s) -- want 0", rc, saved, strerror(saved));

    printf("--- F_GETOWN_EX after setting our own PID ---\n");
    memset(&owner, 0xa5, sizeof(owner));
    errno = 0;
    rc = fcntl(fd, F_GETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == 0,
          "GETOWN_EX rc=%d errno=%d (%s) -- want 0",
          rc, saved, strerror(saved));
    CHECK(owner.type == F_OWNER_PID,
          "owner.type=%d (%s) -- want F_OWNER_PID",
          owner.type, type_name(owner.type));
    CHECK(owner.pid == self,
          "owner.pid=%d -- want %d", owner.pid, self);

    printf("--- F_SETOWN_EX(F_OWNER_PID, 0) (clear) ---\n");
    owner = (struct f_owner_ex){ .type = F_OWNER_PID, .pid = 0 };
    errno = 0;
    rc = fcntl(fd, F_SETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == 0,
          "rc=%d errno=%d (%s) -- want 0", rc, saved, strerror(saved));

    printf("--- F_SETOWN_EX(F_OWNER_PID, INT_MAX) ---\n");
    owner = (struct f_owner_ex){ .type = F_OWNER_PID, .pid = INT_MAX };
    errno = 0;
    rc = fcntl(fd, F_SETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == -1 && saved == ESRCH,
          "rc=%d errno=%d (%s) -- want -1/ESRCH",
          rc, saved, strerror(saved));

    printf("--- F_SETOWN_EX(F_OWNER_TID, gettid()) ---\n");
    owner = (struct f_owner_ex){ .type = F_OWNER_TID, .pid = tid };
    errno = 0;
    rc = fcntl(fd, F_SETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == 0,
          "rc=%d errno=%d (%s) -- want 0", rc, saved, strerror(saved));

    printf("--- F_SETOWN_EX(F_OWNER_PGRP, getpgrp()) ---\n");
    owner = (struct f_owner_ex){ .type = F_OWNER_PGRP, .pid = pgrp };
    errno = 0;
    rc = fcntl(fd, F_SETOWN_EX, &owner);
    saved = errno;
    CHECK(rc == 0,
          "rc=%d errno=%d (%s) -- want 0", rc, saved, strerror(saved));

    /* ---- Probe: every type x sign combination -------------------- */

    printf("\n--- PROBE: SETOWN_EX combinations (informational) ---\n");
    probe(fd, F_OWNER_TID,  tid);
    probe(fd, F_OWNER_TID,  -tid);
    probe(fd, F_OWNER_TID,  0);
    probe(fd, F_OWNER_TID,  INT_MAX);
    probe(fd, F_OWNER_TID,  INT_MIN);
    probe(fd, F_OWNER_PID,  self);
    probe(fd, F_OWNER_PID,  -self);
    probe(fd, F_OWNER_PID,  -pgrp);
    probe(fd, F_OWNER_PID,  0);
    probe(fd, F_OWNER_PID,  INT_MAX);
    probe(fd, F_OWNER_PID,  INT_MIN);
    probe(fd, F_OWNER_PGRP, pgrp);
    probe(fd, F_OWNER_PGRP, -pgrp);
    probe(fd, F_OWNER_PGRP, 0);
    probe(fd, F_OWNER_PGRP, INT_MAX);
    probe(fd, F_OWNER_PGRP, INT_MIN);

    /* type=99 is intentionally bogus — Linux returns EINVAL. */
    probe(fd, 99,           self);

    /* ---- Probe: legacy F_SETOWN ---------------------------------- */

    printf("\n--- PROBE: legacy F_SETOWN (informational) ---\n");
    probe_legacy(fd, self);
    probe_legacy(fd, -self);
    probe_legacy(fd, pgrp);
    probe_legacy(fd, -pgrp);
    probe_legacy(fd, 0);
    probe_legacy(fd, INT_MAX);
    probe_legacy(fd, -INT_MAX);
    probe_legacy(fd, INT_MIN);

    /* ---- Probe: a representative set of fd types ----------------
     *
     * If WSL only refuses F_SETOWN on certain fd kinds, this will
     * surface the one(s) that work. We just run the basic positive,
     * INT_MAX, and "negative" cases. */

    /* Regular file via mkstemp+unlink (O_TMPFILE is rejected on WSL1). */
    char regpath[] = "/tmp/fcntl_setown_ex_XXXXXX";
    int regfd = mkstemp(regpath);
    if (regfd != -1) unlink(regpath);

    struct {
        const char *name;
        int fd;
    } fds[5] = {
        { "UDP socket",     socket(AF_INET, SOCK_DGRAM, 0)  },
        { "TCP socket",     socket(AF_INET, SOCK_STREAM, 0) },
        { "Unix dgram",     socket(AF_UNIX, SOCK_DGRAM, 0)  },
        { "regular file",   regfd                           },
        { "/dev/null",      open("/dev/null", O_RDWR)       },
    };
    for (size_t i = 0; i < sizeof(fds)/sizeof(fds[0]); i++) {
        printf("\n--- PROBE: %s fd (informational) ---\n", fds[i].name);
        if (fds[i].fd == -1) {
            printf("  SKIP: open/socket failed errno=%d (%s)\n",
                   errno, strerror(errno));
            continue;
        }
        probe(fds[i].fd, F_OWNER_PID,  self);
        probe(fds[i].fd, F_OWNER_PID,  -self);
        probe(fds[i].fd, F_OWNER_PID,  0);
        probe(fds[i].fd, F_OWNER_PID,  INT_MAX);
        probe(fds[i].fd, F_OWNER_TID,  tid);
        probe(fds[i].fd, F_OWNER_PGRP, pgrp);
        probe_legacy(fds[i].fd, self);
        probe_legacy(fds[i].fd, -pgrp);
        close(fds[i].fd);
    }

    /* ---- Probe: with O_ASYNC set first ---- */

    printf("\n--- PROBE: pipe with O_ASYNC (informational) ---\n");
    if (fcntl(fd, F_SETFL, O_ASYNC) == -1)
        printf("  NOTE: F_SETFL O_ASYNC failed errno=%d (%s)\n",
               errno, strerror(errno));
    probe(fd, F_OWNER_PID,  self);
    probe(fd, F_OWNER_TID,  tid);
    probe(fd, F_OWNER_PGRP, pgrp);

    /* ---- Probe: against a real live child PID we just forked ----
     *
     * If WSL's EINVAL on positive pids is a "this pid resolves to a
     * process and then something else fails" path, an alien pid (one
     * we know is alive but isn't us) should give the same EINVAL.
     * If instead WSL rejects only our own pid for some reason, a
     * different live pid would succeed. */

    printf("\n--- PROBE: live child PID (informational) ---\n");
    int sync_pipe[2];
    if (pipe(sync_pipe) == -1) err(EXIT_FAILURE, "pipe");
    pid_t child = fork();
    if (child == -1) err(EXIT_FAILURE, "fork");
    if (child == 0) {
        close(sync_pipe[1]);
        char b;
        (void)!read(sync_pipe[0], &b, 1);   /* wait for parent */
        _exit(0);
    }
    close(sync_pipe[0]);
    printf("  child pid=%d\n", child);
    probe(fd, F_OWNER_PID,  child);
    probe(fd, F_OWNER_PID,  -child);
    probe(fd, F_OWNER_TID,  child);
    probe(fd, F_OWNER_PGRP, child);
    probe_legacy(fd, child);
    probe_legacy(fd, -child);
    close(sync_pipe[1]);          /* release child */
    waitpid(child, NULL, 0);

    /* ---- Probe: foreign owner & set-then-die on a working fd type ----
     *
     * We've established that regular files accept F_SETOWN_EX cleanly
     * on WSL, so use one for the cross-owner and lifetime probes. */

    char livepath[] = "/tmp/fcntl_setown_ex_live_XXXXXX";
    int livefd = mkstemp(livepath);
    if (livefd == -1) {
        printf("\nSKIP: mkstemp failed errno=%d\n", errno);
    } else {
        unlink(livepath);

        printf("\n--- PROBE: foreign-uid owner pid (informational) ---\n");
        probe_foreign_owner(livefd);

        printf("\n--- PROBE: owner pid dies after SET (informational) ---\n");
        probe_set_then_die(livefd);

        close(livefd);
    }

    close(pipefd[0]);
    close(pipefd[1]);

    printf("\n%d/%d subtests passed, %d failed\n",
           subtests - failures, subtests, failures);
    return failures == 0 ? 0 : 1;
}
