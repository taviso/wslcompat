CFLAGS = -O0 -ggdb3 -MMD -MP
LDFLAGS =
CPPFLAGS =
LDLIBS = -ldl
VGFLAGS = --quiet --error-exitcode=2 --suppressions=valgrind.supp
VALGRIND = valgrind

all: test

# Pull in gcc-generated header dependency info (-MMD writes a .d
# alongside each .o). The leading `-` silences first-build misses.
-include $(wildcard *.d)

# Default to parallel builds, these tests can be slow.
MAKEFLAGS += -j$(shell nproc) --silent

# Fail early with a clear message if the test prerequisites are missing.
$(foreach tool,valgrind patchelf,                                              \
    $(if $(shell command -v $(tool) 2>/dev/null),,                             \
        $(error required tool '$(tool)' not found in $$PATH)))

.PHONY: clean test linux

# These tests should fail without libwslcompat, and then pass when with.
TESTS = mmap_test statx_test sockaddr_test socktest vtime_test mincore_test    \
        init_order_test multiproc_test mlock_test ofd_block_test               \
        ofd_exec_test ofd_lifecycle_test ofd_multi_test linux_lock_test        \
        ofd_hybrid_test ofd_interop_test posix_vs_posix_test                   \
        posix_vs_posix_local_test sqlite_lock_test renameat2_test              \
        fcntl_getlk_proposed_test fcntl_upgrade_test fcntl_eintr_test          \
        vmin_test elf_test execveat_test clock_nanosleep_cputime_test          \
        reuseport_test null_arg_test tmpfile_test btime_test                   \
        statx_edges_test mmap_min_addr_probe_test synth_stress_test            \
        proc_pid_mem_test mountinfo_relocate_test synth_table_gc_test          \
        copy_file_range_test syscall_redirect_test close_range_test            \
        epoll_pwait2_test fcntl_synth_test open_coverage_test

# These test are expected failures (may implement in future).
XFAIL = name_to_handle_at_test memfd_create_test ofd_range_test                \
        ofd_range_hybrid_test ofd_compat_test ofd_test posix_standard_test     \
        fcntl_stress_test kcmp_test ofd_pid_test posix_pid_test                \
        cputime_signal_test rlimit_cpu_test fcntl_setown_ex_test               \
        tmpfile_linkat_test rlimit_cpu_hard_test                               \
        sync_file_range_test so_timestamp_test

# These tests should pass both with and without the shim.
XPASS = shm_test inode_test flock_vs_fcntl_test flock_standard_test            \
        tcgets_test proc_test unlinked_test verify_xattr_test                  \
        fcntl_unlock_noop_test filetime_test proc_cputime_test times_test      \
        execve_null_argv_test o_path_test fdupfd_cloexec_test tty_wrap_test    \
        tty_poll_test poll_overflow_test poll_edge_cases_test                  \
        synth_shared_backing_test synth_enfile_test dup_self_test              \
        exec_preserves_tty_test exec_wrap_diag_test exec_preexec_test          \
        posix_spawn_preexec_test exec_wrappers_test exec_rlimit_test           \
        vfork_stress_test fortify_chk_test posix_fadvise_synth_test            \
        epoll_synth_test probe_semantics_test inotify_path_close_test          \
        inotify_basic_test mmap_synth_test                                     \
        vfork_fd_share_test pty_master_pollhup_test pty_childclose_alive_test  \
        asyncio_test synth_io_ops_test epoll_after_fork_test                   \
        pty_childclose_poll_test poll_extremes_test poll_fork_order_test       \
        pty_childclose_select_test select_extremes_test                        \
        select_fork_order_test epoll_fork_order_test tty_deep_fork_test        \
        tty_relay_test ioctl_cloexec_test portage_repro_test                   \
        poll_signal_eintr_test synth_fork_release_test                         \
        synth_fork_pause_test tty_lock_test internal_stress_test               \
        tc_wrappers_test logging_test open_tmpfile_fortify_test                \
        fstat64_synth_test tcgets2_wrap_test

# Known regressions: pass without the shim, fail with it.
XFIXME = clone_files_corrupt_test clone_vm_slots_test                          \
         sendfile_synth_test splice_synth_test                                 \
         fcntl_close_wipe_test synth_op_gaps_test                              \
         select_high_fd_bug_test poll_swap_overflow_test                       \
         renameat2_dir_test

# When run as root (sudo make), swap to the cross-privilege suite.
ifeq ($(shell id -u),0)
    VALGRIND = true
    TESTS    =
    XFAIL    =
    XPASS    = priv_drop_exec_test priv_close_loop_exec_test
    XFIXME   = priv_mmap_test
endif

test: $(addprefix chk_,$(TESTS)) $(addprefix xfl_,$(XFAIL))     \
      $(addprefix xps_,$(XPASS)) $(addprefix xfm_,$(XFIXME))    \
      ../libwslcompat.so
	@echo ====================
	@echo = ALL TESTS PASSED =
	@echo ====================

linux: $(addprefix lin_,$(TESTS) $(XFAIL) $(XPASS) $(XFIXME))
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

# Known regression: unpatched must pass, patched must fail. If the
# patched run starts passing we've fixed the bug and need to move the
# test to XPASS.
xfm_%: sys_% fix_%
	@echo NOTE: XFIXME -- KNOWN REGRESSIONS
	./$(word 1,$^)
	@echo NOTE: ERRORS EXPECTED BELOW
	@echo ===========================
	! ./$(word 2,$^)
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
	$(VALGRIND) $(VGFLAGS) $(addprefix ./, $<)

# Generate a log of what happens.
%.log: sys_% ../libwslcompat.so
	strace -E LD_PRELOAD=../libwslcompat.so -o $@ $(addprefix ./,$<)

chk_%: %.py ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so python3 $<

chk_%: %.sh ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so bash $<

# Known regression for a .py test: native must pass, preload must fail.
xfm_%: %.py ../libwslcompat.so
	@echo NOTE: XFIXME -- KNOWN REGRESSIONS
	python3 $<
	@echo NOTE: ERRORS EXPECTED BELOW
	@echo ===========================
	! env LD_PRELOAD=../libwslcompat.so python3 $<
	@echo ===========================

xps_%: %.py ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so python3 $<
	python3 $<

lin_%: %.py
	python3 $<

lin_%: %.sh
	bash $<

# The ELF initialization order test.
sys_init_order_test: init_lib_test.o init_order_test.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@.so $(word 1, $^) $(LDLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(word 2, $^) $@.so -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

# Stress test spawns threads.
sys_synth_stress_test: LDLIBS += -lpthread

# vfork stress test spawns threads.
sys_vfork_stress_test: LDLIBS += -lpthread

# poll_signal_eintr_test uses a flooder thread to generate inotify activity.
sys_poll_signal_eintr_test: LDLIBS += -lpthread

# internal_stress_test spawns threads
sys_internal_stress_test: LDLIBS += -lpthread

# The mixed-alignment ELF used by elf_test.sh.
chk_elf_test lin_elf_test: sys_badalign_test

clean:
	@rm -f a.out *.o *.d *.log fix_* sys_*
