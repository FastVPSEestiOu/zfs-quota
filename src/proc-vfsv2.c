
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/radix-tree.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "handle.h"
#include "tree.h"

#define QTREE_BLOCKSIZE	1024

static int zfs_aquotf_vfsv2r1_open(struct inode *inode, struct file *file)
{
	int err, type;
	struct super_block *sb;
	struct zqtree *quota_tree;
	struct zqhandle *handle;

	err = zqproc_get_sb_type(inode, &sb, &type);
	if (err)
		goto out_err;

	err = -ENOENT;
	handle = zqhandle_get_by_sb(sb);
	if (!handle)
		goto out_err;

	quota_tree = zqhandle_get_tree(handle, type);
	zqhandle_put(handle);

	if (IS_ERR(quota_tree)) {
		err = PTR_ERR(quota_tree);
		goto out_err;
	}
	file->private_data = quota_tree;

	return 0;

out_err:
	return err;
}

static int zfs_aquotf_vfsv2r1_release(struct inode *inode, struct file *file)
{
	struct zqtree *zqtree;

	zqtree = file->private_data;
	file->private_data = NULL;

	zqtree_put(zqtree);

	return 0;
}

static ssize_t zfs_aquotf_vfsv2r1_read_magic(struct zqtree *zqtree,
					     char __user *buf)
{
	char magic[8];
	ssize_t ret;

	ret = zqtree_output_magic(zqtree, magic);
	if (ret < 0)
		return -EIO;

	if (copy_to_user(buf, magic, ret))
		return -EFAULT;

	return ret;
}

static ssize_t zfs_aquotf_vfsv2r1_read(struct file *file,
				       char __user * buf, size_t size,
				       loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	int err;
	struct zqtree *zqtree = file->private_data;

	if (*ppos == 0 && size == 8) {
		return zfs_aquotf_vfsv2r1_read_magic(zqtree, buf);
	}

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

		/* TODO fix double memory set for non-zero regions */
		memset(page, 0, QTREE_BLOCKSIZE);
		l = zqtree_output_block(zqtree, page, *ppos / QTREE_BLOCKSIZE);
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

const struct file_operations zfs_aquotf_vfsv2r1_file_operations = {
	.open = &zfs_aquotf_vfsv2r1_open,
	.read = &zfs_aquotf_vfsv2r1_read,
	.release = &zfs_aquotf_vfsv2r1_release,
};

int zfs_aquotq_vfsv2r1_lookset(struct inode *inode)
{
	inode->i_fop = &zfs_aquotf_vfsv2r1_file_operations;
	return 0;
}
