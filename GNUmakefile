CFLAGS=-fPIC -O2 -MMD -MP
LDLIBS=-ldl
PREFIX=/usr/local
LDFLAGS=-Wl,-z,interpose,-z,initfirst

all: libwslcompat.so tools

debug: CFLAGS=-fPIC -O0 -ggdb3 -MMD -MP
debug: clean all

SHIMS=getsockopt setsockopt mmap fcntl ioctl statx mincore syscall rename   \
      execve execveat clock open synth read close poll epoll copy_file_range

LIB_OBJS = $(SHIMS:=.o) tunables.o logging.o tty.o internal.o utils.o

.PHONY: clean test tools

libwslcompat.so: $(LIB_OBJS)
	gcc -shared $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

open.o: proc.c

-include $(LIB_OBJS:.o=.d)

tools:
	$(MAKE) -C tools

install: libwslcompat.so
	install libwslcompat.so $(PREFIX)/lib
	$(MAKE) -C tools install PREFIX=$(PREFIX)

test: libwslcompat.so
	$(MAKE) -C tests

clean:
	rm -f a.out *.o *.d *.so
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
