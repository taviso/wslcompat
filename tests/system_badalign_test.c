#include <stdlib.h>
#include <stdio.h>
int main() {
    int r = system("./sys_badalign_test");
    printf("system returned %d\n", r);
    return 0;
}
