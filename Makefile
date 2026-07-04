PREFIX ?= /usr/local

all: server-build client-build

server-build:
	$(MAKE) -C server

client-build:
	$(MAKE) -C client

clean:
	$(MAKE) -C server clean
	$(MAKE) -C client clean
	rm -f sremfb-test-*.ppm rfb-test-*.ppm

# On the PC (run as root; the unit goes to the system-wide user-unit dir)
install-server: server-build
	install -D -m 755 server/sremfb-server $(DESTDIR)$(PREFIX)/bin/sremfb-server
	install -D -m 644 systemd/sremfb-server.service \
		$(DESTDIR)/etc/systemd/user/sremfb-server.service
	install -D -m 644 systemd/modules-load-sremfb.conf \
		$(DESTDIR)/etc/modules-load.d/sremfb.conf
	install -D -m 644 systemd/modprobe-sremfb.conf \
		$(DESTDIR)/etc/modprobe.d/sremfb.conf
	install -D -m 644 systemd/61-sremfb-display-vendor.hwdb \
		$(DESTDIR)/etc/udev/hwdb.d/61-sremfb-display-vendor.hwdb
	install -D -m 644 systemd/60-sremfb-evdi.rules \
		$(DESTDIR)/etc/udev/rules.d/60-sremfb-evdi.rules
	install -D -m 644 systemd/sremfb-evdi-perms.service \
		$(DESTDIR)/etc/systemd/system/sremfb-evdi-perms.service
	-systemctl daemon-reload && \
		systemctl enable --now sremfb-evdi-perms.service
	-chgrp video /sys/devices/evdi/add /sys/devices/evdi/remove_all 2>/dev/null && \
		chmod 664 /sys/devices/evdi/add /sys/devices/evdi/remove_all
	@test -f $(DESTDIR)/etc/sremfb-server.conf || \
		install -D -m 644 systemd/sremfb-server.conf.example \
			$(DESTDIR)/etc/sremfb-server.conf
	systemd-hwdb update || true
	@echo "Enable with: systemctl --user daemon-reload && systemctl --user enable --now sremfb-server"
	@echo "First time only: modprobe evdi   (loaded automatically from the next boot)"

# On the SBC (run as root)
install-client: client-build
	install -D -m 755 client/sremfb-client $(DESTDIR)$(PREFIX)/bin/sremfb-client
	install -D -m 644 systemd/sremfb-client.service \
		$(DESTDIR)/etc/systemd/system/sremfb-client.service
	@test -f $(DESTDIR)/etc/sremfb.conf || \
		install -D -m 644 systemd/sremfb.conf.example $(DESTDIR)/etc/sremfb.conf
	@echo "Edit /etc/sremfb.conf, then: systemctl daemon-reload && systemctl enable --now sremfb-client"

.PHONY: all server-build client-build clean install-server install-client
