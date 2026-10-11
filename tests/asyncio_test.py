#!/usr/bin/env python3
"""Smoke test for asyncio.loop.run_until_complete under the shim.

Python's asyncio default selector on Linux is EpollSelector. Real apps
(observed in portage emerge) have hung in the loop with a stack trace
pointing at selectors.select -> epoll.poll. Three subtests escalate
the amount of event-loop machinery exercised:

  1. simple     - immediate coroutine, no I/O.
  2. sleep      - timer only, selector.select called with a timeout.
  3. subprocess - real subprocess, registers the wakeup pipe with
                  epoll and waits for SIGCHLD via that pipe.

A per-subtest SIGALRM watchdog terminates the process rather than
hanging the test runner indefinitely.
"""

import asyncio
import ctypes
import os
import pty
import select
import signal
import sys
import termios

WATCHDOG = 5  # seconds per subtest

failures = []

def check(name, fn):
    signal.alarm(WATCHDOG)
    try:
        fn()
    except BaseException as e:
        print(f"FAIL: {name}: {type(e).__name__}: {e}", file=sys.stderr)
        failures.append(name)
        return
    finally:
        signal.alarm(0)
    print(f"PASS: {name}")


def t_simple():
    async def coro():
        return 42
    loop = asyncio.new_event_loop()
    try:
        result = loop.run_until_complete(coro())
    finally:
        loop.close()
    assert result == 42, f"simple coro returned {result}, expected 42"


def t_sleep():
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(asyncio.sleep(0.05))
    finally:
        loop.close()


