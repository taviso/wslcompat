CFLAGS = -O0 -ggdb3
LDFLAGS =
CPPFLAGS =
LDLIBS = -lrt
VGFLAGS = --quiet --error-exitcode=2 --suppressions=valgrind.supp

# Default to parallel builds, these tests can be slow.
MAKEFLAGS += -j$(shell nproc)

# Fail early with a clear message if the test prerequisites are missing.
$(foreach tool,valgrind patchelf,                                              \
    $(if $(shell command -v $(tool) 2>/dev/null),,                             \
        $(error required tool '$(tool)' not found in $$PATH)))

.PHONY: clean test linux

# These tests should fail without libwslcompat, and then pass when with.
TESTS = mmap statx sockaddr socktest vtime_test mincore init_order multiproc   \
        mlock_test ofd_block ofd_exec_test ofd_lifecycle ofd_multi             \
        linux_lock_test ofd_hybrid_test ofd_interop_test                       \
        posix_vs_posix posix_vs_posix_local sqlite_lock renameat2              \
        fcntl_getlk_proposed fcntl_upgrade fcntl_eintr vmin_test elf_test      \
        execveat clock_nanosleep_cputime reuseport_test null_arg_test          \
        tmpfile_test btime_test statx_edges_test mmap_min_addr_probe

# These test are expected failures (may implement in future).
XFAIL = name_to_handle_at memfd_create ofd_range ofd_range_hybrid ofd_compat   \
        ofd posix_standard fcntl_stress_test kcmp_test ofd_pid_test            \
        posix_pid_test cputime_signal_test rlimit_cpu_test rlimit_cpu_hard_test \
        tmpfile_linkat_test

# These tests should pass both with and without the shim.
XPASS = shm_test inode_test flock_vs_fcntl flock_standard tcgets proc_test     \
        unlinked_test verify_xattr fcntl_unlock_noop filetime_test            \
        proc_cputime_test times_test execve_null_argv_test o_path_test

test: $(addprefix chk_,$(TESTS)) $(addprefix xfl_,$(XFAIL)) $(addprefix xps_,$(XPASS)) ../libwslcompat.so
	@echo ====================
	@echo = ALL TESTS PASSED =
	@echo ====================

linux: $(addprefix lin_,$(TESTS) $(XFAIL) $(XPASS))
	@echo ====================
	@echo = ALL TESTS PASSED =
	@echo ====================

# Check that the unpatched syscalls succeed on native linux.
lin_%: sys_%
	$(addprefix ./, $<)

# Check that the patched syscalls succeed.
chk_%: fix_% xfl_% pre_% val_%
	$(addprefix ./, $<)

# Check that both patched and unpatched syscalls succeed.
xps_%: sys_% fix_% pre_% val_%
	$(addprefix ./, $(word 1, $^))
	$(addprefix ./, $(word 2, $^))

# Check that the unpatched syscalls fail
xfl_%: sys_%
	@echo NOTE: ERRORS EXPECTED BELOW
	@echo ===========================
	! $(addprefix ./, $<)
	@echo ===========================

# All fixes should also work as a preload library.
pre_%: sys_% ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so $(addprefix ./,$<)

# Inject the library as a DT_NEEDED tag, and verify the test pases.
fix_%: sys_% ../libwslcompat.so
	patchelf --add-needed ../libwslcompat.so --output $@ $<

sys_%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Run under valgrind to check for memory errors.
val_%: fix_%
	valgrind $(VGFLAGS) $(addprefix ./, $<)

# Generate a log of what happens.
%.log: sys_% ../libwslcompat.so
	strace -E LD_PRELOAD=../libwslcompat.so -o $@ $(addprefix ./,$<)

chk_%: %.py ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so python3 $<

chk_%: %.sh ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so bash $<

lin_%: %.py
	python3 $<

lin_%: %.sh
	bash $<

# The ELF initialization order test.
sys_init_order: init_lib.o init_order.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@.so $(word 1, $^) $(LDLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(word 2, $^) $@.so -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

# The mixed-alignment ELF used by elf_test.sh.
chk_elf_test lin_elf_test: sys_badalign

clean:
	rm -f a.out *.o *.log fix_* sys_*
