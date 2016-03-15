#!/bin/sh

set -x
set -e

install_zfs() {
	git clone https://github.com/paboldin/zfs/ || :
	pushd zfs
	git checkout dnode_fix
	[ ! -f ./configure ] && ./autogen.sh
	[ ! -f Makefile ] && ./configure
	make -j
	make install
	popd
}

install_zfs_quota() {
	git clone git@github.com:paboldin/zfs-quota.git
	pushd zfs-quota
	make
	popd
}

install_zfs
install_zfs_quota
