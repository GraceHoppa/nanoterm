# Works on Linux and in Termux (Termux sets $PREFIX to its own usr/).
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wshadow -Wformat=2
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)

SRC := src/main.c src/ch340.c src/ihex.c src/stk500.c src/term.c
HDR := $(wildcard src/*.h)

nanoterm: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LIBUSB_LIBS) -lpthread

# Unit tests: no board needed. The upload test runs the real STK500 code
# against a simulated optiboot (tests/fake_optiboot.c).
TESTS := tests/test_ihex tests/test_stk500 tests/test_divisor

tests/test_ihex: tests/test_ihex.c src/ihex.c src/ihex.h tests/check.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_ihex.c src/ihex.c

tests/test_stk500: tests/test_stk500.c tests/fake_optiboot.c tests/fake_optiboot.h src/stk500.c src/stk500.h tests/check.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_stk500.c tests/fake_optiboot.c src/stk500.c

tests/test_divisor: tests/test_divisor.c src/ch340.c src/ch340.h tests/check.h
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -Isrc -o $@ tests/test_divisor.c src/ch340.c $(LIBUSB_LIBS) -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

install: nanoterm
	install -Dm755 nanoterm $(DESTDIR)$(PREFIX)/bin/nanoterm

# Termux: also the nt launcher and the arduino-cli-in-proot wrapper.
install-termux: install
	install -Dm755 termux/nt $(DESTDIR)$(PREFIX)/bin/nt
	install -Dm755 termux/arduino-cli $(DESTDIR)$(PREFIX)/bin/arduino-cli

clean:
	rm -f nanoterm $(TESTS)

.PHONY: test install install-termux clean
