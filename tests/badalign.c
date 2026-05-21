/*
 * WSL1 rejects ELF64 binaries with ENOEXEC when all PT_LOAD headers don't share the same p_align.
 *
 */

__attribute__((section(".align_a"), aligned(0x4000), used))
static const char marker_a[1] = { 0 };

__attribute__((section(".align_b"), aligned(0x8000), used))
static const char marker_b[1] = { 0 };

int main(void)
{
    return 0;
}
