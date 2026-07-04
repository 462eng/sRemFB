CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I..

sremfb-client: sremfb-client.c ../protocol.h
	$(CC) $(CFLAGS) -o $@ sremfb-client.c -llz4

clean:
	rm -f sremfb-client rfb-client

.PHONY: clean
