#!/bin/sh

echo + set -x
set -x
set -e

ZFS_ROOT=nirvana
REPO=~/stage/zfs-quota/
ACTIONS_PER_RUN=128

create_one_zfs()
{
    zfs destroy $ZFS_ROOT/$1 || :
    zfs create $ZFS_ROOT/$1
}

create_one_ve() {
    local VEID=$1
    local PRIVATE=/$ZFS_ROOT/$VEID/disk
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
}

start_one_ve() {
    vzctl start $1

    local PRIVATE=/$ZFS_ROOT/$1/disk
    for i in $(seq 0 10); do
        [ -f "$PRIVATE/pid" ] && break
        sleep 1
    done
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
        echo Creating VE=$ve >&2
        create_one_zfs $ve >&2
        create_one_ve $ve >&2
        start_one_ve $ve >&2

        ves="$ves $ve"
    done
    echo $ves
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
    for pid in $qrandom_pids; do
        kill -USR1 $pid
    done
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
        sleep 1
    done
}

wait_qrandom() {
    wait_log "$1" "DONE"
}

check_qrandom() {
    local ves="$1"
    local qrandom_pids="$2"
    for pid in $qrandom_pids; do
        kill -USR2 $pid
    done
    wait_log "$ves" "REPQUOTA" 4
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

    for iter in $(seq 0 $RUNS); do
        echo ITER $iter from $RUNS
        schedule_qrandom "$ves" "$qrandom_pids"
        wait_qrandom "$ves" "$qrandom_pids"
        sleep 10
        check_qrandom "$ves" "$qrandom_pids"
        copy_logs "$ves" "$qrandom_pids"
    done
}

if [ "$1" = "--help" ]; then
    set +x
    echo "Usage $0 VE_NUMS RUNS"
    exit 0
fi
do_stress_test $@
