PKGS = glib-2.0 liblz4 x264

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter -I.. $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -levdi -lm

OBJS = main.o net.o evdi.o edid.o convert.o control.o encoder.o

sremfb-server: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): sremfb-server.h ../protocol.h

clean:
	rm -f sremfb-server rfb-server *.o

.PHONY: clean
