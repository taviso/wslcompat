#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <err.h>

static void noop_handler(int sig) { (void)sig; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    char path[] = "/tmp/fcntl_eintr_XXXXXX";
    int fd = mkostemp(path, O_RDWR);
    if (fd == -1) err(EXIT_FAILURE, "mkostemp");

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "parent F_SETLK");

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        int fd2 = open(path, O_RDWR);
        if (fd2 == -1) err(EXIT_FAILURE, "child open");

        // sa_flags=0 leaves SA_RESTART unset, so syscalls aren't auto-restarted.
        struct sigaction sa = { .sa_handler = noop_handler, .sa_flags = 0 };
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR1, &sa, NULL) == -1) err(EXIT_FAILURE, "sigaction");

        struct flock cl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        printf("  child: entering F_SETLKW (should block)\n");
        int rc = fcntl(fd2, F_SETLKW, &cl);
        int saved = errno;
        printf("  child: F_SETLKW returned %d errno=%d (%s)\n", rc, saved, strerror(saved));

        if (rc != -1) {
            printf("  FAIL: F_SETLKW succeeded instead of being interrupted\n");
            _exit(1);
        }
        if (saved != EINTR) {
            printf("  FAIL: expected EINTR got %s\n", strerror(saved));
            _exit(1);
        }
        printf("  PASS\n");
        _exit(0);
    }

    sleep(1);
    printf("  parent: sending SIGUSR1 to child\n");
    if (kill(pid, SIGUSR1) == -1) err(EXIT_FAILURE, "kill");

    int status;
    waitpid(pid, &status, 0);

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
    unlink(path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
    return 0;
}
