#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <err.h>

int main() {
    char tmpfile[] = "/tmp/fcntl_close_wipe_XXXXXX";
    int fd1, fd2;
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };

    fd1 = mkostemp(tmpfile, O_RDWR);
    if (fd1 == -1) err(EXIT_FAILURE, "mkostemp");
    
    fd2 = open(tmpfile, O_RDWR);
    if (fd2 == -1) err(EXIT_FAILURE, "open");

    unlink(tmpfile);

    printf("Testing if close() wipes all fcntl() locks for the process...\n");

    printf("Parent: Acquiring fcntl lock on fd1...\n");
    if (fcntl(fd1, F_SETLK, &fl) == -1) {
        err(EXIT_FAILURE, "fcntl(fd1) failed");
    }

    printf("Parent: Closing fd2 (different FD to the same file)...\n");
    close(fd2);

    pid_t pid = fork();
    if (pid == -1) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Child process: Try to acquire a conflicting lock.
        // If the parent's lock was wiped, this should succeed.
        int fd3;
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd1);
        
        fd3 = open(procpath, O_RDWR);
        if (fd3 == -1) err(EXIT_FAILURE, "child open %s", procpath);

        if (fcntl(fd3, F_SETLK, &fl) == 0) {
            printf("RESULT: Lock was WIPED by close() of unrelated FD (Child acquired lock).\n");
            exit(0); 
        } else {
            if (errno == EAGAIN || errno == EACCES) {
                printf("RESULT: Lock PERSISTED (Child was blocked).\n");
                exit(1);
            } else {
                perror("child fcntl");
                exit(1);
            }
        }
    }

    int status;
    waitpid(pid, &status, 0);
    
    close(fd1);
    
    return WEXITSTATUS(status);
}
