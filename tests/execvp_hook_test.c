#include <stdio.h>
#include <unistd.h>
int __execve(const char *pathname, char *const argv[], char *const envp[]) {
    printf("HOOKED __execve %s\n", pathname);
    return -1;
}
int main() {
    char *argv[] = { "ls", NULL };
    execvp("ls", argv);
    return 0;
}