def t_subprocess():
    async def runproc():
        proc = await asyncio.create_subprocess_exec(
            "/bin/true",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        return await proc.wait()
    loop = asyncio.new_event_loop()
    try:
        rc = loop.run_until_complete(runproc())
    finally:
        loop.close()
    assert rc == 0, f"/bin/true returned {rc}, expected 0"


# Real pipe registered with epoll: the parent reads stdout via a stream,
# which is the path most apps (including emerge) actually take.
def t_subprocess_pipe():
    async def runproc():
        proc = await asyncio.create_subprocess_exec(
            "/bin/echo", "hello",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        out, _ = await proc.communicate()
        return proc.returncode, out
    loop = asyncio.new_event_loop()
    try:
        rc, out = loop.run_until_complete(runproc())
    finally:
        loop.close()
    assert rc == 0, f"/bin/echo returned {rc}"
    assert out.strip() == b"hello", f"unexpected output: {out!r}"


# The pipe cases above never touch our shim's translate path because
# pipes are real kernel fds. To exercise the synth-fd canary in
# epoll_ctl we need a tty: tcsetattr on the slave triggers our ioctl
# shim's tty_install, which dup2s an O_PATH synth over the slave fd.
# loop.add_reader(slave) then calls epoll_ctl on the synth, hitting the
# EPERM canary and forcing our translate-and-retry path.
def t_pty_reader():
    master, slave = pty.openpty()
    try:
        # tcsetattr triggers the shim's tty_install for the slave fd.
        # Also clear ICANON/ECHO so a single byte from master shows up
        # at the next read (otherwise canonical mode would buffer until
        # newline and the test would hang on native Linux too).
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~(termios.ICANON | termios.ECHO)  # lflag
        termios.tcsetattr(slave, termios.TCSANOW, attrs)

        loop = asyncio.new_event_loop()
        try:
            future = loop.create_future()
            def on_data():
                if not future.done():
                    future.set_result(os.read(slave, 64))
            loop.add_reader(slave, on_data)
            os.write(master, b"x")
            data = loop.run_until_complete(future)
            loop.remove_reader(slave)
        finally:
            loop.close()
        assert data == b"x", f"expected b'x', got {data!r}"
    finally:
        os.close(master)
        os.close(slave)


# Closer to what emerge does: two pty wraps registered with the same
# event loop. If our translate ever conflates real_fds across wraps,
# epoll_wait could end up waiting on a stale registration forever.
def t_two_pty_readers():
    pair1 = pty.openpty()
    pair2 = pty.openpty()
    try:
        for slave in (pair1[1], pair2[1]):
            attrs = termios.tcgetattr(slave)
            attrs[3] &= ~(termios.ICANON | termios.ECHO)
            termios.tcsetattr(slave, termios.TCSANOW, attrs)

        loop = asyncio.new_event_loop()
        try:
            f1 = loop.create_future()
            f2 = loop.create_future()
            def on(future, slave):
                if not future.done():
                    future.set_result(os.read(slave, 64))
            loop.add_reader(pair1[1], on, f1, pair1[1])
            loop.add_reader(pair2[1], on, f2, pair2[1])
            os.write(pair1[0], b"a")
            os.write(pair2[0], b"b")
            d1, d2 = loop.run_until_complete(asyncio.gather(f1, f2))
            loop.remove_reader(pair1[1])
            loop.remove_reader(pair2[1])
        finally:
            loop.close()
        assert d1 == b"a", f"slave1 expected b'a', got {d1!r}"
        assert d2 == b"b", f"slave2 expected b'b', got {d2!r}"
    finally:
        for fd in (*pair1, *pair2):
            os.close(fd)


# emerge spawns ebuilds via a shell. /bin/echo is just one exec; a
# shell is two (parent -> sh -> id) and exercises more startup work
# than a plain binary. If something we do in execve preexec or
# inheritance trips up bash launched via asyncio, this catches it.
def _shell_pipe(shell):
    async def runproc():
        proc = await asyncio.create_subprocess_exec(
            shell, "-c", "id",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        out, _ = await proc.communicate()
        return proc.returncode, out
    loop = asyncio.new_event_loop()
    try:
        rc, out = loop.run_until_complete(runproc())
    finally:
        loop.close()
    assert rc == 0, f"{shell} -c 'id' returned {rc}"
    assert b"uid=" in out, f"unexpected id output: {out!r}"


def t_sh_id():
    _shell_pipe("/bin/sh")


def t_bash_id():
    if not os.path.exists("/bin/bash"):
        print("  (no /bin/bash, skipping body)")
        return
    _shell_pipe("/bin/bash")


# Hypothesis: if a synth tty's real_fd gets relocated AFTER epoll_ctl
# ADD has already registered (file, real_fd), the kernel auto-removes
# that registration when real_fd is closed -- but our shim doesn't
# re-register the new fd. epoll_wait then blocks indefinitely.
#
# Force the scenario by reaching into the shim via ctypes to find the
# real_fd backing the slave, then close() it directly. close.c's
# relocate_internal -> tty_relocate path should fire, F_DUPFD'ing the
# backing to a fresh number while the original close goes through.
# If the hypothesis holds, the subsequent write+read cycle will hang.
SYNTH_OP_POLL = 0

def _try_load_passthru():
    try:
        lib = ctypes.CDLL(None)
        f = lib.wslcompat_synth_passthru
        f.argtypes = [ctypes.c_int, ctypes.c_int]
        f.restype = ctypes.c_int
        return f
    except (AttributeError, OSError):
        return None


def t_pty_relocate_after_epoll_add():
    passthru = _try_load_passthru()
    if passthru is None:
        print("  (libwslcompat not loaded, skipping)")
        return

    master, slave = pty.openpty()
    try:
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~(termios.ICANON | termios.ECHO)
        termios.tcsetattr(slave, termios.TCSANOW, attrs)

        real_fd = passthru(slave, SYNTH_OP_POLL)
        assert real_fd >= 0, f"no passthru fd for slave={slave}"

        loop = asyncio.new_event_loop()
        try:
            future = loop.create_future()
            def on_data():
                if not future.done():
                    future.set_result(os.read(slave, 64))
            loop.add_reader(slave, on_data)

            # Force the relocate. tty_relocate fires inside our close()
            # shim because the closed fd matches real_fd.
            os.close(real_fd)
            new_real_fd = passthru(slave, SYNTH_OP_POLL)
            print(f"  real_fd was {real_fd}, now {new_real_fd}",
                  file=sys.stderr)

            os.write(master, b"x")
            data = loop.run_until_complete(future)
            loop.remove_reader(slave)
        finally:
            loop.close()
        assert data == b"x", f"expected b'x', got {data!r}"
    finally:
        for fd in (master, slave):
            try:
                os.close(fd)
            except OSError:
                pass


# Hypothesis: when tty_install wraps a pty slave, it F_DUPFD's the
# slave OFD into a private real_fd. That dup keeps the slave OFD
# alive even after the user closes their wrap fd. Master fd then
# never sees POLLHUP when the child closes its end, because our
# hidden real_fd is still referencing the slave. This matches the
# emerge strace: parent close(9) on the wrap, but the liveness probe
# still says "alive" and master epoll_wait blocks forever.
def t_pty_master_pollhup_after_child():
    master, slave = pty.openpty()
    closed = set()
    def safe_close(fd):
        if fd not in closed:
            try:
                os.close(fd)
            except OSError:
                pass
            closed.add(fd)
    try:
        # Triggers our tty_install on slave -> hidden F_DUPFD real_fd.
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~(termios.ICANON | termios.ECHO)
        termios.tcsetattr(slave, termios.TCSANOW, attrs)

        # Fork a child that holds slave as stdout, then exits.
        pid = os.fork()
        if pid == 0:
            try:
                os.dup2(slave, 1)
            finally:
                os._exit(0)

        # Parent: drop our visible references to the slave. If our
        # shim is well-behaved, the only remaining slave OFD ref is
        # the child's fd 1, which goes away on exit.
        safe_close(slave)
        os.waitpid(pid, 0)

        # Master should now see POLLHUP. Bound the wait so a stuck
        # implementation surfaces as a test failure, not a hang.
        ep = select.epoll()
        try:
            ep.register(master, select.EPOLLIN | select.EPOLLHUP)
            events = ep.poll(timeout=2.0)
            assert events, \
                f"master {master} never reported POLLHUP after child exit -- " \
                "slave OFD is being held alive by something"
            (_, evt), = events
            assert evt & (select.EPOLLHUP | select.EPOLLIN), \
                f"unexpected events 0x{evt:x}"
            ep.unregister(master)
        finally:
            ep.close()
    finally:
        safe_close(master)
        safe_close(slave)


check("simple", t_simple)
check("sleep", t_sleep)
check("subprocess", t_subprocess)
check("subprocess_pipe", t_subprocess_pipe)
check("pty_reader", t_pty_reader)
check("two_pty_readers", t_two_pty_readers)
check("sh_id", t_sh_id)
check("bash_id", t_bash_id)
check("pty_relocate_after_epoll_add", t_pty_relocate_after_epoll_add)
check("pty_master_pollhup_after_child", t_pty_master_pollhup_after_child)

sys.exit(1 if failures else 0)
