#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
extern char **environ;
int main() {
    pid_t pid;
    char *argv[] = { "sys_badalign_test", NULL };
    int status = posix_spawn(&pid, "./sys_badalign_test", NULL, NULL, argv, environ);
    if (status == 0) {
        waitpid(pid, &status, 0);
        printf("posix_spawn succeeded, child exited with %d\n", WEXITSTATUS(status));
    } else {
        printf("posix_spawn failed: %d\n", status);
    }
    return 0;
}
