
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "tree.h"

#define DQBLOCK_SIZE 1024

static const char quota_user[] = "quota.user";
static const char quota_group[] = "quota.group";
static const char aquota_user[] = "aquota.user";
static const char aquota_group[] = "aquota.group";
static struct proc_dir_entry *glob_zfsquota_proc;

extern struct quotactl_ops zfsquota_q_cops;

struct v1_disk_dqblk {
	__u32 dqb_bhardlimit;	/* absolute limit on disk blks alloc */
	__u32 dqb_bsoftlimit;	/* preferred limit on disk blks */
	__u32 dqb_curblocks;	/* current block count */
	__u32 dqb_ihardlimit;	/* absolute limit on allocated inodes */
	__u32 dqb_isoftlimit;	/* preferred inode limit */
	__u32 dqb_curinodes;	/* current # allocated inodes */
	time_t dqb_btime;	/* time limit for excessive disk use */
	time_t dqb_itime;	/* time limit for excessive inode use */
};

static int read_v1_disk_dqblk(void *sb, void *buf, int type, qid_t qid)
{
	struct v1_disk_dqblk *v1 = buf;
	struct quota_data *quota_data;

	quota_data = zqtree_get_quota_data(sb, type, qid, 0);
	if (!quota_data)
		return -EIO;

	v1->dqb_bsoftlimit = v1->dqb_bhardlimit = quota_data->space_quota;
	v1->dqb_curblocks = quota_data->space_used;
#ifdef USEROBJ_QUOTA
	v1->dqb_ihardlimit = v1->dqb_isoftlimit = quota_data->obj_quota;
	v1->dqb_curinodes = quota_data->obj_used;
#endif
	v1->dqb_btime = v1->dqb_itime = 0;

	return sizeof(*v1);
}

/*
 * FIXME: this function can handle quota files up to 2GB only.
 */
static int read_proc_quotafile(char *page, off_t off, int count,
			       void *sb, int type)
{
	off_t blk_num, buf_off;
	char *tmp;
	ssize_t buf_size;
	int res = 0;
	printk("%s\n", __func__);

	if (off >= 1024 * 40)
		return 0;

	tmp = kmalloc(sizeof(struct v1_disk_dqblk), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	buf_off = 0;
	buf_size =
	    (count / sizeof(struct v1_disk_dqblk)) *
	    sizeof(struct v1_disk_dqblk);

	blk_num = off / sizeof(struct v1_disk_dqblk);

	while (buf_size > 0) {
		//printk("buf_size = %lu, buf_off = %lu, blk_num = %lu\n", buf_size, buf_off, blk_num);

		res = read_v1_disk_dqblk(sb, tmp, type, blk_num);
		if (res < 0)
			goto out_dq;
		memcpy(page + buf_off, tmp,
		       min((size_t) buf_size, (size_t) res));

		buf_size -= res;
		buf_off += res;

		blk_num++;
	}
	res = buf_off;

out_dq:
	kfree(tmp);

	return res;
}

/* ----------------------------------------------------------------------
 *
 * /proc/vz/vzaquota/QID/aquota.* files
 *
 * FIXME: this code lacks serialization of read/readdir/lseek.
 * However, this problem should be fixed after the mainstream issue of what
 * appears to be non-atomic read and update of file position in sys_read.
 *
 * --------------------------------------------------------------------- */

static inline unsigned long zfs_aquot_getino(dev_t dev)
{
	return 0xec000000UL + dev;
}

static inline dev_t zfs_aquot_getidev(struct inode *inode)
{
	return (dev_t) (unsigned long)PROC_I(inode)->op.proc_get_link;
}

static inline void zfs_aquot_setidev(struct inode *inode, dev_t dev)
{
	PROC_I(inode)->op.proc_get_link = (void *)(unsigned long)dev;
}

static ssize_t zfs_aquotf_read(struct file *file,
			       char __user * buf, size_t size, loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	struct inode *inode;
	struct block_device *bdev;
	struct super_block *sb;
	int err, type;
	printk("%s\n", __func__);

	err = -ENOMEM;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (page == NULL)
		goto out_err;

	err = -ENODEV;
	inode = file->f_dentry->d_inode;
	bdev = bdget(zfs_aquot_getidev(inode));
	if (bdev == NULL)
		goto out_err;
	sb = get_super(bdev);
	type = PROC_I(inode)->fd - 1;
	bdput(bdev);
	if (sb == NULL)
		goto out_err;
	drop_super(sb);

	copied = 0;
	l = l2 = 0;
	while (1) {
		bufsize = min(size, (size_t) PAGE_SIZE);
		if (bufsize <= 0)
			break;

		l = read_proc_quotafile(page, *ppos, bufsize, sb, type);
		if (l <= 0)
			break;

		l2 = copy_to_user(buf, page, l);
		copied += l - l2;
		if (l2)
			break;

		buf += l;
		size -= l;
		*ppos += l;
		l = l2 = 0;
	}

	free_page((unsigned long)page);
	if (copied)
		return copied;
	else if (l2)		/* last copy_to_user failed */
		return -EFAULT;
	else			/* read error or EOF */
		return l;

out_err:
	if (page != NULL)
		free_page((unsigned long)page);
	return err;
}

static struct file_operations zfs_aquotf_file_operations = {
	.read = &zfs_aquotf_read,
};

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
	int type;
};

