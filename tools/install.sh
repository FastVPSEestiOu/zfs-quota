#!/bin/sh

set -x
set -e

install_zfs() {
	git clone https://github.com/paboldin/zfs/ || :
	pushd zfs
	git pull
	git checkout dnode_fix
	[ ! -f ./configure ] && ./autogen.sh
	[ ! -f Makefile ] && ./configure
	make -j
	make install
	popd
}

install_zfs_quota() {
	git clone git@github.com:paboldin/zfs-quota.git || :
	pushd zfs-quota
	git pull
	make
	make install
	popd
}

fix_initd_vz() {
	sed -e '/^\s\+MODULES=.*vzdquota/ { ' \
	       's/#*/#/; p; s/vzdquota \+//; s/#*// }' -i /etc/init.d/vz
}

install_zfs
install_zfs_quota
fix_initd_vz
