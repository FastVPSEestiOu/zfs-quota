
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "radix-tree-iter.h"
#include "tree.h"

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

static int quota_data_to_v1_disk_dqblk(struct quota_data *quota_data,
				       struct v1_disk_dqblk *v1)
{
	v1->dqb_bsoftlimit = v1->dqb_bhardlimit =
	    quota_data->space_quota / 1024;
	v1->dqb_curblocks = quota_data->space_used / 1024;
#ifdef HAVE_ZFS_OBJECT_QUOTA
	v1->dqb_ihardlimit = v1->dqb_isoftlimit = quota_data->obj_quota;
	v1->dqb_curinodes = quota_data->obj_used;
#endif /* HAVE_ZFS_OBJECT_QUOTA */
	v1->dqb_btime = v1->dqb_itime = 0;

	return sizeof(*v1);
}

#define V1_DISK_DQBLK_SIZE (sizeof(struct v1_disk_dqblk))

/*
 * FIXME: this function can handle quota files up to 2GB only.
 */
static int read_proc_quotafile(char *page, off_t off, int count,
			       struct quota_tree *root)
{
	off_t qid_start, qid_last;
	int res = 0;
	my_radix_tree_iter_t iter;
	struct quota_data *qd;
	struct v1_disk_dqblk *buf = (struct v1_disk_dqblk *)page;

	memset(page, 0, count);

	qid_start = off / V1_DISK_DQBLK_SIZE;
	qid_last = (off + count) / V1_DISK_DQBLK_SIZE;

	for (quota_tree_iter_start(&iter, root, qid_start);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid)) {

		if (qd->qid >= qid_last)
			break;

		if (!zqtree_check_qd_version(root, qd))
			continue;

		quota_data_to_v1_disk_dqblk(qd, &buf[qd->qid - qid_start]);
		res = (qd->qid - qid_start + 1) * V1_DISK_DQBLK_SIZE;
	}

	return res;
}

static int zfs_aquotf_vfsold_open(struct inode *inode, struct file *file)
{
	int err, type;
	struct super_block *sb;
	struct quota_tree *quota_tree;

	err = zqproc_get_sb_type(inode, &sb, &type);
	if (err)
		goto out_err;

	quota_tree = zqtree_get_sync_quota_tree(sb, type);
	file->private_data = quota_tree;

	return 0;

out_err:
	return err;
}

static int zfs_aquotf_vfsold_release(struct inode *inode, struct file *file)
{
	struct quota_tree *quota_tree;

	quota_tree = file->private_data;
	file->private_data = NULL;
	zqtree_put_quota_tree(quota_tree);

	return 0;
}


static ssize_t zfs_aquotf_vfsold_read(struct file *file,
				      char __user * buf, size_t size,
				      loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	struct quota_tree *quota_tree = file->private_data;
	int err;

	err = -ENOMEM;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		goto out_err;

	copied = 0;
	l = l2 = 0;
	while (1) {
		bufsize = min(size, (size_t) PAGE_SIZE);
		if (bufsize <= 0)
			break;

		l = read_proc_quotafile(page, *ppos, bufsize, quota_tree);
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
	return err;
}

struct file_operations zfs_aquotf_vfsold_file_operations = {
	.open = zfs_aquotf_vfsold_open,
	.read = &zfs_aquotf_vfsold_read,
	.release = zfs_aquotf_vfsold_release
};

int zfs_aquotq_vfsold_lookset(struct inode *inode)
{
	inode->i_fop = &zfs_aquotf_vfsold_file_operations;
	inode->i_size = 65536 * V1_DISK_DQBLK_SIZE;
	return 0;
}
