PREFIX ?= /usr/local
CC     ?= cc
PKGS    = x11 xrandr cairo cairo-xlib
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -DPREFIX=\"$(PREFIX)\" $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lpam -lm
OBJ     = src/main.o src/config.o src/auth.o

mel: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

install: mel
	install -Dm755 mel $(DESTDIR)$(PREFIX)/bin/mel
	install -Dm644 mel.1 $(DESTDIR)$(PREFIX)/share/man/man1/mel.1
	install -d $(DESTDIR)$(PREFIX)/share/mel/themes
	install -m644 themes/* $(DESTDIR)$(PREFIX)/share/mel/themes/
	install -Dm644 config.example $(DESTDIR)$(PREFIX)/share/mel/config.example

# Runs the real renderer into PNGs without grabbing the display or invoking PAM.
check: tests/preview_test
	./tests/preview_test

tests/preview_test: tests/preview_test.c src/main.c src/config.c src/config.h src/auth.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/preview_test.c src/config.c $(LDLIBS)

clean:
	rm -f mel $(OBJ) tests/preview_test
.PHONY: install clean check
