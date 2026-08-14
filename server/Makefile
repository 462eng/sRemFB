# Two binaries share the source-agnostic core (core/):
#   sremfb-server : EVDI frame source (evdi/)   — GNOME/Wayland host
#   sremfb-spice  : SPICE frame source (spice/) — mirrors a QEMU/KVM VM
# 'make' builds sremfb-server; 'make sremfb-spice' needs the SPICE dev
# package (libspice-client-glib-2.0-dev) and is meant for bridge machines
# that never touch libevdi.

PKGS = glib-2.0 liblz4 x264

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter -I.. -Icore \
          $(shell pkg-config --cflags $(PKGS))
CORE_LIBS = $(shell pkg-config --libs $(PKGS)) -lm

SPICE_CFLAGS = $(shell pkg-config --cflags spice-client-glib-2.0)
SPICE_LIBS   = $(shell pkg-config --libs spice-client-glib-2.0)

CORE_OBJS = core/net.o core/convert.o core/control.o core/encoder.o \
            core/xmit.o core/session.o
EVDI_OBJS = evdi/evdi.o evdi/edid.o main-evdi.o
SPICE_OBJS = spice/spice_source.o main-spice.o

all: sremfb-server

$(EVDI_OBJS): CFLAGS += -Ievdi
sremfb-server: $(CORE_OBJS) $(EVDI_OBJS)
	$(CC) -o $@ $^ $(CORE_LIBS) -levdi

# SPICE objects get the spice-client-glib flags; the core objects do not.
$(SPICE_OBJS): CFLAGS += -Ispice $(SPICE_CFLAGS)
sremfb-spice: $(CORE_OBJS) $(SPICE_OBJS)
	$(CC) -o $@ $^ $(CORE_LIBS) $(SPICE_LIBS)

$(CORE_OBJS):  core/core.h core/source.h ../protocol.h
$(EVDI_OBJS):  core/core.h core/source.h evdi/evdi.h ../protocol.h
$(SPICE_OBJS): core/core.h core/source.h spice/spice.h ../protocol.h

clean:
	rm -f sremfb-server sremfb-spice rfb-server *.o \
	      core/*.o evdi/*.o spice/*.o

.PHONY: all clean
