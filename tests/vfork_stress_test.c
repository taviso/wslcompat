#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <stdatomic.h>
#include <valgrind/valgrind.h>

#define NUM_THREADS 8
#define ITERS 1000

static int shared_fd;
static atomic_int stop_flag;
static atomic_int total_vforks;

static void open_pty(int *m_out, int *s_out)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *m_out = m;
    *s_out = s;
}

static void set_raw(int fd)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

/* Worker thread: constantly performs vfork and relocates the shared FD.
 * On Linux, vfork only suspends the CALLING thread; others keep running.
 * This tests if thread B's COW lookups are stable while thread A's
 * vfork-child is mutating shared memory. */
static void *vfork_worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        pid_t pid = vfork();
        if (pid == 0) {
            /* In child: trigger relocation by closing the shared FD.
             * This triggers tty_relocate -> wslcompat_view_fd_store. */
            close(shared_fd);
            _exit(0);
        } else if (pid > 0) {
            atomic_fetch_add(&total_vforks, 1);
            int status;
            if (waitpid(pid, &status, 0) == -1)
                err(EXIT_FAILURE, "waitpid (thread %ld iter %d)", id, i);
        } else {
            err(EXIT_FAILURE, "vfork (thread %ld iter %d)", id, i);
        }
    }
    return NULL;
}

/* Probe thread: constantly verifies the parent's view of the wrap
 * is uncorrupted. */
static void *probe_worker(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop_flag)) {
        struct termios t;
        /* tcgetattr triggers tty_real_fd_load -> wslcompat_view_fd_load.
         * If a vfork-child's mutation of shared memory leaked into the
         * parent, this will fail with EBADF (or see the wrong FD). */
        if (tcgetattr(shared_fd, &t) != 0) {
            err(EXIT_FAILURE, "parent wrap corrupted! (errno=%d)", errno);
        }
    }
    return NULL;
}

int main(void)
{
    int m, s;

    if (RUNNING_ON_VALGRIND) {
        printf("SKIP: vfork not supported on valgrind!\n");
        return 0;
    }

    open_pty(&m, &s);
    set_raw(s);
    shared_fd = s;

    printf("Starting vfork stress: %d threads, %d iters each...\n",
           NUM_THREADS, ITERS);

    pthread_t threads[NUM_THREADS];
    pthread_t probe_tid;

    pthread_create(&probe_tid, NULL, probe_worker, NULL);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, vfork_worker, (void *)(long)i);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    atomic_store(&stop_flag, 1);
    pthread_join(probe_tid, NULL);

    printf("PASS: %d total vforks completed with zero parent corruption.\n",
           atomic_load(&total_vforks));

    close(s);
    close(m);
    return 0;
}
