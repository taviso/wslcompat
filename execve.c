#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <gnu/libc-version.h>
#include <elf.h>
#include <spawn.h>
#include <limits.h>

#include "shim.h"
#include "synth.h"
#include "tunables.h"
#include "logging.h"
#include "utils.h"

SHIM_INIT(execve, fexecve, execv, execvp, execvpe,
          system, popen, posix_spawn, posix_spawnp);

SHIM_ALIAS(execve);

// There are two main problems we need to handle with the execv* family.
//
// 1) The wsl1 ELF loader rejects some valid ELF files with ENOEXEC, we can
//    work around this by calling the interpreter directly.
// 2) If the program uses synthetic fds, the new process is going to have
//    no idea what the heck they just inherited, so we need to replace them.
//
// 1 is pretty easy, we just catch that errno and fix up argv to include the
// loader.
//
// 2 is a bit messy.

// Boilerplate for exec wrappers that REPLACE the process on success
#define EXEC_REPLACE_SHIM(call, ...) do {                               \
    struct synth_preexec_state _px = { .count = 0 };                    \
    if (wslcompat_passthru(#call))                                      \
        return sym_next(call, __VA_ARGS__);                             \
    wslcompat_synth_preexec_save(&_px);                                 \
    sym_next(call, __VA_ARGS__);                                        \
    preserve_errno();                                                   \
    wslcompat_synth_preexec_restore(&_px);                              \
    return -1;                                                          \
} while (0)

// varargs-to-argv expansion shared by execl/execlp/execle. The fixed
// stack array matches execve.c's existing MAX_NEW_ARGV ceiling.
// Note: could also use __builtin_apply?
static void va_collect_argv(va_list ap, const char *arg0,
                            char **out, size_t cap)
{
    size_t i = 0;
    out[i++] = (char *)arg0;
    while (i < cap - 1) {
        out[i] = va_arg(ap, char *);
        if (out[i] == NULL)
            return;
        i++;
    }
    out[cap - 1] = NULL;
}

#define MAX_NEW_ARGV     1024
#define PTINTERP_DEFAULT "/lib64/ld-linux-x86-64.so.2"

// WSL1's execve rejects ELF64 binaries when their PT_LOAD headers don't all share the same p_align.
static bool elf_needs_fixup(int fd, const char *pathname)
{
    Elf64_Ehdr  ehdr;
    Elf64_Phdr  phdr;
    bool        mixed = false;
    bool        dynamic = false;
    uint64_t    align = 0;

    if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
        return false;
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0)
        return false;
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64)
        return false;
    if (ehdr.e_phnum == 0 || ehdr.e_phentsize != sizeof(Elf64_Phdr))
        return false;

    wsldbg("%s is an Elf64 binary, examining %u phdrs", pathname, ehdr.e_phnum);

    for (size_t i = 0; i < ehdr.e_phnum; i++) {
        if (pread(fd, &phdr, sizeof(phdr), ehdr.e_phoff + i * sizeof(phdr)) != sizeof(phdr))
            return false;

        if (phdr.p_type == PT_LOAD) {
            // Check if we have a base alignment yet.
            align = (align == 0) ? phdr.p_align : align;

            // All alignments must match.
            mixed |= (phdr.p_align != align);

            wsldbg("phdr %lu PT_LOAD with alignment %#lx, mixed=%d", i, phdr.p_align, mixed);
        }

        // Confirm dynamic executable.
        dynamic |= (phdr.p_type == PT_INTERP);
    }

    if (mixed == true && dynamic == false) {
        wslwarn("Non-uniform Phdr alignment; try 'elfclamp %s' to adjust.", pathname);
        return false;
    }

    wsldbg("elf tests complete: mixed=%d, dynamic=%d", mixed, dynamic);
    return mixed;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    char       *new_argv[MAX_NEW_ARGV] = {0};
    char *const empty[] = { NULL };
    const char *interp;
    int         saved_errno;
    int         fd = -1;
    size_t      pos = 0;
    struct stat st;
    struct synth_preexec_state preexec = { .count = 0 };

    if (wslcompat_passthru_self())
        return sym_next(execve, pathname, argv, envp);

    // Our process-local synth state doesn't survive exec; swap real
    // backing fds over each non-CLOEXEC wrap so the new image sees
    // usable descriptors. preexec records what it did so the failure
    // path can put everything back.
    wslcompat_synth_preexec_save(&preexec);

    sym_next(execve, pathname, argv, envp);

    if ((saved_errno = errno) != ENOEXEC)
        goto fail;

    wsldbg("ENOEXEC detected for %s", pathname);

    if ((fd = open(pathname, O_RDONLY | O_CLOEXEC)) < 0) {
        wsldbg("cannot open() %s to check ELF headers", pathname);
        goto fail;
    }

    // Refuse to rewrite setuid/setgid binaries.
    if (fstat(fd, &st) != 0 || (st.st_mode & (S_ISUID | S_ISGID))) {
        wslwarn("cannot determine file mode for %s", pathname);
        goto fail;
    }

    // Test if this is an ELF we can fix.
    if (elf_needs_fixup(fd, pathname) != true) {
        wsldbg("cannot confirm %s needs alignment fix", pathname);
        goto fail;
    }

    // Handle empty argv, which linux accepts.
    if (argv == NULL) {
        argv = empty;
    }

    interp = wslcompat_tunable_str("ptinterp", PTINTERP_DEFAULT);

    // Execute via the interpreter, bypassing the kernel's requirements.
    new_argv[pos++] = (char *) interp;

    // Optionally use the new --argv0 feature since glibc 2.33.
    if (strverscmp(gnu_get_libc_version(), "2.33") >= 0) {
        if (wslcompat_tunable_bool("argv0", true)) {
            new_argv[pos++] = "--argv0";
            new_argv[pos++] = (char *)(argv[0] ? argv[0] : pathname);
        }
    }

    new_argv[pos++] = (char *) pathname;

    // Copy parameters over.
    for (size_t i = 0; argv[i];) {
        new_argv[pos++] = argv[++i];

        // Verify we can fit these parameters
        if (pos >= MAX_NEW_ARGV) {
            saved_errno = E2BIG;
            goto fail;
        }
    }

    wsllog("re-exec '%s' via '%s' (mixed PT_LOAD p_align workaround)", pathname, interp);

    sym_next(execve, interp, new_argv, envp);

    // execve failed, use the real errno.
    saved_errno = errno;

    wsldbg("re-exec failed, returning %m");

  fail:
    if (fd != -1)
        close(fd);
    wslcompat_synth_preexec_restore(&preexec);
    return errno = saved_errno, -1;
}

