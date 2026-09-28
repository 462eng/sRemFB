CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I.. $(shell pkg-config --cflags sdl3)
LDLIBS = $(shell pkg-config --libs sdl3) -llz4 -lm
VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

sremfb-view: sremfb-view.c net.c keymap.c view.h ../protocol.h
	$(CC) $(CFLAGS) -DSREMFB_VIEW_VERSION='"$(VERSION)"' -pthread \
		-o $@ sremfb-view.c net.c keymap.c $(LDLIBS)

clean:
	rm -f sremfb-view

.PHONY: clean
