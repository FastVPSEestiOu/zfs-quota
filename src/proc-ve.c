
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>
#include <linux/ve_proto.h>

#include <linux/fs_struct.h>
#include <linux/sched.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"

#define DQBLOCK_SIZE 1024

static const char quota_user[] = "quota.user";
static const char quota_group[] = "quota.group";
static const char aquota_user[] = "aquota.user";
static const char aquota_group[] = "aquota.group";
static struct proc_dir_entry *glob_zfsquota_proc;

extern struct quotactl_ops zfsquota_q_cops;

/* ----------------------------------------------------------------------
 *
 * /proc/vz/vzaquota/QID/aquota.* files
 *
 * FIXME: this code lacks serialization of read/readdir/lseek.
 * However, this problem should be fixed after the mainstream issue of what
 * appears to be non-atomic read and update of file position in sys_read.
 *
 * --------------------------------------------------------------------- */

static struct inode_operations zfs_aquotf_inode_operations = {
};

/* ----------------------------------------------------------------------
 *
 * /proc/vz/vzaquota/QID directory
 *
 * --------------------------------------------------------------------- */

static int zfs_aquotq_readdir(struct file *file, void *data, filldir_t filler)
{
	loff_t n;
	int err;
	printk("%s\n", __func__);

	n = file->f_pos;
	for (err = 0; !err; n++) {
		/* ppc32 can't cmp 2 long long's in switch, calls __cmpdi2() */
		switch ((unsigned long)n) {
		case 0:
			err = (*filler) (data, ".", 1, n,
					 file->f_dentry->d_inode->i_ino,
					 DT_DIR);
			break;
		case 1:
			err = (*filler) (data, "..", 2, n,
					 parent_ino(file->f_dentry), DT_DIR);
			break;
		case 2:
			err = (*filler) (data, quota_user,
					 sizeof(quota_user) - 1, n,
					 file->f_dentry->d_inode->i_ino
					 + USRQUOTA + 1, DT_REG);
			break;
		case 3:
			err = (*filler) (data, quota_group,
					 sizeof(quota_group) - 1, n,
					 file->f_dentry->d_inode->i_ino
					 + GRPQUOTA + 1, DT_REG);
			break;
		case 4:
			err = (*filler) (data, aquota_user,
					 sizeof(aquota_user) - 1, n,
					 file->f_dentry->d_inode->i_ino
					 + USRQUOTA + 10 + 1, DT_REG);
			break;
		case 5:
			err = (*filler) (data, aquota_group,
					 sizeof(aquota_group) - 1, n,
					 file->f_dentry->d_inode->i_ino
					 + GRPQUOTA + 10 + 1, DT_REG);
			break;
		default:
			goto out;
		}
	}
out:
	file->f_pos = n;
	return err;
}

struct zfs_aquotq_lookdata {
	dev_t dev;
	int type, fmt;
};

static int zfs_aquotq_looktest(struct inode *inode, void *data)
{
	struct zfs_aquotq_lookdata *d = data;
	return inode->i_op == &zfs_aquotf_inode_operations &&
	    zfs_aquot_getdev(inode->i_ino) == d->dev &&
	    zfs_aquot_type(inode->i_ino) == d->type + 1;
}

int zfs_aquotq_vfsold_lookset(struct inode *inode);
int zfs_aquotq_vfsv2r1_lookset(struct inode *inode);

static int zfs_aquotq_lookset(struct inode *inode, void *data)
{
	struct zfs_aquotq_lookdata *d;

	d = data;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = zfs_aquot_getino(d->dev, d->type + 1);
	inode->i_mode = S_IFREG | S_IRUSR;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_nlink = 1;
	inode->i_op = &zfs_aquotf_inode_operations;

	if (d->fmt == QFMT_VFS_OLD) {
		zfs_aquotq_vfsold_lookset(inode);
	} else if (d->fmt == QFMT_VFS_V1) {
		zfs_aquotq_vfsv2r1_lookset(inode);
	}
	return 0;
}

static int zfs_aquotq_revalidate(struct dentry *vdentry, struct nameidata *nd)
{
	return 0;
}

static struct dentry_operations zfs_aquotq_dentry_operations = {
	.d_revalidate = &zfs_aquotq_revalidate,
};

static struct dentry *zfs_aquotq_lookup(struct inode *dir,
					struct dentry *dentry,
					struct nameidata *nd)
{
	struct inode *inode;
	struct zfs_aquotq_lookdata d;
	int k, fmt;
	printk("%s\n", __func__);

