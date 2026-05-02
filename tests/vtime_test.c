#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <err.h>

void handle_alarm(int sig) {
    fprintf(stderr, "\nFAIL: read() blocked indefinitely. Kernel ignores VTIME.\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    const char *dev = (argc == 2) ? argv[1] : "/dev/ptmx";
    
    // Open non-blocking to prevent hanging on carrier detect (e.g., /dev/ttyS0)
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Restore blocking mode for the read() test
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios t;
    if (ioctl(fd, TCGETS, &t) < 0) {
        perror("TCGETS");
        close(fd);
        return 1;
    }

    t.c_lflag &= ~ICANON;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 5; // 0.5 seconds

    if (ioctl(fd, TCSETS, &t) < 0) {
        perror("TCSETS");
        close(fd);
        return 1;
    }

    // Check if the kernel stores the state (viable for stateless shim)
    struct termios t_check = {0};
    if (ioctl(fd, TCGETS, &t_check) < 0) {
        perror("TCGETS validation");
        close(fd);
        return 1;
    }

    if (t_check.c_cc[VMIN] == 0 && t_check.c_cc[VTIME] == 5) {
        printf("STATE PASS: Kernel stores c_cc. Stateless shim will work.\n");
    } else {
        printf("STATE FAIL: Kernel discards c_cc (VMIN=%d, VTIME=%d). Shim must track state.\n",
               t_check.c_cc[VMIN], t_check.c_cc[VTIME]);
    }

    // Check if the kernel actually honors the read timeout
    struct sigaction sa = { .sa_handler = handle_alarm };
    sigaction(SIGALRM, &sa, NULL);
    alarm(2); 

    struct timeval start, end;
    gettimeofday(&start, NULL);

    char buf[1];
    int res = read(fd, buf, 1);

    alarm(0); // Cancel alarm if read() returns
    gettimeofday(&end, NULL);

    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + 
                      (end.tv_usec - start.tv_usec) / 1000;

    printf("read() returned: %d\n", res);
    printf("Elapsed time: %ld ms\n", elapsed_ms);

    if (elapsed_ms >= 400 && elapsed_ms <= 600) {
        printf("READ PASS: Kernel honors VTIME during read().\n");
    } else {
        err(EXIT_FAILURE, "READ FAIL: Kernel returns prematurely or ignores VTIME.\n");
    }

    close(fd);
    return 0;
}
