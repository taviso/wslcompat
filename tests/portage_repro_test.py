#!/usr/bin/env python3
"""End-to-end regression for the FIOCLEX-on-wrap bug. Mirrors the
shape of portage's _setup_pipes_after_fork without depending on
portage itself.

The failure chain (before the fix):
  1. openpty() returns slave_fd with CLOEXEC=1 (Python default).
  2. tcsetattr triggers tty_install; the wrap fd inherits CLOEXEC=1.
  3. Worker calls os.set_inheritable(slave_fd, True) -- which uses
     ioctl(FIONCLEX) internally. Our shim forwarded that to the
     backing real_fd, so the wrap's CLOEXEC stayed 1.
  4. Worker dup2(slave_fd, 1/2) then fcntl(F_SETFD, old_fdflags)
     propagates that latent CLOEXEC=1 onto fd 1/2.
  5. execve() closes CLOEXEC fds -> worker has no stdout -> master
     read times out / EIOs.
"""
import asyncio
import fcntl
import os
import pty
import signal
import sys
import termios

signal.alarm(15)

MARKER = b"HELLO FROM WORKER\n"


def worker(slave_fd):
    # Clear CLOEXEC via the ioctl path (set_inheritable -> FIONCLEX).
    os.set_inheritable(slave_fd, True)

    # Propagate the slave's F_GETFD onto fd 1/2, as portage's
    # _setup_pipes does. After the fix, old_flags is 0 here; before
    # the fix it was FD_CLOEXEC and would close fd 1/2 at execve.
    old_flags = fcntl.fcntl(slave_fd, fcntl.F_GETFD)
    os.dup2(slave_fd, 1)
    fcntl.fcntl(1, fcntl.F_SETFD, old_flags)
    os.dup2(slave_fd, 2)
    fcntl.fcntl(2, fcntl.F_SETFD, old_flags)
    os.close(slave_fd)

    os.execvp("printf", ["printf", "%s", MARKER.decode()])


async def main():
    master_fd, slave_fd = pty.openpty()

    # Match portage's _create_pty_or_pipe: clear OPOST so the kernel
    # doesn't transform \n -> \r\n. tcsetattr triggers tty_install
    # under the shim.
    mode = termios.tcgetattr(slave_fd)
    mode[1] &= ~termios.OPOST
    termios.tcsetattr(slave_fd, termios.TCSANOW, mode)

    os.set_blocking(master_fd, False)

    pid = os.fork()
    if pid == 0:
        os.close(master_fd)
        worker(slave_fd)
        os._exit(70)  # only reached if execvp failed

    os.close(slave_fd)

    loop = asyncio.get_event_loop()
    received = bytearray()
    done = asyncio.Event()

    def on_readable():
        try:
            chunk = os.read(master_fd, 4096)
            if not chunk:
                done.set()
                return
            received.extend(chunk)
            if MARKER in received:
                done.set()
        except OSError as e:
            print(f"read error: {e}", flush=True)
            done.set()

    loop.add_reader(master_fd, on_readable)
    try:
        await asyncio.wait_for(done.wait(), timeout=5.0)
    except asyncio.TimeoutError:
        print(f"FAIL: timeout, got {bytes(received)!r}")
        sys.exit(1)
    finally:
        loop.remove_reader(master_fd)
        os.close(master_fd)
        os.waitpid(pid, 0)

    if MARKER not in received:
        print(f"FAIL: marker missing, got {bytes(received)!r}")
        sys.exit(1)

    print(f"PASS: got {bytes(received)!r}")


if __name__ == "__main__":
    asyncio.run(main())
