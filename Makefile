obj-m += hid-shure-mv6.o

KDIR    ?= /usr/lib/modules/$(shell uname -r)/build
MODDIR  := /usr/lib/modules/$(shell uname -r)/extra
PULSE   := $(shell pkg-config --cflags --libs libpulse)

all: module shure-mv6-sync

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

shure-mv6-sync: shure-mv6-sync.c
	$(CC) -O2 -Wall -o $@ $< $(PULSE)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f shure-mv6-sync

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

.PHONY: all module clean install install-module install-daemon uninstall

