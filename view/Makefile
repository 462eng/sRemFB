# H264=1 (default): H.264 decoding through libavcodec (libavcodec-dev,
# libavutil-dev). H264=0 builds without it: the viewer then never offers
# H.264 and stays on RAW/LZ4.
H264 ?= 1

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I.. $(shell pkg-config --cflags sdl3)
LDLIBS = $(shell pkg-config --libs sdl3) -llz4 -lm
VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

SRCS = sremfb-view.c net.c audio.c keymap.c
ifeq ($(H264),1)
SRCS += h264.c
CFLAGS += -DSREMFB_VIEW_H264 $(shell pkg-config --cflags libavcodec libavutil)
LDLIBS += $(shell pkg-config --libs libavcodec libavutil)
endif

# rebuild when H264= changes
BUILD_FLAGS := h264=$(H264)
.build-flags: FORCE
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@

sremfb-view: $(SRCS) view.h ../protocol.h .build-flags
	$(CC) $(CFLAGS) -DSREMFB_VIEW_VERSION='"$(VERSION)"' -pthread \
		-o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f sremfb-view .build-flags

.PHONY: clean FORCE
