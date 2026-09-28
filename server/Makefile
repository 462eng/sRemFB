# sremfb-server (EVDI backend). The source-agnostic core lives in core/,
# the EVDI frame source in evdi/. A second binary (sremfb-spice) reuses
# the same core objects with a SPICE source — see spice/Makefile.

PKGS = glib-2.0 gio-2.0 liblz4 x264 libpipewire-0.3

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter -I.. -Icore -Ievdi \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -levdi -lm
VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

CORE_OBJS = core/net.o core/convert.o core/control.o core/encoder.o \
            core/xmit.o core/session.o core/input.o \
            core/audio.o core/layout.o
EVDI_OBJS = evdi/evdi.o evdi/edid.o main-evdi.o

all: sremfb-server sremfb-latency-click

sremfb-server: $(CORE_OBJS) $(EVDI_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

# sound half of the latency probe (tools/sremfb-latency-probe --click)
sremfb-latency-click: ../tools/sremfb-latency-click.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --libs libpipewire-0.3)

$(CORE_OBJS): core/core.h core/source.h ../protocol.h
$(EVDI_OBJS): core/core.h core/source.h evdi/evdi.h ../protocol.h

main-evdi.o: CFLAGS += -DSREMFB_VERSION='"$(VERSION)"'

clean:
	rm -f sremfb-server sremfb-latency-click rfb-server *.o core/*.o evdi/*.o

.PHONY: all clean