	if (dentry->d_name.len == sizeof(quota_user) - 1) {
		if (memcmp(dentry->d_name.name, quota_user,
			   sizeof(quota_user) - 1))
			goto out;
		k = USRQUOTA;
		fmt = QFMT_VFS_OLD;
	} else if (dentry->d_name.len == sizeof(quota_group) - 1 ||
		   dentry->d_name.len == sizeof(aquota_user) - 1) {

		if (!memcmp(dentry->d_name.name, quota_group,
			    sizeof(quota_group) - 1)) {
			k = GRPQUOTA;
			fmt = QFMT_VFS_OLD;
		} else if (!memcmp(dentry->d_name.name, aquota_user,
				   sizeof(aquota_user) - 1)) {
			k = USRQUOTA;
			fmt = QFMT_VFS_V1;
		} else
			goto out;
	} else if (dentry->d_name.len == sizeof(aquota_group) - 1) {
		if (memcmp(dentry->d_name.name, aquota_group,
			   sizeof(aquota_group) - 1))
			goto out;
		k = GRPQUOTA;
		fmt = QFMT_VFS_V1;
	} else
		goto out;
	d.dev = zfs_aquot_getdev(dir->i_ino);
	d.type = k;
	d.fmt = fmt;

	inode = iget5_locked(dir->i_sb, dir->i_ino + k + fmt * 10 + 1,
			     zfs_aquotq_looktest, zfs_aquotq_lookset, &d);

	if (inode == NULL)
		goto out;

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);
	dentry->d_op = &zfs_aquotq_dentry_operations;
	d_add(dentry, inode);
	return NULL;

out:
	return ERR_PTR(-ENOENT);
}

static struct file_operations zfs_aquotq_file_operations = {
	.read = &generic_read_dir,
	.readdir = &zfs_aquotq_readdir,
};

static struct inode_operations zfs_aquotq_inode_operations = {
	.lookup = &zfs_aquotq_lookup,
};

/* ----------------------------------------------------------------------
 *
 * /proc/vz/vzaquota directory
 *
 * --------------------------------------------------------------------- */

struct zfs_aquot_de {
	struct list_head list;
	struct vfsmount *mnt;
};

static int zfs_aquot_buildmntlist(struct ve_struct *ve, struct list_head *head)
{
	struct vfsmount *mnt;
	struct path root;
	struct zfs_aquot_de *p;
	int err;

	root = ve->root_path;
	path_get(&root);

	mnt = root.mnt;
	spin_lock(&vfsmount_lock);
	while (1) {
		list_for_each_entry(p, head, list) {
			if (p->mnt->mnt_sb == mnt->mnt_sb)
				goto skip;
		}

		err = -ENOMEM;
		p = kmalloc(sizeof(*p), GFP_ATOMIC);
		if (p == NULL)
			goto out;
		p->mnt = mntget(mnt);
		list_add_tail(&p->list, head);

skip:
		err = 0;
		if (list_empty(&mnt->mnt_mounts)) {
			while (1) {
				if (mnt == root.mnt)
					goto out;
				if (mnt->mnt_child.next !=
				    &mnt->mnt_parent->mnt_mounts)
					break;
				mnt = mnt->mnt_parent;
			}
			mnt = list_entry(mnt->mnt_child.next,
					 struct vfsmount, mnt_child);
		} else
			mnt = list_entry(mnt->mnt_mounts.next,
					 struct vfsmount, mnt_child);
	}
out:
	spin_unlock(&vfsmount_lock);
	path_put(&root);
	return err;
}

static void zfs_aquot_releasemntlist(struct ve_struct *ve,
				     struct list_head *head)
{
	struct zfs_aquot_de *p;

	while (!list_empty(head)) {
		p = list_entry(head->next, typeof(*p), list);
		mntput(p->mnt);
		list_del(&p->list);
		kfree(p);
	}
}
static int zfs_aquotd_readdir(struct file *file, void *data, filldir_t filler)
{
	struct ve_struct *ve, *old_ve;
	struct list_head mntlist;
	struct zfs_aquot_de *de;
	struct super_block *sb;
	loff_t i, n;
	char buf[24];
	int l, err;

	printk("%s\n", __func__);

	i = 0;
	n = file->f_pos;

	INIT_LIST_HEAD(&mntlist);
	ve = file->f_dentry->d_sb->s_type->owner_env;
	old_ve = set_exec_env(ve);

	/*
	 * The only reason of disabling readdir for the host system is that
	 * this readdir can be slow and CPU consuming with large number of VPSs
	 * (or just mount points).
	 */
	err = ve_is_super(ve);

	if (!err) {
		err = zfs_aquot_buildmntlist(ve, &mntlist);
		if (err)
			goto out_err;
	}

	if (i >= n) {
		if ((*filler) (data, ".", 1, i,
			       file->f_dentry->d_inode->i_ino, DT_DIR))
			goto out_fill;
	}
	i++;

	if (i >= n) {
		if ((*filler) (data, "..", 2, i,
			       parent_ino(file->f_dentry), DT_DIR))
			goto out_fill;
	}
	i++;

	list_for_each_entry(de, &mntlist, list) {
		sb = de->mnt->mnt_sb;
		if (get_device_perms_ve(S_IFBLK, sb->s_dev, FMODE_QUOTACTL))
			continue;

		if (sb->s_qcop != &zfsquota_q_cops)
			continue;

		i++;
		if (i <= n)
			continue;

		l = sprintf(buf, "%08x", new_encode_dev(sb->s_dev));
		if ((*filler) (data, buf, l, i - 1,
			       zfs_aquot_getino(sb->s_dev, 0), DT_DIR))
			break;
	}

out_fill:
	err = 0;
	file->f_pos = i;
out_err:
	zfs_aquot_releasemntlist(ve, &mntlist);
	(void) set_exec_env(old_ve);
	return err;
}

