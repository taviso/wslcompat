#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>

/**
 * Unify the p_align values across PT_LOAD program headers in an ELF64
 * binary, working around WSL1's exec bug that rejects binaries with
 * mixed PT_LOAD p_align values with ENOEXEC.
 */

int main(int argc, char *argv[])
{
    int          fd;
    void        *mem;
    struct stat  st;
    size_t       size;
    Elf64_Ehdr  *ehdr;
    Elf64_Phdr  *phdr;
    uint64_t     min_align;
    int          load_count;
    int          changed;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <elf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if ((fd = open(argv[1], O_RDWR)) < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    size = (size_t)st.st_size;
    if (size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "File too small to be ELF64\n");
        close(fd);
        return EXIT_FAILURE;
    }

    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    ehdr = (Elf64_Ehdr *)mem;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "Not an ELF file\n");
        goto cleanup;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "Not an ELF64 file\n");
        goto cleanup;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        fprintf(stderr, "No program headers found\n");
        goto cleanup;
    }

    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        fprintf(stderr, "Unexpected program header size: %u\n", ehdr->e_phentsize);
        goto cleanup;
    }

    if (ehdr->e_phoff + ((uint64_t)ehdr->e_phnum * ehdr->e_phentsize) > size) {
        fprintf(stderr, "Program headers out of bounds\n");
        goto cleanup;
    }

    phdr = (Elf64_Phdr *)((uint8_t *)mem + ehdr->e_phoff);

    min_align  = 0;
    load_count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        load_count++;
        if (min_align == 0 || phdr[i].p_align < min_align)
            min_align = phdr[i].p_align;
    }

    if (load_count == 0) {
        printf("No PT_LOAD program headers found.\n");
        goto cleanup;
    }

    changed = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        if (phdr[i].p_align == min_align)
            continue;
        printf("Unifying p_align of header %d from 0x%lx to 0x%lx\n",
               i, (unsigned long)phdr[i].p_align, (unsigned long)min_align);
        phdr[i].p_align = min_align;
        changed++;
    }

    if (changed > 0) {
        printf("Unified %d program header(s) to p_align 0x%lx.\n",
               changed, (unsigned long)min_align);
        if (msync(mem, size, MS_SYNC) < 0)
            perror("msync");
    } else {
        printf("All PT_LOAD p_align values are already 0x%lx; nothing to do.\n",
               (unsigned long)min_align);
    }

cleanup:
    munmap(mem, size);
    close(fd);
    return EXIT_SUCCESS;
}
