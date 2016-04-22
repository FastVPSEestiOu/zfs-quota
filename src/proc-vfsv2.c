
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/radix-tree.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "radix-tree-iter.h"
#include "tree.h"

static int zfs_aquotf_vfsv2r1_open(struct inode *inode, struct file *file)
{
	int err, type;
	struct super_block *sb;
	struct zblktree *quota_tree;
	struct blktree_root *root;

	err = zqproc_get_sb_type(inode, &sb, &type);
	if (err)
		goto out_err;

	quota_tree = zblktree_get_sync_quota_tree(sb, type);
	root = blktree_build(quota_tree, type);

	file->private_data = root;

	return 0;

out_err:
	return err;
}
static int zfs_aquotf_vfsv2r1_release(struct inode *inode, struct file *file)
{
	struct blktree_root *root;

	root = file->private_data;
	file->private_data = NULL;

	return blktree_free(root);
}

static ssize_t read_proc_quotafile(char *page, off_t blknum,
				   struct blktree_root *root)
{
	memset(page, 0, QTREE_BLOCKSIZE);
	return blktree_output_block(page, blknum, root);
}

static ssize_t zfs_aquotf_vfsv2r1_read(struct file *file,
				       char __user * buf, size_t size,
				       loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	int err;
	struct blktree_root *root = file->private_data;

	err = -ENOMEM;
	page = (char *)__get_free_page(GFP_KERNEL);
	if (!page)
		goto out_err;

	copied = 0;
	l = l2 = 0;
	while (1) {
		bufsize = min(size, (size_t) QTREE_BLOCKSIZE);
		if (bufsize <= 0)
			break;

		l = read_proc_quotafile(page, *ppos / QTREE_BLOCKSIZE, root);
		if (l <= 0)
			break;
		l = bufsize;

		l2 = copy_to_user(buf, page + (*ppos & (QTREE_BLOCKSIZE - 1)),
				  l);
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

struct file_operations zfs_aquotf_vfsv2r1_file_operations = {
	.open = &zfs_aquotf_vfsv2r1_open,
	.read = &zfs_aquotf_vfsv2r1_read,
	.release = &zfs_aquotf_vfsv2r1_release,
};

int zfs_aquotq_vfsv2r1_lookset(struct inode *inode)
{
	inode->i_fop = &zfs_aquotf_vfsv2r1_file_operations;
	return 0;
}
