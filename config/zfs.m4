dnl #
dnl # ZFS_LINUX_CONFTEST_H
dnl #
AC_DEFUN([ZFS_LINUX_CONFTEST_H], [
cat - <<_ACEOF >conftest.h
$1
_ACEOF
])

dnl #
dnl # ZFS_LINUX_CONFTEST_C
dnl #
AC_DEFUN([ZFS_LINUX_CONFTEST_C], [
cat confdefs.h - <<_ACEOF >conftest.c
$1
_ACEOF
])

dnl #
dnl # ZFS_LANG_PROGRAM(C)([PROLOGUE], [BODY])
dnl #
m4_define([ZFS_LANG_PROGRAM], [
$1
int
main (void)
{
dnl Do *not* indent the following line: there may be CPP directives.
dnl Don't move the `;' right after for the same reason.
$2
  ;
  return 0;
}
])

dnl #
dnl # ZFS_LINUX_COMPILE_IFELSE / like AC_COMPILE_IFELSE
dnl #
AC_DEFUN([ZFS_LINUX_COMPILE_IFELSE], [
	m4_ifvaln([$1], [ZFS_LINUX_CONFTEST_C([$1])])
	m4_ifvaln([$6], [ZFS_LINUX_CONFTEST_H([$6])], [ZFS_LINUX_CONFTEST_H([])])
	rm -Rf build && mkdir -p build && touch build/conftest.mod.c
	echo "obj-m := conftest.o" >build/Makefile
	modpost_flag=''
	test "x$enable_linux_builtin" = xyes && modpost_flag='modpost=true' # fake modpost stage
	AS_IF(
		[AC_TRY_COMMAND(cp conftest.c conftest.h build && make [$2] -C $LINUX_OBJ EXTRA_CFLAGS="-Werror $EXTRA_KCFLAGS" $ARCH_UM M=$PWD/build $modpost_flag) >/dev/null && AC_TRY_COMMAND([$3])],
		[$4],
		[_AC_MSG_LOG_CONFTEST m4_ifvaln([$5],[$5])]
	)
	rm -Rf build
])

dnl #
dnl # ZFS_LINUX_TRY_COMPILE like AC_TRY_COMPILE
dnl #
AC_DEFUN([ZFS_LINUX_TRY_COMPILE],
	[ZFS_LINUX_COMPILE_IFELSE(
	[AC_LANG_SOURCE([ZFS_LANG_PROGRAM([[$1]], [[$2]])])],
	[modules],
	[test -s build/conftest.o],
	[$3], [$4])
])

dnl #
dnl # ZFS_AC_OBJECT_QUOTA checks if object quota is enabled
dnl #
AC_DEFUN([ZFS_AC_OBJECT_QUOTA],	[
	AC_MSG_CHECKING([whether ZFS has object quota support])
	tmp_flags="$EXTRA_KCFLAGS"
	EXTRA_KCFLAGS="-I$SPL/include -I$SPL_OBJ -I$ZFS/include -I$ZFS_OBJ"
	ZFS_LINUX_TRY_COMPILE([
		#include <spl_config.h>
		#include <zfs_config.h>
		#include <sys/zfs_context.h>
		#include <sys/types.h>
		#include <sys/zfs_vfsops.h>
	],[
		int t = ZFS_IOC_USEROBJSPACE_UPGRADE;
		(void) t;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_OBJECT_QUOTA, 1, [Define if ZFS has object quota])
	],[
		AC_MSG_RESULT([no])
	])
	EXTRA_KCFLAGS="$tmp_flags"
])

dnl #
dnl # AC_QUOTA_KQID checks if kernel uses kqid based interface for set/get
dnl # quota and fs_disk_quota
dnl #
AC_DEFUN([AC_QUOTA_KQID],	[
	AC_MSG_CHECKING([whether kernel uses kqid_t and fs_disk_quota])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
		#include <linux/quota.h>
	],[
		int get_dqblk(struct super_block *sb, struct kqid kqid,
				struct fs_disk_quota *fdq)
		{
			return 0;
		}

		struct quotactl_ops quotactl_ops = {
			.get_dqblk = get_dqblk
		};

		(void) quotactl_ops;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(QUOTA_KQID, 1, [Define if kernel uses kqid and fs_disk_quota])
	],[
		AC_MSG_RESULT([no])
	])
])
