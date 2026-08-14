# sremfb-server (EVDI backend). The source-agnostic core lives in core/,
# the EVDI frame source in evdi/. A second binary (sremfb-spice) reuses
# the same core objects with a SPICE source — see spice/Makefile.

PKGS = glib-2.0 liblz4 x264

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter -I.. -Icore -Ievdi \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -levdi -lm

CORE_OBJS = core/net.o core/convert.o core/control.o core/encoder.o \
            core/xmit.o core/session.o
EVDI_OBJS = evdi/evdi.o evdi/edid.o main-evdi.o

sremfb-server: $(CORE_OBJS) $(EVDI_OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(CORE_OBJS): core/core.h core/source.h ../protocol.h
$(EVDI_OBJS): core/core.h core/source.h evdi/evdi.h ../protocol.h

clean:
	rm -f sremfb-server rfb-server *.o core/*.o evdi/*.o

.PHONY: clean
