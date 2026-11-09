# GTK3 and pkg-config development packages are required.
CC ?= cc
PREFIX ?= /usr/local
PKGS = gtk+-3.0 gio-2.0
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

dwm-wallpapers: wallpaper.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: dwm-wallpapers
	install -Dm755 dwm-wallpapers $(DESTDIR)$(PREFIX)/bin/dwm-wallpapers

clean:
	rm -f dwm-wallpapers

.PHONY: install clean
