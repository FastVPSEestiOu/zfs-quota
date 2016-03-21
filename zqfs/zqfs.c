/*
 *  fs/zqfs.c
 *
 *  Copyright (C) 2005  SWsoft
 *  All rights reserved.
 *  
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/vzquota.h>
#include <linux/statfs.h>
#include <linux/virtinfo.h>
#include <linux/genhd.h>
#include <linux/reiserfs_fs.h>
#include <linux/exportfs.h>
#include <linux/seq_file.h>
#include <linux/quotaops.h>
#include <linux/string.h>

#include <asm/unistd.h>
#include <asm/uaccess.h>

#define SIMFS_GET_LOWER_FS_SB(sb) sb->s_root->d_sb

static struct super_operations zqfs_super_ops;

static void quota_get_stat(struct inode *ino, struct kstatfs *buf)
{
	int err;
	struct dq_kstat qstat;
	struct virt_info_quota q;
	long free_file, adj_file;
	s64 blk, free_blk, adj_blk;
	int bsize_bits;

	q.inode = ino;
	q.qstat = &qstat;
	err = virtinfo_notifier_call(VITYPE_QUOTA, VIRTINFO_QUOTA_GETSTAT, &q);
	if (err != NOTIFY_OK)
		return;

	bsize_bits = ffs(buf->f_bsize) - 1;
	
	if (qstat.bsoftlimit > qstat.bcurrent)
		free_blk = (qstat.bsoftlimit - qstat.bcurrent) >> bsize_bits;
	else
		free_blk = 0;
	/*
	 * In the regular case, we always set buf->f_bfree and buf->f_blocks to
	 * the values reported by quota.  In case of real disk space shortage,
	 * we adjust the values.  We want this adjustment to look as if the
	 * total disk space were reduced, not as if the usage were increased.
	 *    -- SAW
	 */
	adj_blk = 0;
	if (buf->f_bfree < free_blk)
		adj_blk = free_blk - buf->f_bfree;
	buf->f_bfree = free_blk - adj_blk;

	if (free_blk < buf->f_bavail)
		buf->f_bavail = free_blk;

	blk = (qstat.bsoftlimit >> bsize_bits) - adj_blk;
	buf->f_blocks = blk > LONG_MAX ? LONG_MAX : blk;


	free_file = 0;
	if (qstat.icurrent < qstat.isoftlimit)
		free_file = qstat.isoftlimit - qstat.icurrent;

	if (buf->f_type == REISERFS_SUPER_MAGIC)
		/*
		 * reiserfs doesn't initialize f_ffree and f_files values of
		 * kstatfs because it doesn't have an inode limit.
		 */
		buf->f_ffree = free_file;
	adj_file = 0;
	if (buf->f_ffree < free_file)
		adj_file = free_file - buf->f_ffree;
	buf->f_ffree = free_file - adj_file;
	buf->f_files = qstat.isoftlimit - adj_file;
}

static int zqfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int err;

	err = statfs_by_dentry(dentry, buf);
	if (err)
		return err;

	quota_get_stat(dentry->d_inode, buf);
	return 0;
}

static int zqfs_start_write(struct super_block *sb, int level, bool wait)
{
	struct super_block *root_sb = SIMFS_GET_LOWER_FS_SB(sb);

	if (!__sb_start_write(root_sb, level, wait))
		return 0;

	if (!(current->trans_count++)) {
		current->transaction_info = sb;
		current->flags |= PF_FSTRANS;
	} else
		WARN_ONCE((current->transaction_info != sb), "Broken fs-transaction");
	return 1;
}

static void zqfs_end_write(struct super_block *sb, int level)
{
	struct super_block *root_sb = SIMFS_GET_LOWER_FS_SB(sb);

	WARN_ONCE((current->transaction_info != sb), "Broken fs-transaction");

	if (!(--current->trans_count)) {
		current->flags &= ~PF_FSTRANS;
		current->transaction_info = NULL;
	}
	__sb_end_write(root_sb, level);
}

