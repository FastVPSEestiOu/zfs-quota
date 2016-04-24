#!/bin/sh

set -e
set -x

VEID=${1:-1001}

zpool import -a || :
zfs mount -a || :
vzctl stop $VEID || :
rmmod zfs-quota || :
insmod ./src/zfs-quota.ko vz_qid_limit=4294967295
vzctl start $VEID
sleep 1
#vzctl exec 1001 strace /quotactl_ex
vzctl exec $VEID ls /proc/vz/vzaquota/ -R
vzctl exec $VEID repquota -an &>/dev/null
#vzctl exec $VEID repquota -agn
