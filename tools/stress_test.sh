#!/bin/sh

set -e

ZFS_ROOT=nirvana
REPO=~/stage/zfs-quota/
ACTIONS_PER_RUN=128

create_one_zfs()
{
    echo -n Recreating $ZFS_ROOT/${1}... >&2
    zfs destroy $ZFS_ROOT/$1 || :
    zfs create $ZFS_ROOT/$1
    echo done >&2
}

create_one_ve() {
    local VEID=$1
    local PRIVATE=/$ZFS_ROOT/$VEID/disk
    echo -n Creating VEID=$VEID at ${PRIVATE}... >&2
    vzctl create $VEID --ostemplate debian-7.0-x86_64-minimal --layout simfs \
        --private $PRIVATE
    cp $REPO/tools/qrandom.py $PRIVATE
    sed -e \
"/exit 0/i rm -f /aquota.* \n\
ln -fs /proc/vz/zfsquota/*/aquota.user / \n\
ln -fs /proc/vz/zfsquota/*/aquota.group / \n\
python /qrandom.py /stage $ACTIONS_PER_RUN > /log 2>&1 & \
echo \$! > /pid" \
        -i $PRIVATE/etc/rc.local
    echo done >&2
}

start_one_ve() {
    echo -n Starting VEID=$1 >&2
    vzctl start $1

    local PRIVATE=/$ZFS_ROOT/$1/disk
    for i in $(seq 0 10); do
        [ -f "$PRIVATE/pid" ] && break
        sleep 1
    done
    echo done >&2
    [ -f "$PRIVATE/pid" ]
}

get_qrandom_pid() {
    local VEID=$1
    local PRIVATE=/$ZFS_ROOT/$VEID/disk
    local VEPID=$(cat $PRIVATE/pid)
    local VE0PID=$(grep -Plz "(?s)envID:[[:space:]]*$VEID.*VPid:[[:space:]]*$VEPID" \
             /proc/[0-9]*/status | sed -e 's=/proc/==' -e 's=/status==')
    echo $VE0PID
}


start_ves() {
    ves=""
    for ve in $(seq ${1-1100} ${2-1115}); do
        echo Starting VE=$ve >&2
        create_one_zfs $ve >&2
        create_one_ve $ve >&2
        start_one_ve $ve >&2

        ves="$ves $ve"
    done
    echo "$ves"
}

get_qrandom_pids() {
    qrandom_pids=""
    for ve in $1; do
        qrandom_pids="$qrandom_pids $(get_qrandom_pid $ve)"
    done
    echo $qrandom_pids
}

schedule_qrandom() {
    local qrandom_pids="$2"
    echo -n Asking qrandom to do the job... >&2
    for pid in $qrandom_pids; do
        kill -USR1 $pid
    done
    echo done >&2
}

wait_log() {
    local ves="$1"
    local phrase="$2"
    local timeout="${3-1}"
    while [ -n "$ves" ]; do
        for ve in $ves; do
            local PRIVATE=/$ZFS_ROOT/$ve/disk
            tail -n1 /$ZFS_ROOT/$ve/disk/log | grep -q "$phrase" && \
                ves="$(echo $ves | sed -e "s/$ve//")"
        done
	[ -n "$ves" ] && sleep ${timeout}
    done
}

wait_qrandom() {
    echo -n Waiting for qrandom to finish... >&2
    wait_log "$1" "DONE"
    echo done >&2
}

check_qrandom() {
    local ves="$1"
    local qrandom_pids="$2"
    echo -n Checking repquota results... >&2
    for pid in $qrandom_pids; do
        kill -USR2 $pid
    done
    wait_log "$ves" "REPQUOTA" 4
    echo done >&2
}

ve_repquota() {
    local ve=$1
    local ofname=$(mktemp repquota-$ve.XXXXXXXXXX)
    vzctl exec $v repquota -n /dev/simfs | \
	sed -n 's/^#//; s/--//; /^$/d; /2000/,$p' o2 | \
	sort -n | awk '{ print $1, $2, $3, $5, $6 }' > $ofname
    echo $ofname
}

ve_zfsuserspace() {
   local ve=$1
   local ofname=$(mktemp zfsuserspace-$ve.XXXXXXXXXX)
   zfs userspace /$ZFS_ROOT/$ve -Hinp | \
	sed 's/POSIX User//' | \
	awk '$1 >= 2000 { print $1, int(($2 + 1023)/1024),
			  int(($3 + 1023)/1024), $4, $5+0 }' > $ofname
   echo $ofname
}

compare_repquota_zfsuserspace() {
    local ves="$1"
    for ve in $ves; do
	REPQUOTA_OUT=$(ve_repquota $ve)
	ZFSUSERSPACE_OUT=$(ve_zfsuserspace $ve)
	diff $REPQUOTA_OUT $ZFSUSERSPACE_OUT
    done
}

copy_logs() {
    local ves="$1"
    for ve in $ves; do
        local PRIVATE=/$ZFS_ROOT/$ve/disk
        cp -f $PRIVATE/log ./log-$ve
    done
}

do_stress_test() {
    local VE_NUMS=${1-8}
    local RUNS=${2-1024}
    ves="$(start_ves 1100 $((1100 + $VE_NUMS - 1)))"
    qrandom_pids="$(get_qrandom_pids "$ves")"

    SYNCS="$(seq 0 10)"
    for iter in $(seq 0 $RUNS); do
	ITER=$iter
	export ITER
	echo Iteration $iter/$RUNS >&2
        schedule_qrandom "$ves" "$qrandom_pids"
	wait_qrandom "$ves" "$qrandom_pids"
	for f in $SYNCS; do
	    sync
	    sleep 1
	done
	check_qrandom "$ves" "$qrandom_pids"
	compare_repquota_zfsuserspace "$ves"
        copy_logs "$ves" "$qrandom_pids"
    done
}

if [ "$1" = "--help" ]; then
    set +x
    echo "Usage $0 VE_NUMS RUNS"
    exit 0
fi
do_stress_test $@
