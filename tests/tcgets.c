#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    struct termios2 term;
    return ioctl(0, TCGETS2, &term) == 0;
}
