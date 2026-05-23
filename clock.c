#define _GNU_SOURCE
#include <time.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/param.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(clock_nanosleep, clock_getres, clock_gettime);

// Difference between TAI and UTC, in seconds.
#define TAI_UTC_DEFAULT 37

// Windows native scheduler tick -- 1/64 second.
#define WIN_TICK_NS 15625000L

static long long tv_to_ns(const struct timeval *tv)
{
    return (long long)tv->tv_sec * 1000000000LL
         + (long long)tv->tv_usec * 1000LL;
}

static void ns_to_ts(long long ns, struct timespec *ts)
{
    ts->tv_sec  = ns / 1000000000LL;
    ts->tv_nsec = ns % 1000000000LL;
}

// Read getrusage(who) and write summed user+system CPU time in ns to *out.
static int rusage_cpu_ns(int who, long long *out)
{
    struct rusage r;
    if (getrusage(who, &r) != 0)
        return errno;
    *out = tv_to_ns(&r.ru_utime) + tv_to_ns(&r.ru_stime);
    return 0;
}

// Per-clockid handler signature.
typedef int (*nanosleep_fn)(clockid_t, int,
                            const struct timespec *, struct timespec *);

static int handle_boottime(clockid_t id, int flags,
                           const struct timespec *req, struct timespec *rem)
{
    wsldbg("CLOCK_BOOTTIME -> CLOCK_MONOTONIC (flags=0x%x)", flags);
    return sym_next(clock_nanosleep, CLOCK_MONOTONIC, flags, req, rem);
}

// TAI advances at the same rate as REALTIME; they only differ by a static
// integer-seconds offset.
static int handle_tai(clockid_t id, int flags,
                      const struct timespec *req, struct timespec *rem)
{
    struct timespec adj = *req;
    long offset;

    if ((flags & TIMER_ABSTIME) == 0) {
        wsldbg("CLOCK_TAI relative -> CLOCK_REALTIME");
        return sym_next(clock_nanosleep, CLOCK_REALTIME, flags, req, rem);
    }

    offset = wslcompat_tunable_int("taioffset", TAI_UTC_DEFAULT);

    wsldbg("CLOCK_TAI ABSTIME -> CLOCK_REALTIME (offset=%lds)", offset);

    if (adj.tv_sec >= offset)
        adj.tv_sec -= offset;
    else
        adj.tv_sec = adj.tv_nsec = 0;
    return sym_next(clock_nanosleep, CLOCK_REALTIME, flags, &adj, rem);
}

// CLOCK_PROCESS_CPUTIME_ID
// wsl1 doesn't deliver any CPU-time-driven signal (itimer prof, posix-timer,
// RLIMIT_CPU, etc), so we poll getrusage at one Windows tick per iteration.
static int handle_process_cputime(clockid_t id, int flags,
                                  const struct timespec *req,
                                  struct timespec *rem)
{
    int rc;
    long long cur;
    long long now_ns;
    long long target_ns;

    if ((rc = rusage_cpu_ns(RUSAGE_SELF, &now_ns)) != 0)
        return rc;

    target_ns = req->tv_sec * 1000000000LL + req->tv_nsec;

    if (!(flags & TIMER_ABSTIME))
        target_ns += now_ns;

    wsldbg("CLOCK_PROCESS_CPUTIME_ID polling: now=%lldns target=%lldns",
           now_ns, target_ns);

    while (now_ns < target_ns) {
        // CPU time can never advance faster than wall time, so sleeping
        // for the full remaining duration is safe. Floor at one Windows
        // tick so sub-tick sleeps don't just spin.
        long long sleep_ns = MAX(target_ns - now_ns, WIN_TICK_NS);
        struct timespec poll;

        // Convert target to a timespec.
        ns_to_ts(sleep_ns, &poll);

        if ((rc = sym_next(clock_nanosleep, CLOCK_MONOTONIC, 0, &poll, NULL)) != 0) {
            if (rc != EINTR) {
                wsldbg("clock_nanosleep failed unexpectedly during usage polling: %s", strerror(rc));
                return rc;
            }

            if (rem && !(flags & TIMER_ABSTIME)) {
                if (rusage_cpu_ns(RUSAGE_SELF, &cur) == 0)
                    ns_to_ts(MAX(target_ns - cur, 0LL), rem);
            }

            return rc;
        }

        if ((rc = rusage_cpu_ns(RUSAGE_SELF, &now_ns)) != 0)
            return rc;
    }

