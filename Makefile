
obj-m = zfs-quota.o
ccflags-y := -I/opt/zfs/src/spl-0.6.5/include
ccflags-y += -I/opt/zfs/src/zfs-0.6.5/include

KDIR=/home/davinchi/openvz/linux-2.6.32

all: zfs-quota.ko

zfs-quota.ko: zfs-quota.c
	@echo Compiling for kernel $(KVERSION)
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y
	@touch $@
