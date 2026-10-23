# sremfb-server (EVDI backend). The source-agnostic core lives in core/,
# the EVDI frame source in evdi/. A second binary (sremfb-spice) reuses
# the same core objects with a SPICE source — see spice/Makefile.
# sremfb-kvm (capture HDMI, kvm/) : voir la section en fin de fichier.

PKGS = glib-2.0 gio-2.0 liblz4 x264 libpipewire-0.3

CFLAGS ?= -O2 -g
KVM_USER_CFLAGS := $(CFLAGS)
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

# ------------------------------------------------------------ sremfb-kvm
# sremfb-kvm : source V4L2 (TC358743) + gadget HID + encodeur V4L2 M2M
# (doc/SPEC-kvm-backend.md §4.1). Ne lie que glib-2.0 et lz4 : audio/layout
# sont des stubs, input et encoder sont remplacés par leurs variantes kvm/.
# Hors de `all`.
#
# Les objets du KVM, cœur compris, sont compilés à part (*.kvm.o) avec
# leurs seuls drapeaux : gio, pipewire, x264, libevdi peuvent manquer (Pi,
# sysroot croisé), et CC, CFLAGS, LDFLAGS, PKG_CONFIG se surchargent depuis
# la ligne de commande sans perdre les -I (KVM_CFLAGS est interne).
#
#   make sremfb-kvm                 encodeur x264 (core/encoder.c) : le
#                                   défaut tant que le lot C n'a pas livré
#   make sremfb-kvm ENCODER=v4l2    encodeur matériel kvm/encoder_v4l2.c
#                                   (lot C) — échoue tant que le fichier
#                                   n'existe pas
#   make sremfb-kvm KVM_STATIC=1    glib, pcre2, lz4 liés en statique, la
#                                   libc (et libm, pthread) en dynamique :
#                                   le feuillet millefeuille
#   make check-kvm                  tests unitaires diff.c / convert_uyvy.c
PKG_CONFIG ?= pkg-config
ENCODER    ?= x264

KVM_PKGS   = glib-2.0 liblz4
KVM_CORE   = core/net.kvm.o core/convert.kvm.o core/control.kvm.o \
             core/xmit.kvm.o core/session.kvm.o
KVM_OBJS   = kvm/capture.kvm.o kvm/diff.kvm.o kvm/convert_uyvy.kvm.o \
             kvm/input_hid.kvm.o kvm/stubs.kvm.o kvm/testsrc.kvm.o \
             main-kvm.kvm.o
# hidmap.c (lot B) est pris dès qu'il existe
KVM_OBJS  += $(patsubst %.c,%.kvm.o,$(wildcard kvm/hidmap.c))
ifeq ($(ENCODER),x264)
KVM_ENC    = core/encoder.kvm.o
KVM_ENC_PKGS = x264
else ifeq ($(ENCODER),v4l2)
KVM_ENC    = kvm/encoder_v4l2.kvm.o
KVM_ENC_PKGS =
else
$(error ENCODER=$(ENCODER) inconnu (x264 | v4l2))
endif

KVM_CFLAGS = $(KVM_USER_CFLAGS) -Wall -Wextra -Wno-unused-parameter \
             -I.. -Icore -Ikvm \
             $(shell $(PKG_CONFIG) --cflags $(KVM_PKGS) $(KVM_ENC_PKGS))

# libs dynamiques même en KVM_STATIC=1 (fournies par la libc de la base)
KVM_DYN_LIBS = -lm -lpthread -pthread -ldl -lrt -lc
ifeq ($(KVM_STATIC),1)
KVM_SLIBS  = $(shell $(PKG_CONFIG) --static --libs $(KVM_PKGS))
KVM_LIBS   = -Wl,-Bstatic $(filter-out $(KVM_DYN_LIBS),$(KVM_SLIBS)) \
             -Wl,-Bdynamic $(sort $(filter $(KVM_DYN_LIBS),$(KVM_SLIBS)))
else
KVM_LIBS   = $(shell $(PKG_CONFIG) --libs $(KVM_PKGS))
endif
KVM_LIBS  += $(if $(KVM_ENC_PKGS),$(shell $(PKG_CONFIG) --libs $(KVM_ENC_PKGS))) -lm

%.kvm.o: %.c
	$(CC) $(KVM_CFLAGS) -c -o $@ $<

kvm/convert_uyvy.kvm.o: KVM_CFLAGS += -ftree-vectorize
main-kvm.kvm.o: KVM_CFLAGS += -DSREMFB_VERSION='"$(VERSION)"'

sremfb-kvm: $(KVM_CORE) $(KVM_ENC) $(KVM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(KVM_LIBS)

KVM_TEST_OBJS = kvm/test-kvm.kvm.o kvm/diff.kvm.o kvm/convert_uyvy.kvm.o
kvm/test-kvm: $(KVM_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(shell $(PKG_CONFIG) --libs glib-2.0) -lm

check-kvm: kvm/test-kvm
	./kvm/test-kvm

# source explicite : ENCODER=v4l2 sans kvm/encoder_v4l2.c (lot C) échoue
# avec « aucune règle pour fabriquer kvm/encoder_v4l2.c »
$(KVM_ENC): $(KVM_ENC:.kvm.o=.c)
$(KVM_CORE) $(KVM_ENC) $(KVM_OBJS) $(KVM_TEST_OBJS): core/core.h \
	core/source.h ../protocol.h kvm/kvm.h

clean:
	rm -f sremfb-server sremfb-latency-click rfb-server *.o core/*.o evdi/*.o
	rm -f sremfb-kvm kvm/test-kvm kvm/*.o

.PHONY: all clean check-kvm
