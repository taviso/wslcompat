CFLAGS=-fPIC -O2
LDLIBS=-ldl -lrt -lpthread
PREFIX=/usr/local
LDFLAGS=-Wl,-z,interpose,-z,initfirst

all: libwslcompat.so

.PHONY: clean test

libwslcompat.so: getsockopt.o  mmap.o fcntl.o ioctl.o statx.o mincore.o
	gcc -shared $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

install: libwslcompat.so
	install $< $(PREFIX)/lib

test: libwslcompat.so
	$(MAKE) -C tests

clean:
	rm -f *.o *.so
	$(MAKE) -C tests clean
