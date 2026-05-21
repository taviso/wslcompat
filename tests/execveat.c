#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <err.h>

int main(int argc, char *argv[], char *envp[])
{
    char *args[] = { "/bin/true", NULL };

    // execveat does not return on success; reaching err() means failure.
    execveat(AT_FDCWD, "/bin/true", args, envp, 0);

    err(EXIT_FAILURE, "execveat");
}
