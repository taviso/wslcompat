CFLAGS = -O0 -ggdb3
LDFLAGS =
CPPFLAGS =
LDLIBS =
VGFLAGS = --quiet --error-exitcode=2

.PHONY: clean test

TESTS = mmap statx sockaddr socktest

test: $(addprefix chk_,$(TESTS)) | ../libwslcompat.so

# Check that the unmodified test fails, and the patched one succeeds.
chk_%: sys_% fix_% pre_% val_%
	@echo NOTE: ERRORS EXPECTED BELOW
	@echo ===========================
	! $(addprefix ./,$(word 1, $^))
	@echo ===========================
	$(addprefix ./,$(word 2, $^))

# Also check that it works as a preload library.
pre_%: sys_% | ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so $(addprefix ./,$<)

# Inject the library as a DT_NEEDED tag, and verify the test pases.
fix_%: sys_% | ../libwslcompat.so
	patchelf --add-needed ../libwslcompat.so --output $@ $^

sys_%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Run under valgrind to check for memory errors.
val_%: fix_%
	valgrind $(VGFLAGS) $(addprefix ./, $<)

# Generate a log of what happens.
%.log: sys_% | ../libwslcompat.so
	strace -E LD_PRELOAD=../libwslcompat.so -o $@ $(addprefix ./,$(word 1, $^))

chk_%: %.py | ../libwslcompat.so
	env LD_PRELOAD=../libwslcompat.so python $<

clean:
	rm -f *.o *.log $(addprefix sys_, $(TESTS)) $(addprefix fix_, $(TESTS))
