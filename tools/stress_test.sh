#!/bin/sh

if [[ "$(basename -- "$0")" == "stress_test.sh" ]]; then
	set -e
fi

LOGFILE=$(mktemp stress_test.XXXXXXXXXX.log)
echo "Writing log to $LOGFILE"
exec 3>$LOGFILE
ZFS_ROOT=nirvana
REPO=$(dirname -- $(dirname -- "$0"))
ACTIONS_PER_RUN=128

create_one_zfs()
{
    echo -n Recreating $ZFS_ROOT/${1}... >&2
    zfs destroy $ZFS_ROOT/$1 || : >&3 2>&3
    zfs create $ZFS_ROOT/$1 >&3 2>&3
    echo done >&2
}

create_one_ve() {
    local VEID=$1
    local PRIVATE=/$ZFS_ROOT/$VEID/disk
    echo -n Creating VEID=$VEID at ${PRIVATE}... >&2
    vzctl create $VEID --ostemplate debian-7.0-x86_64-minimal --layout simfs \
        --private $PRIVATE >&3 2>&3
    vzctl set $VEID --save --quotaugidlimit 1 >&3 2>&3
    cp $REPO/tools/qrandom.py $PRIVATE >&3 2>&3
    sed -e \
"/exit 0/i python /qrandom.py /stage $ACTIONS_PER_RUN > /log 2>&1 & \
echo \$! > /pid" \
        -i $PRIVATE/etc/rc.local
    echo done >&2
}

start_one_ve() {
    echo -n Starting VEID=${1}... >&2
    vzctl start $1 >&3 2>&3

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
             /proc/[0-9]*/status 2>/dev/null | \
	     sed -e 's=/proc/==' -e 's=/status==')
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
	[ -n "$ves" ] && sleep ${timeout} || :
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
    local args=$2
    local ofname=$(mktemp --tmpdir repquota-$ve.XXXXXXXXXX)
    vzctl exec $ve repquota $args -n /dev/simfs | \
	sed -n 's/^#//; s/--//; /^$/d; /2000/,$p' | \
	sort -n | awk '{ print $1, $2, $3, $5, $6 }' > $ofname
    echo $ofname
}

ve_zfs_space() {
   local ve=$1
   local cmd=${2-userspace}
   local ofname=$(mktemp --tmpdir zfsuserspace-$ve.XXXXXXXXXX)
   zfs $cmd $ZFS_ROOT/$ve -Hinp | \
	sed 's/POSIX User//' | \
	awk '$1 >= 2000 { print $1, int(($2 + 1023)/1024),
			  int(($3 + 1023)/1024), $4, $5+0 }' > $ofname
   echo $ofname
}

compare_repquota_zfsuserspace() {
    local ves="$1"
    for ve in $ves; do
	REPQUOTA_OUT=$(ve_repquota $ve)
	ZFS_SPACE_OUT=$(ve_zfs_space $ve)
	echo "REPQUOTA FOR $ve" >&2
	tail -n1 /$ZFS_ROOT/$ve/disk/log >&2
	echo "QUOTA DIFF FOR $ve" 2>&1 | \
		tee -a /$ZFS_ROOT/$ve/disk/log
	diff -u $REPQUOTA_OUT $ZFS_SPACE_OUT 2>&1 | \
		tee -a /$ZFS_ROOT/$ve/disk/log
	/bin/rm -f $REPQUOTA_OUT $ZFS_SPACE_OUT
    done
}

copy_logs() {
    local ves="$1"
    for ve in $ves; do
        local PRIVATE=/$ZFS_ROOT/$ve/disk
        cp -f $PRIVATE/log ./log-$ve
    done
}

init_vz_qid_limit() {
	[ -f /sys/module/zfs_quota/parameters/vz_qid_limit ] &&
		echo 4294967295 > /sys/module/zfs_quota/parameters/vz_qid_limit
}

do_stress_test() {
    local VE_NUMS=${1-8}
    local RUNS=${2-1024}
    init_vz_qid_limit
    ves="$(start_ves 1100 $((1100 + $VE_NUMS - 1)))"
    qrandom_pids="$(get_qrandom_pids "$ves")"

    SYNCS="$(seq 1 10)"
    for iter in $(seq 1 $RUNS); do
	ITER=$iter
	export ITER
	echo Iteration $iter/$RUNS >&2
	schedule_qrandom "$ves" "$qrandom_pids"
	wait_qrandom "$ves" "$qrandom_pids"
	echo -n "Syncing..." >&2
	for f in $SYNCS; do
	    sync
	    sleep 1
	done
	echo done >&2
	check_qrandom "$ves" "$qrandom_pids"
	compare_repquota_zfsuserspace "$ves"
	copy_logs "$ves" "$qrandom_pids"
    done
}

if [[ "$(basename -- "$0")" == "stress_test.sh" ]]; then
	if [ "$1" = "--help" ]; then
	    set +x
	    echo "Usage $0 VE_NUMS RUNS"
	    exit 0
	fi
	do_stress_test $@
fi
