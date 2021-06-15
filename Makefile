SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

CFLAGS=-Wall -Werror -Wextra -Wpedantic -Wno-unused-parameter -Wconversion $(shell pkg-config --cflags pixman-1)
LIBS=-lwayland-client $(shell pkg-config --libs pixman-1) -lrt
OBJ=river-tag-overlay.o river-status-unstable-v1.o wlr-layer-shell-unstable-v1.o xdg-shell.o
GEN=river-status-unstable-v1.c river-status-unstable-v1.h wlr-layer-shell-unstable-v1.c wlr-layer-shell-unstable-v1.h xdg-shell.c xdg-shell.h

river-tag-overlay: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install: river-tag-overlay
	install -D river-tag-overlay   $(DESTDIR)$(BINDIR)/river-tag-overlay
	install -D river-tag-overlay.1 $(DESTDIR)$(MANDIR)/man1/river-tag-overlay.1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/river-tag-overlay
	$(RM) $(DESTDIR)$(MANDIR)/man1/river-tag-overlay.1

clean:
	$(RM) river-tag-overlay $(GEN) $(OBJ)

.PHONY: clean install

