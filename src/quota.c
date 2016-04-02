
#include <linux/kernel.h>
#include <linux/module.h>

#ifdef QUOTA_KQID
#	include <linux/uidgid.h>
#	include <linux/projid.h>
#endif /* #ifdef QUOTA_KQID */

#ifdef CONFIG_VE
#	include <linux/vzquota.h>
#	include <linux/virtinfo.h>
#	include <linux/ve_proto.h>
#else /* #ifdef CONFIG_VE */
#	include <linux/fs.h>
#	include <linux/quota.h>
#endif /* #else #ifdef CONFIG_VE */

#include "tree.h"

static int zfsquota_get_dqblk(struct super_block *sb, int type,
		       qid_t id, struct if_dqblk *di)
{
	memset(di, 0, sizeof(*di));

	return zqtree_get_quota_dqblk(sb, type, id, di);
}

static int zfsquota_set_dqblk(struct super_block *sb, int type,
			      qid_t id, struct if_dqblk *di)
{
	return zqtree_set_quota_dqblk(sb, type, id, di);
}

#ifdef HAVE_QUOTA_KQID_FDQ
static void copy_to_if_dqblk(struct if_dqblk *dst, struct fs_disk_quota *src)
{
	dst->dqb_bhardlimit = src->d_blk_hardlimit;
	dst->dqb_bsoftlimit = src->d_blk_softlimit;
	dst->dqb_curspace = src->d_bcount;
	dst->dqb_ihardlimit = src->d_ino_hardlimit;
	dst->dqb_isoftlimit = src->d_ino_softlimit;
	dst->dqb_curinodes = src->d_icount;
	dst->dqb_btime = src->d_btimer;
	dst->dqb_itime = src->d_itimer;
	dst->dqb_valid = QIF_ALL;
}

static void copy_from_if_dqblk(struct fs_disk_quota *dst, struct if_dqblk *src)
{
	dst->d_blk_hardlimit = src->dqb_bhardlimit;
	dst->d_blk_softlimit  = src->dqb_bsoftlimit;
	dst->d_bcount = src->dqb_curspace;
	dst->d_ino_hardlimit = src->dqb_ihardlimit;
	dst->d_ino_softlimit = src->dqb_isoftlimit;
	dst->d_icount = src->dqb_curinodes;
	dst->d_btimer = src->dqb_btime;
	dst->d_itimer = src->dqb_itime;

	dst->d_fieldmask = 0;
	if (src->dqb_valid & QIF_BLIMITS)
		dst->d_fieldmask |= FS_DQ_BSOFT | FS_DQ_BHARD;
	if (src->dqb_valid & QIF_SPACE)
		dst->d_fieldmask |= FS_DQ_BCOUNT;
	if (src->dqb_valid & QIF_ILIMITS)
		dst->d_fieldmask |= FS_DQ_ISOFT | FS_DQ_IHARD;
	if (src->dqb_valid & QIF_INODES)
		dst->d_fieldmask |= FS_DQ_ICOUNT;
	if (src->dqb_valid & QIF_BTIME)
		dst->d_fieldmask |= FS_DQ_BTIMER;
	if (src->dqb_valid & QIF_ITIME)
		dst->d_fieldmask |= FS_DQ_ITIMER;
}

#	define	quota_struct	fs_disk_quota
#endif /* HAVE_QUOTA_KQID_FDQ */

#ifdef HAVE_QUOTA_KQID_QC_DQBLK
static inline qsize_t qbtos(qsize_t blocks)
{
	return blocks << QIF_DQBLKSIZE_BITS;
}

static inline qsize_t stoqb(qsize_t space)
{
	return (space + QIF_DQBLKSIZE - 1) >> QIF_DQBLKSIZE_BITS;
}

static void copy_to_if_dqblk(struct if_dqblk *dst, struct qc_dqblk *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->dqb_bhardlimit = stoqb(src->d_spc_hardlimit);
	dst->dqb_bsoftlimit = stoqb(src->d_spc_softlimit);
	dst->dqb_curspace = src->d_space;
	dst->dqb_ihardlimit = src->d_ino_hardlimit;
	dst->dqb_isoftlimit = src->d_ino_softlimit;
	dst->dqb_curinodes = src->d_ino_count;
	dst->dqb_btime = src->d_spc_timer;
	dst->dqb_itime = src->d_ino_timer;
	dst->dqb_valid = QIF_ALL;
}

