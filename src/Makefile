
KERNEL_VERSION ?= $(shell uname -r)

obj-m = zfs-quota.o
zfs-quota-y := quota.o proc.o tree.o zfs.o radix-tree-iter.o proc-vfsold.o proc-vfsv2.o
ccflags-y := -DHAVE_USLEEP_RANGE=1 -DOBJECT_QUOTA
ccflags-y += -I/usr/local/src/spl-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/$(KERNEL_VERSION)
KBUILD_EXTRA_SYMBOLS = /usr/local/src/zfs-0.6.5/$(KERNEL_VERSION)/Module.symvers

KDIR ?= /lib/modules/$(shell uname -r)/build

all: zfs-quota.ko

zfs-quota.ko: $(zfs-quota-y:%.o=%.c)
	@echo Compiling for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y
	@touch $@

modules_install: zfs-quota.ko
	@echo Installing for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules_install
	@touch $@
