
KERNEL_VERSION ?= $(shell uname -r)

obj-m = zqfs.o
ccflags-y := -DHAVE_USLEEP_RANGE=1 -DOBJECT_QUOTA
ccflags-y += -I/usr/local/src/spl-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/$(KERNEL_VERSION)
KBUILD_EXTRA_SYMBOLS = /usr/local/src/zfs-0.6.5/$(KERNEL_VERSION)/Module.symvers

KDIR ?= /lib/modules/$(shell uname -r)/build

all: zqfs.ko

zqfs.ko:
	@echo Compiling for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y
	@touch $@

modules_install: zqfs.ko
	@echo Installing for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules_install
	@touch $@
