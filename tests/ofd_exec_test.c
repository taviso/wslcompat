#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <string.h>

int main(int argc, char *argv[]) {
    struct flock fl1 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 10 };
    struct flock fl2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 20, .l_len = 10 };

    if (argc > 1 && strcmp(argv[1], "helper") == 0) {
        int fd1 = atoi(argv[2]);
        int fd2 = atoi(argv[3]);
        printf("--- Subtest: exec recovery helper (fd1=%d, fd2=%d) ---\n", fd1, fd2);

        if (fcntl(fd1, F_OFD_SETLK, &fl1) == -1) err(EXIT_FAILURE, "helper fcntl fd1");
        if (fcntl(fd2, F_OFD_SETLK, &fl2) == -1) err(EXIT_FAILURE, "helper fcntl fd2");
        
        printf("Success: Both IDs recovered after exec.\n");
        return 0;
    }

    printf("--- Subtest: exec() persistence and ambiguity ---\n");
    char tmpfile[] = "/tmp/ofd_exec_XXXXXX";
    int fd1 = mkostemp(tmpfile, O_RDWR);
    int fd2 = dup(fd1);
    if (fd1 == -1 || fd2 == -1) err(EXIT_FAILURE, "setup");
    unlink(tmpfile);

    if (fcntl(fd1, F_OFD_SETLK, &fl1) == -1) err(EXIT_FAILURE, "parent fcntl 1");
    if (fcntl(fd2, F_OFD_SETLK, &fl2) == -1) err(EXIT_FAILURE, "parent fcntl 2");

    char s1[10], s2[10];
    snprintf(s1, sizeof(s1), "%d", fd1);
    snprintf(s2, sizeof(s2), "%d", fd2);

    printf("Execing self...\n");
    char *args[] = {argv[0], "helper", s1, s2, NULL};
    execv(args[0], args);
    err(EXIT_FAILURE, "execv");
}