// Apps calling the libc fexecve symbol come through here. Glibc's
// own internal fexecve (used by other libc routines) goes straight
// to a raw syscall(SYS_execveat) and bypasses interposition entirely
// -- nothing we can do about that path.
int fexecve(int fd, char *const argv[], char *const envp[])
{
    EXEC_REPLACE_SHIM(fexecve, fd, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    EXEC_REPLACE_SHIM(execv, path, argv);
}

int execvp(const char *file, char *const argv[])
{
    EXEC_REPLACE_SHIM(execvp, file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    EXEC_REPLACE_SHIM(execvpe, file, argv, envp);
}

// execl / execlp / execle: expand varargs into a stack array and
// hand off to the array-form sibling. Our array shims do the save
// dance, so these stay simple.
int execl(const char *path, const char *arg0, ...)
{
    char *argv[MAX_NEW_ARGV];
    va_list ap;
    va_start(ap, arg0);
    va_collect_argv(ap, arg0, argv, _countof(argv));
    va_end(ap);
    return execv(path, argv);
}

int execlp(const char *file, const char *arg0, ...)
{
    char *argv[MAX_NEW_ARGV];
    va_list ap;
    va_start(ap, arg0);
    va_collect_argv(ap, arg0, argv, _countof(argv));
    va_end(ap);
    return execvp(file, argv);
}

int execle(const char *path, const char *arg0, ...)
{
    char *argv[MAX_NEW_ARGV];
    char **envp;
    va_list ap;
    va_start(ap, arg0);
    va_collect_argv(ap, arg0, argv, _countof(argv));
    envp = va_arg(ap, char **);
    va_end(ap);
    return execve(path, argv, envp);
}

// fork()+exec wrappers: glibc's child calls __execve via direct
// address, so we save before sym_next (the parent's swap propagates
// to the child via fork's fd table copy) and restore on return.
int system(const char *cmd)
{
    struct synth_preexec_state preexec = { .count = 0 };
    int result;
    if (wslcompat_passthru_self())
        return sym_next(system, cmd);
    wslcompat_synth_preexec_save(&preexec);
    result = sym_next(system, cmd);
    preserve_errno();
    wslcompat_synth_preexec_restore(&preexec);
    return result;
}

FILE *popen(const char *cmd, const char *mode)
{
    struct synth_preexec_state preexec = { .count = 0 };
    FILE *fp;

    if (wslcompat_passthru_self())
        return sym_next(popen, cmd, mode);
    wslcompat_synth_preexec_save(&preexec);
    fp = sym_next(popen, cmd, mode);
    preserve_errno();
    wslcompat_synth_preexec_restore(&preexec);
    return fp;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    struct synth_preexec_state preexec = { .count = 0 };
    int result;

    if (wslcompat_passthru_self())
        return sym_next(posix_spawn, pid, path, file_actions, attrp, argv, envp);

    wsldbg("posix_spawn: %s", path);

    // posix_spawn uses CLONE_VFORK -- parent blocks until the child
    // execs (or exits), so when sym_next returns the child has
    // already inherited (and exec'd with) our swapped fds. Restore
    // unconditionally so the parent keeps its wraps.
    wslcompat_synth_preexec_save(&preexec);
    result = sym_next(posix_spawn, pid, path, file_actions, attrp, argv, envp);
    wslcompat_synth_preexec_restore(&preexec);
    return result;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    struct synth_preexec_state preexec = { .count = 0 };
    int result;

    if (wslcompat_passthru_self())
        return sym_next(posix_spawnp, pid, file, file_actions, attrp, argv, envp);

    wsldbg("posix_spawnp: %s", file);

    wslcompat_synth_preexec_save(&preexec);
    result = sym_next(posix_spawnp, pid, file, file_actions, attrp, argv, envp);
    wslcompat_synth_preexec_restore(&preexec);
    return result;
}
