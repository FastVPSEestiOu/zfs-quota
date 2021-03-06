ZFS Quota proxy module
======================

The project aims at providing a proxy between UNIX
[quota](http://sourceforge.net/projects/linuxquota) and
[ZFS On Linux](http://zfsonlinux.org/) quota. The user is able to use standard
utilities such as `repquota` and `edquota` to show or edit `zfs` quotas.

It was originally developed for the [OpenVZ6](https://openvz.org/)-enabled
kernel here at [FastVPS](http://vps2fast.com/) but includes proxy filesystem
named ZQFS for the [CentOS7](https://www.centos.org/) vanilla kernels as well.

Requirements
------------

SPL and ZFS installed with the appopriate developer packages are required.
Autoconf and gang are required to generate `./configure` file.

Vanilla CentOS 6 kernel lacks necessary infrastructure and the module
cannot be compiled here. Vanilla CentOS 7 kernel works well.

"Development Tools" group has to be installed as well:

```shell
yum groupinstall -y "Development Tools"
```

Installation
------------

After the requirements are met and the kernel-devel along with spl and zfs
are [installed](http://zfsonlinux.org/epel.html) it is as easy as the following:

```shell
git clone https://github.com/paboldin/zfs-quota
cd zfs-quota
automake --add-missing --force-missing &>/dev/null
autoreconf -fisv
./configure
make -j
make install
```

By default this builds and installs kernel modules `zfs-quota` and `zqfs` for
the currently active kernel. To specify another kernel use `--with-linux-obj`
configuration option with the kernel directory as the argument.

Note that `inode` quotas are not yet merged upstream so one will have to
pull the code from the appropriate
[pull request](https://github.com/zfsonlinux/zfs/issues/3500).

Using with OpenVZ
-----------------

First, the regular `vzdquota` module must be disabled and `DISK_QUOTA` set to
`no`:

```shell
sed -e '/^\s\+MODULES=.*vzdquota/ {
   s/#*/#/; p; s/vzdquota/zfs-quota/; s/#*// }' -i /etc/init.d/vz
sed -e '/DISK_QUOTA=/ { s/yes/no/; }' -i /etc/vz/vz.conf
```

The machine must be reboot afterwards as the `vzdquota` module does not
support the module unload.

This will disable OpenVZ per-container (level1) quotas as well. They will be
replaced with ZFS diskspace quota.

Next just deploy a VE above a ZFS instance as described in
[this manual](https://github.com/pavel-odintsov/OpenVZ_ZFS/blob/master/OpenVZ_containers_on_zfs_filesystem.md).

The OpenVZ uses a `simfs` filesystem and appropriate device as the target
for `quotactl` syscalls performed by `quota` utils executed from inside the VE.
To enable container access to this interface it is necessary to allow VE
to use user/group disk quota. To do this execute:

```shell
vzctl set $CTID --quotaugidlimit 1 --save
```

Now (re)start the VE and execute the `repquota` from inside it:

```shell
vzctl exec $CTID repquota -a
```

It should provide the output with the current disk usage. *NOTE* that the
maximum output quota id by default equals 131072. This can be changed with
`zfs-quota` module parameter `vz_qid_limit` either during module load
or via file `/sys/module/zfs_quota/parameters/vz_qid_limit` and remounting
the `simfs` filesystem.

Usage with ZQFS
---------------

Vanilla kernel users can use the module too though this usage requires proxy
file system that has to exploit kernel in a tricky-hacky manner. In particular
the ZQFS module creates an alias (almost hardlink) for a directory inode and
overrides a `stat` call to the directory root so it matches the specified
device.

That being said here is how you can use it. Just mount the `zqfs` type file
system above the ZFS:

```shell
mount -t zqfs /dev/zqfs/mydev /mnt/zqfs -o fsroot=/zfs/data[,limit=qid_limit]
```

This will mount the ZQFS from the given ZFS root and create appropriate device
node. The optional `limit` is used to specify maximum QID that will be shown
for user.

TODO
----

* Implement `vzdquota` interface as a `vzquota` executable to set `ugidlimit`.
