CFLAGS=-fPIC
LDLIBS=-ldl
PREFIX=/usr/local

all: libwslcompat.so

.PHONY: clean

%32.o: CFLAGS+=-m32
%32.o: %.c
	gcc -c $(CFLAGS) $(CPPFLAGS) -o $@ $^

libwslcompat.so: getsockopt.o  mmap.o fcntl.o ioctl.o
	gcc -shared $(CFLAGS) -o $@ $^ $(LDLIBS)

libwslcompat32.so: getsockopt32.o  mmap32.o fcntl32.o ioctl32.o
	gcc -shared $(CFLAGS) -m32 -o $@ $^ $(LDLIBS)

install: libwslcompat.so
	install $< $(PREFIX)/lib

clean:
	rm -f *.o *.so
