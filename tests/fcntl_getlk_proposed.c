#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

static const char *describe(int t)
{
    if (t == F_UNLCK) return "F_UNLCK";
    if (t == F_RDLCK) return "F_RDLCK";
    if (t == F_WRLCK) return "F_WRLCK";
    return "?";
}

static const char *cmd_name(int cmd)
{
    if (cmd == F_GETLK) return "F_GETLK";
    return "F_OFD_GETLK";
}

static void hold(int fd, int type)
{
    struct flock fl = {
        .l_type   = type,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };
    if (fcntl(fd, F_SETLK, &fl) == -1) err(EXIT_FAILURE, "F_SETLK type=%d", type);
}

static int query_in_child(const char *path, int cmd, int proposed, int expected)
{
    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        int fd = open(path, O_RDWR);
        if (fd == -1) err(EXIT_FAILURE, "child open");

        struct flock fl = {
            .l_type   = proposed,
            .l_whence = SEEK_SET,
            .l_start  = 0,
            .l_len    = 0,
        };
        if (fcntl(fd, cmd, &fl) == -1) err(EXIT_FAILURE, "%s", cmd_name(cmd));

        printf("  proposed=%s got=%s (expected %s)\n",
               describe(proposed), describe(fl.l_type), describe(expected));
        fflush(stdout);

        if (fl.l_type != expected) _exit(1);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

static int run_battery(int holder, const char *path, int cmd)
{
    int failures = 0;

    printf("=== %s ===\n", cmd_name(cmd));

    printf("--- Holder: shared (F_RDLCK) ---\n");
    hold(holder, F_RDLCK);
    failures += query_in_child(path, cmd, F_RDLCK, F_UNLCK);
    failures += query_in_child(path, cmd, F_WRLCK, F_RDLCK);
    hold(holder, F_UNLCK);

    printf("--- Holder: exclusive (F_WRLCK) ---\n");
    hold(holder, F_WRLCK);
    failures += query_in_child(path, cmd, F_RDLCK, F_WRLCK);
    failures += query_in_child(path, cmd, F_WRLCK, F_WRLCK);
    hold(holder, F_UNLCK);

    printf("--- Holder: none ---\n");
    failures += query_in_child(path, cmd, F_RDLCK, F_UNLCK);
    failures += query_in_child(path, cmd, F_WRLCK, F_UNLCK);

    return failures;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    char path[] = "/tmp/fcntl_getlk_proposed_XXXXXX";
    int holder = mkostemp(path, O_RDWR);
    if (holder == -1) err(EXIT_FAILURE, "mkostemp");

    int failures = run_battery(holder, path, F_GETLK);
    failures    += run_battery(holder, path, F_OFD_GETLK);

    close(holder);
    unlink(path);

    if (failures > 0) {
        printf("FAIL: %d mismatch(es)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
