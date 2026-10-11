#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

int main() {
    printf("Main execution started.\n");

    // Check libwslcompat was loaded
    if (mincore(NULL, 0, 0) != 0)
        return 1;

    return 0;
}
