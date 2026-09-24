# Works on Linux and in Termux (Termux sets $PREFIX to its own usr/).
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0) -lpthread

nanoterm: nanoterm.c

install: nanoterm
	install -Dm755 nanoterm $(DESTDIR)$(PREFIX)/bin/nanoterm

# Termux only: also install the nt launcher.
install-termux: install
	install -Dm755 nt $(DESTDIR)$(PREFIX)/bin/nt

clean:
	rm -f nanoterm

.PHONY: install install-termux clean
