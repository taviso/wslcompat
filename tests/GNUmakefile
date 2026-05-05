CFLAGS = -O0 -ggdb3
LDFLAGS =
CPPFLAGS =
LDLIBS =
VGFLAGS = --quiet --error-exitcode=2 --suppressions=valgrind.supp

.PHONY: clean test

# These tests should fail without libwslcompat, and then pass when with.
TESTS = mmap statx sockaddr socktest vtime_test ofd_close ofd_dup mincore init_order

# These test are expected failures (may implement in future).
XFAIL = name_to_handle_at memfd_create

test: $(addprefix chk_,$(TESTS)) $(addprefix xfl_,$(XFAIL)) ../libwslcompat.so
	@echo ====================
	@echo = ALL TESTS PASSED =
	@echo ====================

# Check that the patched syscalls succeed.
chk_%: fix_% xfl_% pre_% val_%
	$(addprefix ./, $<)

# Check that the unpatches syscalls fail
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

# The ELF initialization order test.
sys_init_order sys_init_order.so: init_lib.o init_order.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@.so $(word 1, $^) $(LDLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(word 2, $^) $@.so -Wl,-rpath,'$$ORIGIN'

clean:
	rm -f a.out *.o *.log fix_* sys_*
