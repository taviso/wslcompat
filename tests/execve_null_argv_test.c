/* Verify how execve(path, NULL, NULL) is handled. Forks a child so the
 * caller survives whatever the kernel/polyfill decides. Reports the
 * exact outcome and passes only if the child cleanly ran the target
 * binary (exit 0), which is what our polyfill aims for by translating
 * NULL argv to a single-NULL argv. */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <valgrind/valgrind.h>

/* Volatile NULL to evade glibc's __nonnull on execve at the call site. */
static void *opaque_null(void)
{
    static void *volatile p;
    return p;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Valgrind's intercepted execve refuses NULL argv (ENOENT) and is
     * checking memory anyway -- nothing useful to test under it. */
    if (RUNNING_ON_VALGRIND) {
        printf("SKIP: behavior test, not meaningful under valgrind\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0)
        err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        /* Run /bin/true with NULL argv and NULL envp. */
        execve("/bin/true", opaque_null(), opaque_null());
        /* If execve returns, it failed. Encode errno into exit status. */
        _exit(errno);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
        err(EXIT_FAILURE, "waitpid");

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("FAIL: child killed by signal %d (%s)\n", sig, strsignal(sig));
        return 1;
    }
    if (!WIFEXITED(status)) {
        printf("FAIL: unexpected wait status 0x%x\n", status);
        return 1;
    }

    int code = WEXITSTATUS(status);
    if (code == 0) {
        printf("PASS: execve(/bin/true, NULL, NULL) ran target cleanly\n");
        return 0;
    }
    printf("FAIL: execve returned errno=%d (%s)\n", code, strerror(code));
    return 1;
}
