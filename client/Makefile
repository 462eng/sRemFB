CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I..
VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

sremfb-client: sremfb-client.c v4l2dec.c v4l2dec.h usbexport.c usbexport.h \
               ../protocol.h
	$(CC) $(CFLAGS) -DSREMFB_VERSION='"$(VERSION)"' -pthread -o $@ sremfb-client.c v4l2dec.c usbexport.c -llz4

clean:
	rm -f sremfb-client rfb-client

.PHONY: clean
