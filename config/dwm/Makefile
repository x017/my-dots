# dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk
.DEFAULT_GOAL := all

SRC = drw.c dwm.c util.c
OBJ = ${SRC:.c=.o}

# Optional embedded CUI dashboard. Configure/build dashboard/ first; see README.
ifeq ($(CUI),1)
CUI_DIR = dashboard/build/_deps/cui-src
CUI_LIB = dashboard/build/_deps/cui-build/libcui.a
OBJ += dashboard/cui-embedded.o
CFLAGS += -DDWM_CUI -I$(CUI_DIR)/include
LDFLAGS += $(CUI_LIB) -pthread -lm

dashboard/cui-embedded.o: dashboard/dashboard.c dashboard/pages.h dashboard/persian_font.h dashboard/desktop_controls.h dashboard/desktop_pages.h dashboard_backend.h $(CUI_LIB)
	$(CC) -c $(CFLAGS) -DDWM_EMBEDDED_CUI -o $@ dashboard/dashboard.c
endif

all: dwm

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk
dwm.o: dashboard_native.h dashboard_backend.h screenshot.h screenshot_geometry.h screenshot_pixel.h screenshot_png.h
drw.o: persian_text.h

config.h:
	cp config.def.h $@

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f dwm ${OBJ} dashboard/cui-embedded.o dwm-${VERSION}.tar.gz

dist: clean
	mkdir -p dwm-${VERSION}
	cp -R LICENSE Makefile README config.def.h config.mk\
		dwm.1 drw.h util.h dashboard_backend.h dashboard_native.h ${SRC} dwm.png transient.c persian_text.h screenshot.h screenshot_geometry.h screenshot_pixel.h screenshot_png.h dwm-${VERSION}
	cp -R dashboard dwm-${VERSION}/
	tar -cf dwm-${VERSION}.tar dwm-${VERSION}
	gzip dwm-${VERSION}.tar
	rm -rf dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f dwm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/dwm
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${MANPREFIX}/man1/dwm.1

.PHONY: all clean dist install uninstall
