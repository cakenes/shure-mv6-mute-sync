obj-m += hid-shure-mv6.o

KDIR      ?= /usr/lib/modules/$(shell uname -r)/build
MODDIR    := /usr/lib/modules/$(shell uname -r)/extra
PULSE     := $(shell pkg-config --cflags --libs libpulse)
DKMS_NAME := hid-shure-mv6
DKMS_VER  := 1.1.0
DKMS_SRC  := /usr/src/$(DKMS_NAME)-$(DKMS_VER)

all: module shure-mv6-sync

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

shure-mv6-sync: shure-mv6-sync.c
	$(CC) -O2 -Wall -o $@ $< $(PULSE)

module-clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f shure-mv6-sync

# --- DKMS (recommended) ---

dkms-install: install-daemon
	install -d $(DKMS_SRC)
	install -m 644 hid-shure-mv6.c dkms.conf Makefile $(DKMS_SRC)/
	install -D -m 644 99-shure-mv6.rules /etc/udev/rules.d/99-shure-mv6.rules
	udevadm control --reload-rules
	echo "hid_shure_mv6" > /etc/modules-load.d/hid-shure-mv6.conf
	dkms add $(DKMS_NAME)/$(DKMS_VER) || true
	dkms build $(DKMS_NAME)/$(DKMS_VER)
	dkms install $(DKMS_NAME)/$(DKMS_VER)

dkms-uninstall:
	dkms remove $(DKMS_NAME)/$(DKMS_VER) --all || true
	rm -rf $(DKMS_SRC)
	rm -f /etc/udev/rules.d/99-shure-mv6.rules
	udevadm control --reload-rules
	rm -f /etc/modules-load.d/hid-shure-mv6.conf
	rm -f /usr/local/bin/shure-mv6-sync
	rm -f /usr/lib/systemd/user/shure-mv6-sync.service

# --- Manual install (no DKMS) ---

install: install-module install-daemon

install-module:
	install -D -m 644 hid-shure-mv6.ko $(MODDIR)/hid-shure-mv6.ko
	depmod -A
	install -D -m 644 99-shure-mv6.rules /etc/udev/rules.d/99-shure-mv6.rules
	udevadm control --reload-rules
	echo "hid_shure_mv6" > /etc/modules-load.d/hid-shure-mv6.conf

install-daemon: shure-mv6-sync
	install -D -m 755 shure-mv6-sync /usr/local/bin/shure-mv6-sync
	install -D -m 644 shure-mv6-sync.service \
		/usr/lib/systemd/user/shure-mv6-sync.service

uninstall:
	rm -f $(MODDIR)/hid-shure-mv6.ko
	depmod -A
	rm -f /etc/udev/rules.d/99-shure-mv6.rules
	udevadm control --reload-rules
	rm -f /etc/modules-load.d/hid-shure-mv6.conf
	rm -f /usr/local/bin/shure-mv6-sync
	rm -f /usr/lib/systemd/user/shure-mv6-sync.service

.PHONY: all module clean install install-module install-daemon uninstall \
        dkms-install dkms-uninstall

