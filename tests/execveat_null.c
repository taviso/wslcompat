#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <err.h>
#include <stdio.h>

int main(int argc, char *argv[], char *envp[])
{
    int fd;
    char *args[] = { "true", NULL };

    if (argc > 1) {
        printf("Successfully executed via execveat with NULL path!\n");
        return 0;
    }

    fd = open("/bin/true", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        err(EXIT_FAILURE, "open /bin/true");

    // Re-exec ourselves with NULL path and AT_EMPTY_PATH
    // We use a dummy argument to detect we've been re-executed.
    char *new_args[] = { argv[0], "reexeced", NULL };
    
    // Note: We're executing /bin/true, but passing our own argv[0] is just for the test.
    // Actually, let's just exec /bin/true directly to be simpler.
    char *true_args[] = { "true", NULL };
    
    printf("Attempting execveat with NULL path...\n");
    execveat(fd, NULL, true_args, envp, AT_EMPTY_PATH);

    err(EXIT_FAILURE, "execveat");
}
