obj-m := crowpil_battery.o

KVER ?= $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(KVER)/extra/crowpil_battery.ko*
	depmod -a

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

