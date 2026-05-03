CFLAGS=-fPIC
LDLIBS=-ldl -lrt -lpthread
PREFIX=/usr/local
LDFLAGS=-Wl,-z,interpose,-z,initfirst

all: libwslcompat.so

.PHONY: clean test

%32.o: CFLAGS+=-m32
%32.o: %.c
	gcc -c $(CFLAGS) $(CPPFLAGS) -o $@ $^

libwslcompat.so: getsockopt.o  mmap.o fcntl.o ioctl.o stat.o
	gcc -shared $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

libwslcompat32.so: getsockopt32.o  mmap32.o fcntl32.o ioctl32.o stat32.o
	gcc -shared $(CFLAGS) $(LDFLAGS) -m32 -o $@ $^ $(LDLIBS)

install: libwslcompat.so
	install $< $(PREFIX)/lib

test: libwslcompat.so
	$(MAKE) -C tests

clean:
	rm -f *.o *.so
	$(MAKE) -C tests clean
