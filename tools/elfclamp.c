#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>

/**
 * Clamps ELF64 program header alignment to 4096.
 */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <elf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    size_t size = (size_t)st.st_size;
    if (size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "File too small to be ELF64\n");
        close(fd);
        return EXIT_FAILURE;
    }

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;

    // Verify ELF64 header
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

    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)mem + ehdr->e_phoff);
    int clamped_count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_align > 4096) {
            printf("Clamping p_align of header %d from 0x%lx to 0x1000\n", 
                   i, (unsigned long)phdr[i].p_align);
            phdr[i].p_align = 4096;
            clamped_count++;
        }
    }

    if (clamped_count > 0) {
        printf("Clamped %d program headers.\n", clamped_count);
        if (msync(mem, size, MS_SYNC) < 0) {
            perror("msync");
        }
    } else {
        printf("No program headers required clamping.\n");
    }

cleanup:
    munmap(mem, size);
    close(fd);
    return EXIT_SUCCESS;
}
