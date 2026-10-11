#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    struct termios2 term;

    // Stdin is not a tty (CI, piped shell, etc.); nothing to test.
    if (!isatty(0))
        return 0;

    return ioctl(0, TCGETS2, &term) != 0;
}