static void copy_from_if_dqblk(struct qc_dqblk *dst, struct if_dqblk *src)
{
	dst->d_spc_hardlimit = qbtos(src->dqb_bhardlimit);
	dst->d_spc_softlimit = qbtos(src->dqb_bsoftlimit);
	dst->d_space = src->dqb_curspace;
	dst->d_ino_hardlimit = src->dqb_ihardlimit;
	dst->d_ino_softlimit = src->dqb_isoftlimit;
	dst->d_ino_count = src->dqb_curinodes;
	dst->d_spc_timer = src->dqb_btime;
	dst->d_ino_timer = src->dqb_itime;

	dst->d_fieldmask = 0;
	if (src->dqb_valid & QIF_BLIMITS)
		dst->d_fieldmask |= QC_SPC_SOFT | QC_SPC_HARD;
	if (src->dqb_valid & QIF_SPACE)
		dst->d_fieldmask |= QC_SPACE;
	if (src->dqb_valid & QIF_ILIMITS)
		dst->d_fieldmask |= QC_INO_SOFT | QC_INO_HARD;
	if (src->dqb_valid & QIF_INODES)
		dst->d_fieldmask |= QC_INO_COUNT;
	if (src->dqb_valid & QIF_BTIME)
		dst->d_fieldmask |= QC_SPC_TIMER;
	if (src->dqb_valid & QIF_ITIME)
		dst->d_fieldmask |= QC_INO_TIMER;
}

#	define quota_struct	qc_dqblk
#endif /* HAVE_QUOTA_KQID_QC_DQBLK */

static int get_qid_type(struct kqid kqid, qid_t *pqid, int *ptype)
{
	if (!pqid || !ptype)
		return -EFAULT;

	switch(kqid.type) {
	case USRQUOTA:
		*pqid = __kuid_val(kqid.uid);
		*ptype = USRQUOTA;
		break;
	case GRPQUOTA:
		*pqid = __kgid_val(kqid.gid);
		*ptype = GRPQUOTA;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

#if defined(HAVE_QUOTA_KQID_QC_DQBLK) || defined(HAVE_QUOTA_KQID_FDQ)
static int zfsquota_get_quota_struct(struct super_block *sb, struct kqid kqid,
		       struct quota_struct *fdq)
{
	qid_t qid;
	int type, ret;
	struct if_dqblk dqblk;

	ret = get_qid_type(kqid, &qid, &type);
	if (ret)
		return ret;

	ret = zfsquota_get_dqblk(sb, type, qid, &dqblk);

	if (ret == 0)
		copy_from_if_dqblk(fdq, &dqblk);

	return ret;
}

static int zfsquota_set_quota_struct(struct super_block *sb, struct kqid kqid,
		       struct quota_struct *fdq)
{
	qid_t qid;
	int type, ret;
	struct if_dqblk dqblk;

	ret = get_qid_type(kqid, &qid, &type);
	if (ret)
		return ret;

	copy_to_if_dqblk(&dqblk, fdq);

	ret = zfsquota_set_dqblk(sb, type, qid, &dqblk);
	return ret;
}
#endif /* defined(HAVE_QUOTA_KQID_QC_DQBLK) || defined(HAVE_QUOTA_KQID_FDQ) */

static int zfsquota_get_info(struct super_block *sb, int type,
			     struct if_dqinfo *ii)
{
	memset(ii, 0, sizeof(*ii));
	return 0;
}

static int zfsquota_set_info(struct super_block *sb, int type,
			     struct if_dqinfo *ii)
{
	return 0;
}

#ifdef CONFIG_QUOTA_COMPAT
static int zfsquota_get_quoti(struct super_block *sb, int type, qid_t idx,
			      struct v2_disk_dqblk __user * dqblk)
{
	return 0;
}
#endif /* #ifdef CONFIG_QUOTA_COMPAT */

static int zfsquota_sync(struct super_block *sb, int type)
{
	if (type == -1) {
		zqtree_zfs_sync_tree(sb, USRQUOTA);
		zqtree_zfs_sync_tree(sb, GRPQUOTA);
	} else
		zqtree_zfs_sync_tree(sb, type);
	return 0;
}

struct quotactl_ops zfsquota_q_cops = {
	.quota_sync = zfsquota_sync,
	.get_info = zfsquota_get_info,
	.set_info = zfsquota_set_info,
#if defined(HAVE_QUOTA_KQID_QC_DQBLK) || defined(HAVE_QUOTA_KQID_FDQ)
	.get_dqblk = zfsquota_get_quota_struct,
	.set_dqblk = zfsquota_set_quota_struct,
#else
	.get_dqblk = zfsquota_get_dqblk,
	.set_dqblk = zfsquota_set_dqblk,
#endif
#ifdef CONFIG_QUOTA_COMPAT
	.get_quoti = zfsquota_get_quoti,
#endif
};

struct quota_format_type zfs_quota_empty_vfsold_format = {
	.qf_fmt_id = QFMT_VFS_OLD,
	.qf_ops = NULL,
	.qf_owner = THIS_MODULE,
};

struct quota_format_type zfs_quota_empty_vfsv2_format = {
	.qf_fmt_id = QFMT_VFS_V1,
	.qf_ops = NULL,
	.qf_owner = THIS_MODULE,
};

int zfsquota_notify_quota_on(struct super_block *sb)
{
	const char *fsname;
#ifdef CONFIG_VE
	fsname = sb->s_op->get_quota_root(sb)->i_sb->s_type->name;
#else /* #ifdef CONFIG_VE */
	fsname = sb->s_root->d_inode->i_sb->s_type->name;
#endif /* #else #ifdef CONFIG_VE */
	if (strcmp(fsname, "zfs")) {
		return NOTIFY_OK;
	}
	if (!try_module_get(THIS_MODULE))
		return NOTIFY_BAD;

	sb->s_qcop = &zfsquota_q_cops;
	sb->s_dquot.flags = dquot_state_flag(DQUOT_USAGE_ENABLED, USRQUOTA) |
	    dquot_state_flag(DQUOT_USAGE_ENABLED, GRPQUOTA);

#ifdef USE_VFSOLD_FORMAT
	sb->s_dquot.info[USRQUOTA].dqi_format = &zfs_quota_empty_vfsold_format;
	sb->s_dquot.info[GRPQUOTA].dqi_format = &zfs_quota_empty_vfsold_format;
#else
	sb->s_dquot.info[USRQUOTA].dqi_format = &zfs_quota_empty_vfsv2_format;
	sb->s_dquot.info[GRPQUOTA].dqi_format = &zfs_quota_empty_vfsv2_format;
#endif

	if (zqtree_init_superblock(sb))
		return NOTIFY_BAD;

	return NOTIFY_OK;
}
EXPORT_SYMBOL(zfsquota_notify_quota_on);

int zfsquota_notify_quota_off(struct super_block *sb)
{
	if (sb->s_qcop != &zfsquota_q_cops) {
		return NOTIFY_OK;
	}
	zqtree_free_superblock(sb);
	module_put(THIS_MODULE);

	return NOTIFY_OK;
}
EXPORT_SYMBOL(zfsquota_notify_quota_off);

#ifdef CONFIG_VE
static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		err = zfsquota_notify_quota_on(viq->super);
		break;
	case VIRTINFO_QUOTA_OFF:
		err = zfsquota_notify_quota_off(viq->super);
		break;
	}
	return err;
}

