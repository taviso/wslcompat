CFLAGS = -O0 -ggdb3
LDFLAGS =
CPPFLAGS =
LDLIBS = -lrt
VGFLAGS = --quiet --error-exitcode=2 --suppressions=valgrind.supp

.PHONY: clean test linux

# These tests should fail without libwslcompat, and then pass when with.
TESTS = mmap statx sockaddr socktest vtime_test ofd_close ofd_dup ofd mincore \
        init_order multiproc mlock_test

# These test are expected failures (may implement in future).
XFAIL = name_to_handle_at memfd_create ofd_partial kcmp_test ofd_flock_conflict

# These tests should pass both with and without the shim.
XPASS = shm_test inode_test flock_vs_fcntl fcntl_close_wipe

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

lin_%: %.py
	python3 $<

# The ELF initialization order test.
sys_init_order: init_lib.o init_order.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@.so $(word 1, $^) $(LDLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(word 2, $^) $@.so -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

clean:
	rm -f a.out *.o *.log fix_* sys_*
