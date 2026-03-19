obj-m += hid-shure-mv6.o

KDIR ?= /usr/lib/modules/$(shell uname -r)/build
MODDIR := /usr/lib/modules/$(shell uname -r)/extra

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	install -D -m 644 hid-shure-mv6.ko $(MODDIR)/hid-shure-mv6.ko
	depmod -A
