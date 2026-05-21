CFLAGS=-fPIC -O2
LDLIBS=-ldl
PREFIX=/usr/local
LDFLAGS=-Wl,-z,interpose,-z,initfirst

all: libwslcompat.so tools

SHIMS=getsockopt mmap fcntl ioctl statx mincore syscall rename execve execveat

.PHONY: clean test tools

libwslcompat.so: $(SHIMS:=.o) tunables.o logging.o
	gcc -shared $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tools:
	$(MAKE) -C tools

install: libwslcompat.so
	install libwslcompat.so $(PREFIX)/lib
	$(MAKE) -C tools install PREFIX=$(PREFIX)

test: libwslcompat.so
	$(MAKE) -C tests

clean:
	rm -f a.out *.o *.so
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
