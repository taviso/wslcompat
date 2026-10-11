/* Worker -> wrapped slave -> master -> reader relay.
 *
 * The simplest shape of "build worker output reaches user":
 *   parent: posix_openpt + tcsetattr the slave (triggers tty_install,
 *           wrapping the slave fd with our synth handler)
 *   fork:   child inherits the wrap + the hidden real_fd
 *   child:  write() a known byte stream to the wrapped slave fd
 *   parent: read() from the master, capture, byte-compare with the
 *           sent payload
 *
 * The wrap-write path under test:
 *   write(slave) -> shim write -> kernel write on O_PATH returns
 *   EBADF -> SYNTH_DISPATCH(write) -> tty_ops.passthru(SYNTH_OP_IO)
 *   -> sym_next(write, real_fd, ...) -> real slave -> kernel ->
 *   master buffer.
 *
 * Payload is deliberately mixed (\n, NUL, high-bit) so a line
 * discipline that didn't get cfmakeraw'd, a short-write that lost
 * bytes, or O_NONBLOCK cache desync would show up as a corrupted or
 * truncated receive buffer.
 *
 * If this fails under the shim, that's a likely root cause for
 * emerge's missing build output. If it passes, the basic relay is
 * intact and the emerge issue lives somewhere more specific to
 * portage's machinery (display thread, isatty/buffering, ...).
 *
 * XPASS: should pass with and without the shim. */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PAYLOAD_SIZE 1024

static void fill_payload(char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i % 73 == 0)        p[i] = '\n';
        else if (i % 97 == 0)   p[i] = '\0';
        else if (i % 131 == 0)  p[i] = (char)0xff;
        else                    p[i] = (char)('0' + (i % 10));
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) err(EXIT_FAILURE, "posix_openpt");
    if (grantpt(master) != 0) err(EXIT_FAILURE, "grantpt");
    if (unlockpt(master) != 0) err(EXIT_FAILURE, "unlockpt");

    char *sname = ptsname(master);
    if (sname == NULL) err(EXIT_FAILURE, "ptsname");

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) err(EXIT_FAILURE, "open slave");

    // cfmakeraw + tcsetattr: byte-clean pipe, and the tcsetattr
    // triggers tty_install on the slave fd under the shim.
    struct termios attrs;
    if (tcgetattr(slave, &attrs) != 0) err(EXIT_FAILURE, "tcgetattr");
    cfmakeraw(&attrs);
    if (tcsetattr(slave, TCSANOW, &attrs) != 0) err(EXIT_FAILURE, "tcsetattr");

    long sflags = syscall(SYS_fcntl, slave, F_GETFL);
    printf("slave fd=%d raw F_GETFL=0x%lx%s\n",
           slave, sflags,
           (sflags >= 0 && (sflags & O_PATH)) ? " [O_PATH wrap]" : " [real fd]");

    char payload[PAYLOAD_SIZE];
    fill_payload(payload, PAYLOAD_SIZE);

    pid_t pid = fork();
    if (pid < 0) err(EXIT_FAILURE, "fork");

    if (pid == 0) {
        // Worker: write the payload to the wrapped slave.
        close(master);

        ssize_t total = 0;
        while (total < PAYLOAD_SIZE) {
            ssize_t n = write(slave, payload + total, PAYLOAD_SIZE - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "worker: write at %zd: %m\n", total);
                _exit(70);
            }
            if (n == 0) {
                fprintf(stderr, "worker: write returned 0 at %zd\n", total);
                _exit(71);
            }
            total += n;
        }
        close(slave);
        _exit(0);
    }

    // Reader: pull bytes off the master concurrently with the worker
    // so the pty buffer (~4 KB) doesn't back-pressure into a hang.
    // We read exactly PAYLOAD_SIZE and stop -- avoids depending on
    // EOF propagation, which requires every ref to the slave OFD
    // (including our hidden real_fd) to be released.
    close(slave);

    char recv[PAYLOAD_SIZE];
    ssize_t total = 0;
    while (total < PAYLOAD_SIZE) {
        ssize_t n = read(master, recv + total, PAYLOAD_SIZE - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            warn("reader: read at %zd", total);
            break;
        }
        if (n == 0)
            break;
        total += n;
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) err(EXIT_FAILURE, "waitpid");
    close(master);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: worker exit status=%d signal=%d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1,
               WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return 1;
    }

    if (total != PAYLOAD_SIZE) {
        printf("FAIL: received %zd bytes (expected %d)\n", total, PAYLOAD_SIZE);
        return 1;
    }

    for (int i = 0; i < PAYLOAD_SIZE; i++) {
        if (recv[i] != payload[i]) {
            printf("FAIL: byte %d: got 0x%02x, expected 0x%02x\n",
                   i, (unsigned char)recv[i], (unsigned char)payload[i]);
            return 1;
        }
    }

    printf("PASS: %d bytes relayed through wrapped slave intact\n",
           PAYLOAD_SIZE);
    return 0;
}
