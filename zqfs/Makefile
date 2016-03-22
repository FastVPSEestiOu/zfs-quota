
KERNEL_VERSION ?= $(shell uname -r)

obj-m = zqfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: zqfs.ko

zqfs.ko: zqfs.c
	@echo Compiling for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y \
		KBUILD_EXTRA_SYMBOLS=$(CURDIR)/../src/Module.symvers
	@touch $@

modules_install: zqfs.ko
	@echo Installing for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules_install
	@touch $@
