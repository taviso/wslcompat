CFLAGS=-fPIC
LDLIBS=-ldl

all: libwslcompat.so

libwslcompat.so: getsockopt.o  mmap.o fcntl.o
	gcc -shared $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f *.o *.so
