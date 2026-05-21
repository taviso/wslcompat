CFLAGS=-fPIC -O2
LDLIBS=-ldl
PREFIX=/usr/local
LDFLAGS=-Wl,-z,interpose,-z,initfirst

all: libwslcompat.so

SHIMS=getsockopt mmap fcntl ioctl statx mincore syscall rename

.PHONY: clean test

libwslcompat.so: $(SHIMS:=.o) tunables.o logging.o
	gcc -shared $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

install: libwslcompat.so
	install $< $(PREFIX)/lib

test: libwslcompat.so
	$(MAKE) -C tests

clean:
	rm -f a.out *.o *.so
	$(MAKE) -C tests clean
