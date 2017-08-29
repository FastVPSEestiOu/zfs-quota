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
dnl # AC_ZFS_HAVE_OBJECT_QUOTA checks if object quota is enabled
dnl #
AC_DEFUN([AC_ZFS_HAVE_OBJECT_QUOTA],	[
	AC_MSG_CHECKING([whether ZFS has object quota support])
	tmp_flags="$EXTRA_KCFLAGS"
	EXTRA_KCFLAGS="-I$SPL/include -I$SPL_OBJ -I$ZFS/include -I$ZFS_OBJ"
	ZFS_LINUX_TRY_COMPILE([
		#include <spl_config.h>
		#include <zfs_config.h>
		#include <sys/fs/zfs.h>
	],[
		int t = ZFS_PROP_USEROBJUSED;
		(void) t;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_ZFS_OBJECT_QUOTA, 1,
			  [Define if ZFS has object quota])
	],[
		AC_MSG_RESULT([no])
	])
	EXTRA_KCFLAGS="$tmp_flags"
])

dnl #
dnl # AC_HAVE_QUOTA_KQID_QC_DQBLK checks if kernel uses kqid and fs_disk_quota
dnl # based interface for set/get quota
dnl #
AC_DEFUN([AC_HAVE_QUOTA_KQID_QC_DQBLK], [
	AC_MSG_CHECKING([whether kernel uses kqid and qc_dqblk])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
		#include <linux/quota.h>

		int get_dqblk(struct super_block *sb, struct kqid kqid,
				struct qc_dqblk *dqblk)
		{
			return 0;
		}
	],[
		struct quotactl_ops quotactl_ops = {
			.get_dqblk = get_dqblk
		};

		(void) quotactl_ops;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_QUOTA_KQID_QC_DQBLK, 1,
			  [Define if kernel uses kqid and qc_dqblk])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_HAVE_QUOTA_KQID_FDQ checks if kernel uses kqid and fs_disk_quota
dnl # based interface for set/get quota
dnl #
AC_DEFUN([AC_HAVE_QUOTA_KQID_FDQ], [
	AC_MSG_CHECKING([whether kernel uses kqid and fs_disk_quota])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
		#include <linux/quota.h>

		int get_dqblk(struct super_block *sb, struct kqid kqid,
				struct fs_disk_quota *fdq)
		{
			return 0;
		}
	],[
		struct quotactl_ops quotactl_ops = {
			.get_dqblk = get_dqblk
		};

		(void) quotactl_ops;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_QUOTA_KQID_FDQ, 1,
			[Define if kernel uses kqid and fs_disk_quota])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_PATH_LOOKUP checks if path_lookup is accessible
dnl #
AC_DEFUN([AC_PATH_LOOKUP],	[
	AC_MSG_CHECKING([whether path_lookup is accessible])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
		#include <linux/namei.h>
	],[
		path_lookup(NULL, 0, NULL);
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_PATH_LOOKUP, 1, [Define if path_lookup is exported])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_PROC_MKDIR_DATA checks if there is proc_mkdir_data
dnl #
AC_DEFUN([AC_PROC_MKDIR_DATA],	[
	AC_MSG_CHECKING([whether proc_mkdir_data is accessible])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/proc_fs.h>
	],[
		proc_mkdir_data(NULL, 0, NULL, NULL);
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_PROC_MKDIR_DATA, 1, [Define if proc_mkdir_data is exported])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_PROC_GET_PARENT_DATA checks if there is proc_get_parent_data
dnl #
AC_DEFUN([AC_PROC_GET_PARENT_DATA],	[
	AC_MSG_CHECKING([whether proc_get_parent_data is accessible])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/proc_fs.h>
	],[
		proc_get_parent_data(NULL);
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_PROC_GET_PARENT_DATA, 1,
			  [Define if proc_get_parent_data is exported])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_HAVE_SHOW_OPTIONS_VFSMOUNT checks if there show_options' last arg
dnl # is struct vfsmount *
dnl #
AC_DEFUN([AC_HAVE_SHOW_OPTIONS_VFSMOUNT],	[
	AC_MSG_CHECKING([whether super_operations.show_options accepts vfsmount])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
	],[
		int foobar_show_options(struct seq_file *m,
					struct vfsmount *mnt)
		{
			m = m;
			mnt = mnt;
			return 0;
		}

		struct super_operations sops = {
			.show_options = foobar_show_options,
		};

		sops = sops;
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_SHOW_OPTIONS_VFSMOUNT, 1,
			  [Define if super_operations.show_options accepts vfsmount as the last argument])
	],[
		AC_MSG_RESULT([no])
	])
])

dnl #
dnl # AC_HAVE_GET_QUOTA_ROOT checks if super_operations has get_quota_root
dnl # method
dnl #
AC_DEFUN([AC_HAVE_GET_QUOTA_ROOT],	[
	AC_MSG_CHECKING([whether super_operations.get_quota_root exists])
	ZFS_LINUX_TRY_COMPILE([
		#include <linux/fs.h>
	],[
		struct super_operations sops = {};

		(void) sops.get_quota_root((struct super_block *)NULL);
	],[
		AC_MSG_RESULT([yes])
		AC_DEFINE(HAVE_GET_QUOTA_ROOT, 1,
			  [Define if super_operations has get_quota_root method])
	],[
		AC_MSG_RESULT([no])
	])
])