static int zfs_aquotd_looktest(struct inode *inode, void *data)
{
	return inode->i_op == &zfs_aquotq_inode_operations &&
	    zfs_aquot_getdev(inode->i_ino) == (dev_t) (unsigned long)data;
}

static int zfs_aquotd_lookset(struct inode *inode, void *data)
{
	dev_t dev;

	dev = (dev_t) (unsigned long)data;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = zfs_aquot_getino(dev, 0);
	inode->i_mode = S_IFDIR | S_IRUSR | S_IXUSR;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_nlink = 2;
	inode->i_op = &zfs_aquotq_inode_operations;
	inode->i_fop = &zfs_aquotq_file_operations;
	return 0;
}

static struct dentry *zfs_aquotd_lookup(struct inode *dir,
					struct dentry *dentry,
					struct nameidata *nd)
{
	struct ve_struct *ve, *old_ve;
	const unsigned char *s;
	int l;
	dev_t dev;
	struct inode *inode;

	printk("%s\n", __func__);

	ve = dir->i_sb->s_type->owner_env;
	old_ve = set_exec_env(ve);
	/*
	 * Lookup is much lighter than readdir, so it can be allowed for the
	 * host system.  But it would be strange to be able to do lookup only
	 * without readdir...
	 */
	if (ve_is_super(ve))
		goto out;

	dev = 0;
	l = dentry->d_name.len;
	if (l <= 0)
		goto out;
	for (s = dentry->d_name.name; l > 0; s++, l--) {
		if (!isxdigit(*s))
			goto out;
		if (dev & ~(~0UL >> 4))
			goto out;
		dev <<= 4;
		if (isdigit(*s))
			dev += *s - '0';
		else if (islower(*s))
			dev += *s - 'a' + 10;
		else
			dev += *s - 'A' + 10;
	}
	dev = new_decode_dev(dev);

	if (get_device_perms_ve(S_IFBLK, dev, FMODE_QUOTACTL))
		goto out;

	inode = iget5_locked(dir->i_sb, zfs_aquot_getino(dev, 0),
			     zfs_aquotd_looktest, zfs_aquotd_lookset,
			     (void *)(unsigned long)dev);
	if (inode == NULL)
		goto out;

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);

	d_add(dentry, inode);
	(void)set_exec_env(old_ve);
	return NULL;

out:
	(void)set_exec_env(old_ve);
	return ERR_PTR(-ENOENT);
}

static int zfs_aquotd_getattr(struct vfsmount *mnt, struct dentry *dentry,
			      struct kstat *stat)
{
	struct ve_struct *ve, *old_ve;
	struct list_head mntlist, *pos;

	generic_fillattr(dentry->d_inode, stat);
	ve = dentry->d_sb->s_type->owner_env;

	/*
	 * The only reason of disabling getattr for the host system is that
	 * this getattr can be slow and CPU consuming with large number of VPSs
	 * (or just mount points).
	 */
	if (ve_is_super(ve))
		return 0;

	INIT_LIST_HEAD(&mntlist);
	old_ve = set_exec_env(ve);
	if (!zfs_aquot_buildmntlist(ve, &mntlist))
		list_for_each(pos, &mntlist)
		    stat->nlink++;
	zfs_aquot_releasemntlist(ve, &mntlist);
	(void)set_exec_env(old_ve);
	return 0;
}

static struct file_operations zfs_aquotd_file_operations = {
	.read = &generic_read_dir,
	.readdir = &zfs_aquotd_readdir,
};

static struct inode_operations zfs_aquotd_inode_operations = {
	.lookup = &zfs_aquotd_lookup,
	.getattr = &zfs_aquotd_getattr,
};

int __init zfsquota_proc_init(void)
{
	glob_zfsquota_proc =
	    create_proc_entry("zfsquota", S_IFDIR | S_IRUSR | S_IXUSR,
			      glob_proc_vz_dir
			    );
	glob_zfsquota_proc->proc_iops = &zfs_aquotd_inode_operations;
	glob_zfsquota_proc->proc_fops = &zfs_aquotd_file_operations;

	return 0;
}

void __exit zfsquota_proc_exit(void)
{
	remove_proc_entry("zfsquota", glob_proc_vz_dir
			);
}
