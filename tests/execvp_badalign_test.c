#include <stdio.h>
#include <unistd.h>
int main() {
    char *argv[] = { "sys_badalign_test", NULL };
    execvp("./sys_badalign_test", argv);
    perror("execvp");
    return 1;
}