#ifdef CONFIG_QUOTA
static struct inode *zqfs_quota_root(struct super_block *sb)
{
	return sb->s_root->d_inode;
}
#endif

/*
 * NOTE: We need to setup s_bdev field on super block, since sys_quotactl()
 * does lookup_bdev() and get_super() which are comparing sb->s_bdev.
 * so this is a MUST if we want unmodified sys_quotactl
 * to work correctly on /dev/zqfs inside VE
 */
static int zqfs_init_blkdev(struct super_block *sb)
{
	static struct hd_struct fake_hd;
	struct block_device *blkdev;

	blkdev = bdget(sb->s_dev);
	if (blkdev == NULL)
		return -ENOMEM;

	blkdev->bd_part = &fake_hd;	/* required for bdev_read_only() */
	sb->s_bdev = blkdev;

	return 0;
}

static void zqfs_free_blkdev(struct super_block *sb)
{
	if (sb->s_bdev) {
		/* set bd_part back to NULL */
		sb->s_bdev->bd_part = NULL;
		bdput(sb->s_bdev);
	}
}

static void zqfs_quota_init(struct super_block *sb)
{
	struct virt_info_quota viq;

	viq.super = sb;
	virtinfo_notifier_call(VITYPE_QUOTA, VIRTINFO_QUOTA_ON, &viq);
}

static void zqfs_quota_free(struct super_block *sb)
{
	struct virt_info_quota viq;

	viq.super = sb;
	virtinfo_notifier_call(VITYPE_QUOTA, VIRTINFO_QUOTA_OFF, &viq);
}

static void zqfs_show_type(struct seq_file *m, struct super_block *sb)
{
#ifdef CONFIG_QUOTA
	if (vzquota_fake_fstype(current))
		seq_escape(m, VZQUOTA_FAKE_FSTYPE, " \t\n\\");
	else
#endif
		seq_escape(m, sb->s_type->name, " \t\n\\");
}

static int zqfs_show_options(struct seq_file *m, struct vfsmount *mnt)
{
#ifdef CONFIG_QUOTA
	if (sb_has_quota_loaded(mnt->mnt_sb, USRQUOTA))
		seq_puts(m, ",usrquota");
	if (sb_has_quota_loaded(mnt->mnt_sb, GRPQUOTA))
		seq_puts(m, ",grpquota");
#endif
	return 0;
}

static int zqfs_show_devname(struct seq_file *m, struct vfsmount *mnt)
{
	if (ve_is_super(get_exec_env()) && mnt->mnt_devname)
		seq_escape(m, mnt->mnt_devname, " \t\n\\");
	else
		seq_puts(m, "zqfs");

	return 0;
}

static struct super_operations zqfs_super_ops = {
#ifdef CONFIG_QUOTA
	.show_type	= &zqfs_show_type,
	.show_options	= &zqfs_show_options,
	.show_devname   = &zqfs_show_devname,
	.get_quota_root	= &zqfs_quota_root,
#endif
	.statfs = zqfs_statfs,
	.start_write	= &zqfs_start_write,
	.end_write	= &zqfs_end_write,
};

#if defined(CONFIG_EXPORTFS) || defined(CONFIG_EXPORTFS_MODULE)

