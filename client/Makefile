CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I..

sremfb-client: sremfb-client.c v4l2dec.c v4l2dec.h usbexport.c usbexport.h \
               output.h output_fb.c ../protocol.h
	$(CC) $(CFLAGS) -pthread -o $@ sremfb-client.c v4l2dec.c usbexport.c \
	      output_fb.c -llz4

clean:
	rm -f sremfb-client rfb-client

.PHONY: clean
