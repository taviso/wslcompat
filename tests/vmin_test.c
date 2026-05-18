#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <err.h>

static void timeout_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: test timed out (kernel ignoring VMIN/VTIME?)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static void open_pty(int *master_fd, int *slave_fd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m == -1) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(m) == -1) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(m) == -1) err(EXIT_FAILURE, "unlockpt");
    char *name = ptsname(m);
    if (!name) err(EXIT_FAILURE, "ptsname");
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s == -1) err(EXIT_FAILURE, "open slave");
    *master_fd = m;
    *slave_fd  = s;
}

static void set_raw(int fd, cc_t vmin, cc_t vtime)
{
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == -1) err(EXIT_FAILURE, "TCGETS");
    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN]  = vmin;
    t.c_cc[VTIME] = vtime;
    if (ioctl(fd, TCSETS, &t) == -1) err(EXIT_FAILURE, "TCSETS");
}

static long elapsed_ms(struct timeval *t0, struct timeval *t1)
{
    return (t1->tv_sec  - t0->tv_sec)  * 1000
         + (t1->tv_usec - t0->tv_usec) / 1000;
}

static int test_polled(void)
{
    printf("--- VMIN=0/VTIME=0 (polled, instant return) ---\n");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 0, 0);

    char buf[16];
    struct timeval t0, t1;

    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    printf("  empty read -> %zd bytes in %ld ms\n", n, ms);
    if (n != 0 || ms > 200) {
        close(m); close(s);
        printf("  FAIL: expected 0 bytes instantly\n");
        return 1;
    }

    if (write(m, "abc", 3) != 3) err(EXIT_FAILURE, "write");
    usleep(50000);

    gettimeofday(&t0, NULL);
    n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    ms = elapsed_ms(&t0, &t1);
    printf("  3-byte read -> %zd bytes in %ld ms\n", n, ms);
    close(m); close(s);
    if (n != 3 || ms > 200) {
        printf("  FAIL: expected 3 bytes instantly\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

static int test_vmin_only(void)
{
    printf("--- VMIN=5/VTIME=0 (block until N bytes) ---\n");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 5, 0);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");
    if (pid == 0) {
        usleep(300000);
        write(m, "hello", 5);
        _exit(0);
    }

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);

    waitpid(pid, NULL, 0);
    close(m); close(s);

    printf("  read -> %zd bytes in %ld ms\n", n, ms);
    if (n < 5) {
        printf("  FAIL: expected at least 5 bytes\n");
        return 1;
    }
    if (ms < 200) {
        printf("  FAIL: returned too fast (kernel ignored VMIN)\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

static int test_interbyte_timeout(void)
{
    printf("--- VMIN=10/VTIME=5 (inter-byte timeout after first byte) ---\n");
    int m, s;
    open_pty(&m, &s);
    set_raw(s, 10, 5);

    if (write(m, "xyz", 3) != 3) err(EXIT_FAILURE, "write");

    char buf[16];
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ssize_t n = read(s, buf, sizeof(buf));
    gettimeofday(&t1, NULL);
    long ms = elapsed_ms(&t0, &t1);
    close(m); close(s);

    printf("  read -> %zd bytes in %ld ms\n", n, ms);
    if (n != 3) {
        printf("  FAIL: expected exactly 3 bytes\n");
        return 1;
    }
    if (ms < 350 || ms > 1500) {
        printf("  FAIL: timing %ld ms out of expected range [350, 1500]\n", ms);
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = { .sa_handler = timeout_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(10);

    int failures = 0;
    failures += test_polled();
    failures += test_vmin_only();
    failures += test_interbyte_timeout();

    if (failures > 0) {
        printf("FAIL: %d subtest(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
