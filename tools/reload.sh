#!/bin/sh

set -e
set -x

zpool import -a || :
zfs mount -a || :
vzctl stop 1001 || :
rmmod zfs-quota || :
insmod ./src/zfs-quota.ko
vzctl start 1001
sleep 1
#vzctl exec 1001 strace /quotactl_ex
vzctl exec 1001 ls /proc/vz/vzaquota/ -R
vzctl exec 1001 repquota -an
vzctl exec 1001 repquota -agn
