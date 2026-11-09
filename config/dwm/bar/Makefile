# Requires GTK3 and Xlib development packages and pkg-config.
CC ?= cc
PKGS = gtk+-3.0 x11
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))
PREFIX ?= /usr/local

dwm-bar: bar.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: dwm-bar
	install -Dm755 dwm-bar $(DESTDIR)$(PREFIX)/bin/dwm-bar

clean:
	rm -f dwm-bar

.PHONY: install clean