static int zfs_aquotq_looktest(struct inode *inode, void *data)
{
	struct zfs_aquotq_lookdata *d = data;
	return inode->i_op == &zfs_aquotf_inode_operations &&
	    zfs_aquot_getidev(inode) == d->dev &&
	    PROC_I(inode)->fd == d->type + 1;
}

static int zfs_aquotq_lookset(struct inode *inode, void *data)
{
	struct zfs_aquotq_lookdata *d;

	d = data;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = zfs_aquot_getino(d->dev) + d->type + 1;
	inode->i_mode = S_IFREG | S_IRUSR;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_nlink = 1;
	inode->i_op = &zfs_aquotf_inode_operations;
	inode->i_fop = &zfs_aquotf_file_operations;
	PROC_I(inode)->fd = d->type + 1;
	zfs_aquot_setidev(inode, d->dev);

	/* Setting size */
	inode->i_size = 100 * sizeof(struct v1_disk_dqblk);
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
	int k;
	printk("%s\n", __func__);

	if (dentry->d_name.len == sizeof(quota_user) - 1) {
		if (memcmp(dentry->d_name.name, quota_user,
			   sizeof(quota_user) - 1))
			goto out;
		k = USRQUOTA;
	} else if (dentry->d_name.len == sizeof(quota_group) - 1) {
		if (memcmp(dentry->d_name.name, quota_group,
			   sizeof(quota_group) - 1))
			goto out;
		k = GRPQUOTA;
	} else
		goto out;
	d.dev = zfs_aquot_getidev(dir);
	d.type = k;

	inode = iget5_locked(dir->i_sb, dir->i_ino + k + 1,
			     zfs_aquotq_looktest, zfs_aquotq_lookset, &d);

	/* qmlbk ref is not needed, we used it for i_size calculation only */
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
	printk("%s\n", __func__);

#ifdef CONFIG_VE
	root = ve->root_path;
	path_get(&root);
#else
	get_fs_root(current->fs, &root)
#endif
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
	printk("%s\n", __func__);

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
	ve = file->f_dentry->d_sb->s_type->owner_env;
	old_ve = set_exec_env(ve);

	INIT_LIST_HEAD(&mntlist);
#ifdef CONFIG_VE
	/*
	 * The only reason of disabling readdir for the host system is that
	 * this readdir can be slow and CPU consuming with large number of VPSs
	 * (or just mount points).
	 */
	err = ve_is_super(ve);
#else
	err = 0;
#endif
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
		if (sb->s_qcop != &zfsquota_q_cops)
			continue;

		i++;
		if (i <= n)
			continue;

		l = sprintf(buf, "%08x", new_encode_dev(sb->s_dev));
		if ((*filler) (data, buf, l, i - 1,
			       zfs_aquot_getino(sb->s_dev), DT_DIR))
			break;
	}

out_fill:
	err = 0;
	file->f_pos = i;
out_err:
	zfs_aquot_releasemntlist(ve, &mntlist);
	(void)set_exec_env(old_ve);
	return err;
}

static int zfs_aquotd_looktest(struct inode *inode, void *data)
{
	return inode->i_op == &zfs_aquotq_inode_operations &&
	    zfs_aquot_getidev(inode) == (dev_t) (unsigned long)data;
}

static int zfs_aquotd_lookset(struct inode *inode, void *data)
{
	dev_t dev;

	dev = (dev_t) (unsigned long)data;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = zfs_aquot_getino(dev);
	inode->i_mode = S_IFDIR | S_IRUSR | S_IXUSR;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_nlink = 2;
	inode->i_op = &zfs_aquotq_inode_operations;
	inode->i_fop = &zfs_aquotq_file_operations;
	zfs_aquot_setidev(inode, dev);
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
#ifdef CONFIG_VE
	/*
	 * Lookup is much lighter than readdir, so it can be allowed for the
	 * host system.  But it would be strange to be able to do lookup only
	 * without readdir...
	 */
	if (ve_is_super(ve))
		goto out;
#endif

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

/*
	if (get_device_perms_ve(S_IFBLK, dev, FMODE_QUOTACTL))
		goto out;
 */

	inode = iget5_locked(dir->i_sb, zfs_aquot_getino(dev),
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
#ifdef CONFIG_VE
	/*
	 * The only reason of disabling getattr for the host system is that
	 * this getattr can be slow and CPU consuming with large number of VPSs
	 * (or just mount points).
	 */
	if (ve_is_super(ve))
		return 0;
#endif
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
			      glob_proc_vz_dir);
	glob_zfsquota_proc->proc_iops = &zfs_aquotd_inode_operations;
	glob_zfsquota_proc->proc_fops = &zfs_aquotd_file_operations;

	return 0;
}

void __exit zfsquota_proc_exit(void)
{
	remove_proc_entry("zfsquota", glob_proc_vz_dir);
}
