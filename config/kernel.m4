dnl #
dnl # Detect the kernel to be built against
dnl #
AC_DEFUN([ZFS_AC_KERNEL], [
	AC_ARG_WITH([linux],
		AS_HELP_STRING([--with-linux=PATH],
		[Path to kernel source]),
		[kernelsrc="$withval"])

	AC_ARG_WITH(linux-obj,
		AS_HELP_STRING([--with-linux-obj=PATH],
		[Path to kernel build objects]),
		[kernelbuild="$withval"])

	AC_MSG_CHECKING([kernel source directory])
	AS_IF([test -z "$kernelsrc"], [
		AS_IF([test -e "/lib/modules/$(uname -r)/source"], [
			headersdir="/lib/modules/$(uname -r)/source"
			sourcelink=$(readlink -f "$headersdir")
		], [test -e "/lib/modules/$(uname -r)/build"], [
			headersdir="/lib/modules/$(uname -r)/build"
			sourcelink=$(readlink -f "$headersdir")
		], [
			sourcelink=$(ls -1d /usr/src/kernels/* \
			             /usr/src/linux-* \
			             2>/dev/null | grep -v obj | tail -1)
		])

		AS_IF([test -n "$sourcelink" && test -e ${sourcelink}], [
			kernelsrc=`readlink -f ${sourcelink}`
		], [
			kernelsrc="[Not found]"
		])
	], [
		AS_IF([test "$kernelsrc" = "NONE"], [
			kernsrcver=NONE
		])
	])

	AC_MSG_RESULT([$kernelsrc])
	AS_IF([test ! -d "$kernelsrc"], [
		AC_MSG_ERROR([
	*** Please make sure the kernel devel package for your distribution
	*** is installed and then try again.  If that fails, you can specify the
	*** location of the kernel source with the '--with-linux=PATH' option.])
	])

	AC_MSG_CHECKING([kernel build directory])
	AS_IF([test -z "$kernelbuild"], [
		AS_IF([test -e "/lib/modules/$(uname -r)/build"], [
			kernelbuild=`readlink -f /lib/modules/$(uname -r)/build`
		], [test -d ${kernelsrc}-obj/${target_cpu}/${target_cpu}], [
			kernelbuild=${kernelsrc}-obj/${target_cpu}/${target_cpu}
		], [test -d ${kernelsrc}-obj/${target_cpu}/default], [
			kernelbuild=${kernelsrc}-obj/${target_cpu}/default
		], [test -d `dirname ${kernelsrc}`/build-${target_cpu}], [
			kernelbuild=`dirname ${kernelsrc}`/build-${target_cpu}
		], [
			kernelbuild=${kernelsrc}
		])
	])
	AC_MSG_RESULT([$kernelbuild])

	AC_MSG_CHECKING([kernel source version])
	utsrelease1=$kernelbuild/include/linux/version.h
	utsrelease2=$kernelbuild/include/linux/utsrelease.h
	utsrelease3=$kernelbuild/include/generated/utsrelease.h
	AS_IF([test -r $utsrelease1 && fgrep -q UTS_RELEASE $utsrelease1], [
		utsrelease=linux/version.h
	], [test -r $utsrelease2 && fgrep -q UTS_RELEASE $utsrelease2], [
		utsrelease=linux/utsrelease.h
	], [test -r $utsrelease3 && fgrep -q UTS_RELEASE $utsrelease3], [
		utsrelease=generated/utsrelease.h
	])

	AS_IF([test "$utsrelease"], [
		kernsrcver=`(echo "#include <$utsrelease>";
		             echo "kernsrcver=UTS_RELEASE") |
		             cpp -I $kernelbuild/include |
		             grep "^kernsrcver=" | cut -d \" -f 2`

		AS_IF([test -z "$kernsrcver"], [
			AC_MSG_RESULT([Not found])
			AC_MSG_ERROR([*** Cannot determine kernel version.])
		])
	], [
		AC_MSG_RESULT([Not found])
		if test "x$enable_linux_builtin" != xyes; then
			AC_MSG_ERROR([*** Cannot find UTS_RELEASE definition.])
		else
			AC_MSG_ERROR([
	*** Cannot find UTS_RELEASE definition.
	*** Please run 'make prepare' inside the kernel source tree.])
		fi
	])

	AC_MSG_RESULT([$kernsrcver])

	LINUX=${kernelsrc}
	LINUX_OBJ=${kernelbuild}
	LINUX_VERSION=${kernsrcver}

	AC_SUBST(LINUX)
	AC_SUBST(LINUX_OBJ)
	AC_SUBST(LINUX_VERSION)
])

AC_DEFUN([ZFSQUOTA_AC_ZFS], [
	AC_ARG_WITH([zfs],
		AS_HELP_STRING([--with-zfs=PATH],
		[Path to zfs source]),
		AS_IF([test "$withval" = "yes"],
			AC_MSG_ERROR([--with-zfs=PATH requires a PATH]),
			[zfssrc="$withval"]))

	AC_ARG_WITH([zfs-obj],
		AS_HELP_STRING([--with-zfs-obj=PATH],
		[Path to zfs build objects]),
		[zfsbuild="$withval"])

	dnl #
	dnl # The existence of zfs.release.in is used to identify a valid
	dnl # source directory.  In order of preference:
	dnl #
	
	zfssrc0="/var/lib/dkms/zfs/*/build"
	zfssrc1="/usr/local/src/zfs-*/${LINUX_VERSION}"
	zfssrc2="/usr/local/src/zfs-*"
	zfssrc3="/usr/src/zfs-*/${LINUX_VERSION}"
	zfssrc4="/usr/src/zfs-*"
	zfssrc5="../zfs/"
	zfssrc6="$LINUX"

	test_zfs_srcdir() {
		listdir="${1}"
		lastdir=""
		for zfssrc in ${listdir}; do
			test -e "${zfssrc}/zfs.release.in" && lastdir="${zfssrc}"
		done
		test -z "${lastdir}" && return 1
		dnl # use AS_VERSION_COMPARE here
		ZFSVERSION=$(echo ${lastdir} | sed 's,.*zfs@<:@-/@:>@,,;s,/.*,,')
	}
	
	AC_MSG_CHECKING([zfs source directory])
	AS_IF([test -z "${zfssrc}"], [
		[all_zfs_sources="
		${zfssrc0}
		${zfssrc1}
		${zfssrc2}
		${zfssrc3}
		${zfssrc4}
		${zfssrc5}
		${zfssrc6}"],
		AS_IF([ test_zfs_srcdir "${zfssrc0}"], [
			zfssrc=${lastdir}
		], [ test_zfs_srcdir "${zfssrc1}"], [
			zfssrc=${lastdir}
		], [ test_zfs_srcdir "${zfssrc2}"], [
			zfssrc=${lastdir}
		], [ test_zfs_srcdir "${zfssrc3}"], [
			zfssrc=$(readlink -f "${lastdir}")
		], [ test_zfs_srcdir "${zfssrc4}"], [
			zfssrc=${lastdir}
		], [ test -e "${zfssrc5}/zfs.release.in"], [
			zfssrc=$(readlink -f "${zfssrc5}")
		], [ test -e "${zfssrc6}/zfs.release.in" ], [
			zfssrc=${zfssrc6}
		], [
			zfssrc="[Not found]"
		])
	], [
		[all_zfs_sources="$withval"],
		AS_IF([test "$zfssrc" = "NONE"], [
			zfsbuild=NONE
			zfssrcver=NONE
		])
	])

	AS_IF([ test ! -e "$zfssrc/zfs.release.in"], [
		AC_MSG_ERROR([
	*** Please make sure the kmod zfs devel package for your distribution
	*** is installed then try again.  If that fails you can specify the
	*** location of the zfs source with the '--with-zfs=PATH' option.
	*** Failed to find zfs.release.in in the following:
	$all_zfs_sources])
	])

	AC_MSG_RESULT([$zfssrc])

	dnl #
	dnl # The existence of the zfs_config.h is used to identify a valid
	dnl # zfs object directory.  In many cases the object and source
	dnl # directory are the same, however the objects may also reside
	dnl # is a subdirectory named after the kernel version.
	dnl #
	AC_MSG_CHECKING([zfs build directory])

	all_zfs_config_locs="${zfssrc}/${LINUX_VERSION}
	${zfssrc}"

	AS_IF([test -z "$zfsbuild"], [
		AS_IF([ test -e "${zfssrc}/${LINUX_VERSION}/zfs_config.h" ], [
			zfsbuild="${zfssrc}/${LINUX_VERSION}"
		], [ test -e "${zfssrc}/zfs_config.h" ], [
			zfsbuild="${zfssrc}"
		], [ find -L "${zfssrc}" -name zfs_config.h 2> /dev/null | grep -wq zfs_config.h ], [
			zfsbuild=$(find -L "${zfssrc}" -name zfs_config.h | sed 's,/zfs_config.h,,')
		], [
			zfsbuild="[Not found]"
		])
	])

	AC_MSG_RESULT([$zfsbuild])
	AS_IF([ ! test -e "$zfsbuild/zfs_config.h"], [
		AC_MSG_ERROR([
	*** Please make sure the kmod zfs devel <kernel> package for your
	*** distribution is installed then try again.  If that fails you
	*** can specify the location of the zfs objects with the
	*** '--with-zfs-obj=PATH' option.  Failed to find zfs_config.h in
	*** any of the following:
	$all_zfs_config_locs])
	])

	ZFS=${zfssrc}
	ZFS_OBJ=${zfsbuild}

	AC_SUBST(ZFSVERSION)
	AC_SUBST(ZFS)
	AC_SUBST(ZFS_OBJ)
])

dnl #
dnl # Detect the SPL module to be built against
dnl #
AC_DEFUN([ZFS_AC_SPL], [
	AC_ARG_WITH([spl],
		AS_HELP_STRING([--with-spl=PATH],
		[Path to spl source]),
		AS_IF([test "$withval" = "yes"],
			AC_MSG_ERROR([--with-spl=PATH requires a PATH]),
			[splsrc="$withval"]))

	dnl #
	dnl # The existence of spl.release.in is used to identify a valid
	dnl # source directory.  In order of preference:
	dnl #
	splsrc0="/var/lib/dkms/spl/${ZFSVERSION}/build"
	splsrc1="/usr/local/src/spl-${ZFSVERSION}/${LINUX_VERSION}"
	splsrc2="/usr/local/src/spl-${ZFSVERSION}"
	splsrc3="/usr/src/spl-${ZFSVERSION}/${LINUX_VERSION}"
	splsrc4="/usr/src/spl-${ZFSVERSION}"
	splsrc5="../spl/"
	splsrc6="$LINUX"

	AC_MSG_CHECKING([spl source directory])
	AS_IF([test -z "${splsrc}"], [
		[all_spl_sources="
		${splsrc0}
		${splsrc1}
		${splsrc2}
		${splsrc3}
		${splsrc4}
		${splsrc5}
		${splsrc6}"],
		AS_IF([ test -e "${splsrc0}/spl.release.in"], [
			splsrc=${splsrc0}
		], [ test -e "${splsrc1}/spl.release.in"], [
			splsrc=${splsrc1}
		], [ test -e "${splsrc2}/spl.release.in"], [
			splsrc=${splsrc2}
		], [ test -e "${splsrc3}/spl.release.in"], [
			splsrc=$(readlink -f "${splsrc3}")
		], [ test -e "${splsrc4}/spl.release.in" ], [
			splsrc=${splsrc4}
		], [ test -e "${splsrc5}/spl.release.in"], [
			splsrc=$(readlink -f "${splsrc5}")
		], [ test -e "${splsrc6}/spl.release.in" ], [
			splsrc=${splsrc6}
		], [
			splsrc="[Not found]"
		])
	], [
		[all_spl_sources="$withval"],
		AS_IF([test "$splsrc" = "NONE"], [
			splbuild=NONE
			splsrcver=NONE
		])
	])

	AC_MSG_RESULT([$splsrc])
	AS_IF([ test ! -e "$splsrc/spl.release.in"], [
		AC_MSG_ERROR([
	*** Please make sure the kmod spl devel package for your distribution
	*** is installed then try again.  If that fails you can specify the
	*** location of the spl source with the '--with-spl=PATH' option.
	*** The spl version must match the version of ZFS you are building,
	*** ${VERSION}.  Failed to find spl.release.in in the following:
	$all_spl_sources])
	])

	dnl #
	dnl # The existence of the spl_config.h is used to identify a valid
	dnl # spl object directory.  In many cases the object and source
	dnl # directory are the same, however the objects may also reside
	dnl # is a subdirectory named after the kernel version.
	dnl #
	dnl # This file is supposed to be available after DKMS finishes
	dnl # building the SPL kernel module for the target kernel.  The
	dnl # '--with-spl-timeout' option can be passed to pause here,
	dnl # waiting for the file to appear from a concurrently building
	dnl # SPL package.
	dnl #
	AC_MSG_CHECKING([spl build directory])

	all_spl_config_locs="${splsrc}/${LINUX_VERSION}
	${splsrc}"

	AS_IF([test -z "$splbuild"], [
		AS_IF([ test -e "${splsrc}/${LINUX_VERSION}/spl_config.h" ], [
			splbuild="${splsrc}/${LINUX_VERSION}"
		], [ test -e "${splsrc}/spl_config.h" ], [
			splbuild="${splsrc}"
		], [ find -L "${splsrc}" -name spl_config.h 2> /dev/null | grep -wq spl_config.h ], [
			splbuild=$(find -L "${splsrc}" -name spl_config.h | sed 's,/spl_config.h,,')
		], [
			splbuild="[Not found]"
		])
	])

	AC_MSG_RESULT([$splbuild])
	AS_IF([ ! test -e "$splbuild/spl_config.h"], [
		AC_MSG_ERROR([
	*** Please make sure the kmod spl devel <kernel> package for your
	*** distribution is installed then try again.  If that fails you
	*** can specify the location of the spl objects with the
	*** '--with-spl-obj=PATH' option.  Failed to find spl_config.h in
	*** any of the following:
	$all_spl_config_locs])
	])

	SPL=${splsrc}
	SPL_OBJ=${splbuild}

	AC_SUBST(SPL)
	AC_SUBST(SPL_OBJ)
])
