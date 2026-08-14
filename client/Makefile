CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I.. $(shell pkg-config --cflags libdrm)
DRM_LIBS = $(shell pkg-config --libs libdrm)

sremfb-client: sremfb-client.c v4l2dec.c v4l2dec.h usbexport.c usbexport.h \
               output.h output_fb.c output_drm.c ../protocol.h
	$(CC) $(CFLAGS) -pthread -o $@ sremfb-client.c v4l2dec.c usbexport.c \
	      output_fb.c output_drm.c -llz4 $(DRM_LIBS)

clean:
	rm -f sremfb-client rfb-client

.PHONY: clean
