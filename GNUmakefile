CFLAGS=-fPIC
LDLIBS=-ldl
PREFIX=/usr/local

all: libwslcompat.so

libwslcompat.so: getsockopt.o  mmap.o fcntl.o ioctl.o
	gcc -shared $(CFLAGS) -o $@ $^ $(LDLIBS)

install: libwslcompat.so
	install $< $(PREFIX)/lib

clean:
	rm -f *.o *.so