    if (rem && !(flags & TIMER_ABSTIME)) {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

// To add a new clock: write a handler above, add an entry below.
// Anything not in the table is passed straight to the kernel.
static const struct {
    clockid_t    id;
    nanosleep_fn fn;
} dispatch[] = {
    { CLOCK_BOOTTIME,           handle_boottime        },
    { CLOCK_TAI,                handle_tai             },
    { CLOCK_PROCESS_CPUTIME_ID, handle_process_cputime },
};

int clock_nanosleep(clockid_t clockid, int flags,
                    const struct timespec *request,
                    struct timespec *remain)
{
    if (wslcompat_passthru_self())
        return sym_next(clock_nanosleep, clockid, flags, request, remain);

    if (request == NULL)
        return EFAULT;

    for (size_t i = 0; i < _countof(dispatch); i++) {
        if (dispatch[i].id == clockid)
            return dispatch[i].fn(clockid, flags, request, remain);
    }
    return sym_next(clock_nanosleep, clockid, flags, request, remain);
}

typedef int (*getres_fn)(clockid_t, struct timespec *);

// CLOCK_TAI ticks at the same rate as REALTIME, so its resolution is identical.
static int handle_tai_getres(clockid_t id, struct timespec *res)
{
    return sym_next(clock_getres, CLOCK_REALTIME, res);
}

static const struct {
    clockid_t id;
    getres_fn fn;
} getres_dispatch[] = {
    { CLOCK_TAI, handle_tai_getres },
};

int clock_getres(clockid_t clockid, struct timespec *res)
{
    if (wslcompat_passthru_self())
        return sym_next(clock_getres, clockid, res);

    for (size_t i = 0; i < _countof(getres_dispatch); i++) {
        if (getres_dispatch[i].id == clockid)
            return getres_dispatch[i].fn(clockid, res);
    }

    return sym_next(clock_getres, clockid, res);
}

typedef int (*gettime_fn)(clockid_t, struct timespec *);

// TAI = REALTIME + the static UTC offset. Read REALTIME then shift forward.
static int handle_tai_gettime(clockid_t id, struct timespec *tp)
{
    if (sym_next(clock_gettime, CLOCK_REALTIME, tp) != 0)
        return -1;

    long offset = wslcompat_tunable_int("taioffset", TAI_UTC_DEFAULT);

    wsldbg("clock_gettime(CLOCK_TAI) = REALTIME + %lds", offset);

    tp->tv_sec += offset;

    return 0;
}

static const struct {
    clockid_t  id;
    gettime_fn fn;
} gettime_dispatch[] = {
    { CLOCK_TAI, handle_tai_gettime },
};

// Encoded CPU clockids returned by clock_getcpuclockid() and pthread_getcpuclockid()
// are packed by glibc as ((~pid << 3) | clock_type_bits), where bits 0-1 are the
// clock type, bit 2 is the PERTHREAD flag, and the upper bits carry ~pid.
static int encoded_cpuclock_who(clockid_t clockid)
{
    if (clockid >= 0)
        return -1;
    if ((clockid & 3) != 2)
        return -1;

    pid_t decoded = ~(clockid >> 3);
    int   who     = RUSAGE_SELF;
    pid_t self    = getpid();

    if (clockid & 4) {
        who  = RUSAGE_THREAD;
        self = syscall(SYS_gettid);
    }

    if (decoded != 0 && decoded != self)
        return -1;
    return who;
}

int clock_gettime(clockid_t clockid, struct timespec *tp)
{
    long long ns;

    if (wslcompat_passthru_self())
        return sym_next(clock_gettime, clockid, tp);

    int who = encoded_cpuclock_who(clockid);

    if (who >= 0) {
        wsldbg("encoded cpuclockid %d -> %d", clockid, who);

        if (rusage_cpu_ns(who, &ns) != 0)
            return -1;
        ns_to_ts(ns, tp);
        return 0;
    }

    for (size_t i = 0; i < _countof(gettime_dispatch); i++) {
        if (gettime_dispatch[i].id == clockid)
            return gettime_dispatch[i].fn(clockid, tp);
    }
    return sym_next(clock_gettime, clockid, tp);
}
