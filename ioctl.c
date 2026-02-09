#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>

static int (*sym_ioctl)(int fd, unsigned long op, ...);

static void __attribute__((constructor)) init(void)
{
    sym_ioctl = dlsym(RTLD_NEXT, "ioctl");
    return;
}

int ioctl(int fd, unsigned long op, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, op);
    arg = va_arg(ap, void *);
    va_end(ap);

    // termios and termios2 are the same except for c_ispeed and c_ospeed.
    if (op == TCGETS2) {
        op = TCGETS;
    }

    return sym_ioctl(fd, op, arg);
}


int isatty(int fd)
{
    struct termios term;
    return sym_ioctl(fd, TCGETS, &term) == 0;
}

int tcgetattr(int fd, struct termios *termios_p)
{
  struct ktermios k_termios;
  int retval;

  retval = sym_ioctl(fd, TCGETS, &k_termios);

  if (retval != 0)
    return retval;

  termios_p->c_iflag = k_termios.c_iflag;
  termios_p->c_oflag = k_termios.c_oflag;
  termios_p->c_cflag = k_termios.c_cflag;
  termios_p->c_lflag = k_termios.c_lflag;
  termios_p->c_line = k_termios.c_line;
  memcpy (&termios_p->c_cc[0], &k_termios.c_cc[0], NCCS * sizeof (cc_t));

  return 0;
}

#define IBAUD0 020000000000

int tcsetattr(int fd, int optional_actions, struct termios *termios_p)
{
  struct ktermios k_termios;
  unsigned long int cmd;


  switch (optional_actions) {
    case TCSANOW:
      cmd = TCSETS;
      break;
    case TCSADRAIN:
      cmd = TCSETSW;
      break;
    case TCSAFLUSH:
      cmd = TCSETSF;
      break;
    default:
      return EINVAL;
  }

  k_termios.c_iflag = termios_p->c_iflag & ~IBAUD0;
  k_termios.c_oflag = termios_p->c_oflag;
  k_termios.c_cflag = termios_p->c_cflag;
  k_termios.c_lflag = termios_p->c_lflag;
  k_termios.c_line = termios_p->c_line;
  memcpy (&k_termios.c_cc[0], &termios_p->c_cc[0],NCCS * sizeof (cc_t));
  return sym_ioctl(fd, cmd, &k_termios);
}