#define SIM_CALL_LOWER(method, sb, args...)		\
	struct super_block *lsb;			\
	const struct export_operations *lop;		\
							\
	lsb = SIMFS_GET_LOWER_FS_SB(sb);		\
	lop = lsb->s_export_op;				\
	return lop->method(lsb, ## args)

#define SIM_CALL_DENTRY(method, dentry, args...)	\
	struct super_block *lsb;			\
	const struct export_operations *lop;		\
							\
	lsb = (dentry)->d_sb;				\
	lop = lsb->s_export_op;				\
	return lop->method(dentry, ## args)

static int zqfs_encode_fh(struct dentry *de, __u32 *fh, int *max_len,
			int connectable)
{
	SIM_CALL_DENTRY(encode_fh, de, fh, max_len, connectable);
}

static struct dentry * zqfs_fh_to_dentry(struct super_block *sb, struct fid *fid,
			int fh_len, int fh_type)
{
	SIM_CALL_LOWER(fh_to_dentry, sb, fid, fh_len, fh_type);
}

static struct dentry * zqfs_fh_to_parent(struct super_block *sb, struct fid *fid,
			int fh_len, int fh_type)
{
	SIM_CALL_LOWER(fh_to_parent, sb, fid, fh_len, fh_type);
}

static int zqfs_get_name(struct dentry *parent, char *name,
			struct dentry *child)
{
	SIM_CALL_DENTRY(get_name, parent, name, child);
}

static struct dentry * zqfs_get_parent(struct dentry *child)
{
	SIM_CALL_DENTRY(get_parent, child);
}

static int zqfs_init_export_op(struct super_block *sb, struct super_block *rsb)
{
	struct export_operations *op;

	if (rsb->s_export_op == NULL)
		return 0;

	op = kzalloc(sizeof(*op), GFP_KERNEL);
	if (op == NULL)
		return -ENOMEM;

	if (rsb->s_export_op->encode_fh)
		op->encode_fh = zqfs_encode_fh;
	if (rsb->s_export_op->fh_to_dentry)
		op->fh_to_dentry = zqfs_fh_to_dentry;
	if (rsb->s_export_op->fh_to_parent)
		op->fh_to_parent = zqfs_fh_to_parent;
	if (rsb->s_export_op->get_name)
		op->get_name = zqfs_get_name;
	if (rsb->s_export_op->get_parent)
		op->get_parent = zqfs_get_parent;

	sb->s_export_op = op;
	return 0;
}

static void zqfs_free_export_op(struct super_block *sb)
{
	kfree(sb->s_export_op);
}
#else
static int zqfs_init_export_op(struct super_block *sb, struct super_block *rsb)
{
	return 0;
}
static void zqfs_free_export_op(struct super_block *sb)
{
}
#endif

static int zqfs_fill_super(struct super_block *s, void *data, int silent)
{
	struct nameidata *nd = data;
	int err;

	err = zqfs_init_export_op(s, nd->path.dentry->d_sb);
	if (err)
		goto out;

	err = zqfs_init_blkdev(s);
	if (err)
		goto out;

	err = 0;
	s->s_fs_info = mntget(nd->path.mnt);
	s->s_root = dget(nd->path.dentry);
	s->s_op = &zqfs_super_ops;

	zqfs_quota_init(s);
out:
	return err;
}

static int zqfs_get_sb(struct file_system_type *type, int flags,
		const char *dev_name, void *opt, struct vfsmount *mnt)
{
	int err;
	struct nameidata nd;

	err = -EINVAL;
	if (opt == NULL)
		goto out;

	err = path_lookup(opt, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &nd);
	if (err)
		goto out;

	err = get_sb_nodev(type, flags, &nd, zqfs_fill_super, mnt);

	path_put(&nd.path);
out:
	return err;
}

static void zqfs_kill_sb(struct super_block *sb)
{
	dput(sb->s_root);
	sb->s_root = NULL;
	mntput((struct vfsmount *)(sb->s_fs_info));
	zqfs_free_export_op(sb);

	zqfs_quota_free(sb);
	zqfs_free_blkdev(sb);

	kill_anon_super(sb);
}

static struct file_system_type zq_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "zqfs",
	.get_sb		= zqfs_get_sb,
	.kill_sb	= zqfs_kill_sb,
	.fs_flags	= FS_HAS_NEW_FREEZE,
};

static int __init init_zqfs(void)
{
	int err;

	err = register_filesystem(&zq_fs_type);
	if (err)
		return err;

	return 0;
}

static void __exit exit_zqfs(void)
{
	unregister_filesystem(&zq_fs_type);
}

MODULE_AUTHOR("SWsoft <info@sw-soft.com>");
MODULE_DESCRIPTION("Open Virtuozzo Simulation of File System");
MODULE_LICENSE("GPL v2");

module_init(init_zqfs);
module_exit(exit_zqfs);
