
obj-m = zfs-quota.o
ccflags-y := -I/opt/zfs/src/spl-0.6.5/include
ccflags-y += -DHAVE_USLEEP_RANGE=1
ccflags-y += -I/opt/zfs/src/zfs-0.6.5/include
ccflags-y += -I/usr/local/src/spl-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/include
ccflags-y += -I/usr/local/src/zfs-0.6.5/2.6.32-042stab113.11/
KBUILD_EXTRA_SYMBOLS = /usr/local/src/zfs-0.6.5/2.6.32-042stab113.11/Module.symvers

KDIR=/lib/modules/`uname -r`/build

all: zfs-quota.ko

zfs-quota.ko: zfs-quota.c
	@echo Compiling for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y
	@touch $@
