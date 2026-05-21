CFLAGS=-O2
PREFIX=/usr/local

.PHONY: all clean install

all: elfclamp

install: elfclamp
	install elfclamp $(PREFIX)/bin

clean:
	rm -f elfclamp
