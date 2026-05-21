#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <elf.h>

#include "shim.h"
#include "tunables.h"
#include "logging.h"

SHIM_INIT(execve);

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

    if (wslcompat_passthru("execve"))
        return sym_next(execve, pathname, argv, envp);

    sym_next(execve, pathname, argv, envp);

    if ((saved_errno = errno) != ENOEXEC)
        return -1;

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
    if (wslcompat_tunable_bool("argv0", true)) {
        new_argv[pos++] = "--argv0";
        new_argv[pos++] = (char *)(argv[0] ? argv[0] : pathname);
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
    errno = saved_errno;
    return -1;
}
