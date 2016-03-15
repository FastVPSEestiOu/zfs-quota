#!/bin/sh

set -x
set -e

install_depends() {
	yum groupinstall -q -y 'Development Tools'
	yum install -q -y wget zlib-devel libuuid-devel libblkid-devel
}

install_openvz() {
	KERNEL_VERSION="$(rpm -q vzkernel-devel | sed -e '$!d; s/vzkernel-devel-//; s/.x86_64//')"
	KDIR="/lib/modules/$KERNEL_VERSION/build"
	export KERNEL_VERSION KDIR
	[ -d "$KDIR" ] && return
	[ -f /etc/yum.repos.d/openvz.repo ] || \
		wget -O /etc/yum.repos.d/openvz.repo \
			http://download.openvz.org/openvz.repo
	rpm --import http://download.openvz.org/RPM-GPG-Key-OpenVZ
	yum install -q -y vzkernel vzkernel-devel vzctl
	KERNEL_VERSION="$(rpm -q vzkernel-devel | sed -e '$!d; s/vzkernel-devel-//; s/.x86_64//')"
	KDIR="/lib/modules/$KERNEL_VERSION/build"
	export KERNEL_VERSION KDIR
	[ -d $KDIR ] || exit 1
}

install_spl() {
	[ -f spl_installed ] && return
	git clone https://github.com/zfsonlinux/spl/ || :
	pushd spl
	git pull
	[ ! -f ./configure ] && ./autogen.sh
	[ ! -f Makefile ] && ./configure --with-linux-obj=$KDIR
	make -j
	make install
	popd
	touch spl_installed
}

install_zfs() {
	[ -f zfs_installed ] && return
	git clone https://github.com/paboldin/zfs/ || :
	pushd zfs
	git pull
	git checkout dnode_fix
	[ ! -f ./configure ] && ./autogen.sh
	[ ! -f Makefile ] && ./configure --with-linux-obj=$KDIR
	make -j
	make install
	popd
	touch zfs_installed
}

install_zfs_quota() {
	[ -f zfs_quota_installed ] && return
	ssh-keyscan -t rsa,dsa -H github.com >> ~/.ssh/known_hosts
	git clone git@github.com:paboldin/zfs-quota.git || :
	pushd zfs-quota
	git pull
	make KDIR=$KDIR KERNEL_VERSION=$KERNEL_VERSION
	make install
	popd
	touch zfs_quota_installed
}

fix_initd_vz() {
	sed -e '/^\s\+MODULES=.*vzdquota/ {
	       s/#*/#/; p; s/vzdquota/zfs-quota/; s/#*// }' -i /etc/init.d/vz
}

install_depends
install_openvz
install_spl
install_zfs
install_zfs_quota
fix_initd_vz