struct vnotifier_block zfsquota_notifier_block = {
	.notifier_call = zfsquota_notifier_call,
	.priority = INT_MAX / 2
};
#endif /* #ifdef CONFIG_VE */

int __init zfsquota_proc_init(void);
void __exit zfsquota_proc_exit(void);
int __init zfsquota_proc_ve_init(void);
void __exit zfsquota_proc_ve_exit(void);
int __init zfsquota_tree_init(void);
int __init zfsquota_tree_exit(void);

static int __init zfsquota_init(void)
{
	zfsquota_proc_init();
	zfsquota_tree_init();

#ifdef CONFIG_VE
	zfsquota_proc_ve_init();
	virtinfo_notifier_register(VITYPE_QUOTA, &zfsquota_notifier_block);
#endif /* #ifdef CONFIG_VE */

	register_quota_format(&zfs_quota_empty_vfsold_format);
	return 0;
}

static void __exit zfsquota_exit(void)
{
	zfsquota_proc_exit();
	zfsquota_tree_exit();

#ifdef CONFIG_VE
	zfsquota_proc_ve_exit();
	virtinfo_notifier_unregister(VITYPE_QUOTA, &zfsquota_notifier_block);
#endif /* #ifdef CONFIG_VE */

	unregister_quota_format(&zfs_quota_empty_vfsold_format);

	return;
}

MODULE_AUTHOR("Pavel Boldin <boldin.pavel@gmail.com>");
MODULE_DESCRIPTION("ZFS quota <-> OpenVZ proxy");
MODULE_LICENSE("GPL");

module_init(zfsquota_init);
module_exit(zfsquota_exit);
