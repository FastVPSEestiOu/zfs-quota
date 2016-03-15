
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "tree.h"
#include "radix-tree-iter.h"

struct v2r1_disk_dqblk {
	__le32 dqb_id;		/* id this quota applies to */
	__le32 dqb_pad;
	__le64 dqb_ihardlimit;	/* absolute limit on allocated inodes */
	__le64 dqb_isoftlimit;	/* preferred inode limit */
	__le64 dqb_curinodes;	/* current # allocated inodes */
	__le64 dqb_bhardlimit;	/* absolute limit on disk space (in QUOTABLOCK_SIZE) */
	__le64 dqb_bsoftlimit;	/* preferred limit on disk space (in QUOTABLOCK_SIZE) */
	__le64 dqb_curspace;	/* current space occupied (in bytes) */
	__le64 dqb_btime;	/* time limit for excessive disk use */
	__le64 dqb_itime;	/* time limit for excessive inode use */
};

static int quota_data_to_v2r1_disk_dqblk(struct quota_data *quota_data,
					 struct v2r1_disk_dqblk *v2r1)
{
	v2r1->dqb_id = quota_data->qid;
	v2r1->dqb_bsoftlimit = v2r1->dqb_bhardlimit = quota_data->space_quota;
	v2r1->dqb_curspace = quota_data->space_used;
#ifdef USEROBJ_QUOTA
	v2r1->dqb_ihardlimit = v2r1->dqb_isoftlimit = quota_data->obj_quota;
	v2r1->dqb_curinodes = quota_data->obj_used;
#endif
	v2r1->dqb_btime = v2r1->dqb_itime = 0;

	return sizeof(*v2r1);
}

#define V2R1_DISK_DQBLK_SIZE (sizeof(struct v2r1_disk_dqblk))

/*
 * FIXME: this function can handle quota files up to 2GB only.
 */
static int read_proc_quotafile(char *page, off_t off, int count,
			       struct radix_tree_root *quota_tree_root)
{
	off_t qid_start, qid_last;
	int res = 0;
	radix_tree_iter_t iter;
	struct quota_data *qd;
	struct v1_disk_dqblk *buf = (struct v1_disk_dqblk *)page;

	memset(page, 0, count);

	qid_start = off / V2R1_DISK_DQBLK_SIZE;
	qid_last = (off + count) / V2R1_DISK_DQBLK_SIZE;

	for (radix_tree_iter_start(&iter, quota_tree_root, qid_start);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {

		if (qd->qid >= qid_last)
			break;

		//quota_data_to_v1_disk_dqblk(qd, &buf[qd->qid - qid_start]);
		//res = (qd->qid - qid_start + 1) * V1_DISK_DQBLK_SIZE;
	}

	return res;
}

/* TODO (Pavel Boldin) basically this code converts qid_t keyed radix tree
 * into blknum-keyed radix tree. A better data structure that keeps both
 * trees simultaneously should be used instead. This structure code is to be
 * copied from the radix tree with bits per level = 8 (???)
 */
struct qtree_tree_block;

#warning introduce qtree_tree_root and keep a list of free data-blocks there\
         as well as list quota_tree_root and allocated blocks count \
         and optionally radix_tree or simple list of blocks

struct qtree_tree_block {
	uint32_t blknum;
	uint32_t depth, num;

	struct qtree_tree_block *parent;
	struct list_head child;	/* if depth < 4, lists siblings
				 * depth == 4, lists leafs */
	struct list_head siblings;
};

struct qtree_leaf {
	struct list_head siblings;
	struct quota_data *qd;
};

#define QTREE_DEPTH     4

struct qtree_tree_block *new_qtree_tree_block(int *blocks,
					      uint32_t num,
					      struct qtree_tree_block *parent)
{
	struct qtree_tree_block *node;

	node = kzalloc(sizeof(*node), GFP_NOFS);
	if (!node)
		return NULL;

	node->blknum = (*blocks)++;
	node->num = num;
	node->depth = parent->depth + 1;
	node->parent = parent;
	INIT_LIST_HEAD(&node->child);
	INIT_LIST_HEAD(&node->siblings);
	list_add_tail(&node->siblings, &parent->child);

	printk("blknum = %lu, depth = %lu, num = %lu\n", node->blknum,
	       node->depth, num);

	return node;
}

void fill_leafs(struct qtree_tree_block *tree_node,
		struct radix_tree_root *quota_tree_root, int *blocks)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;
	struct qtree_tree_block *data_block = NULL;

	for (radix_tree_iter_start(&iter, quota_tree_root, tree_node->num);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {
		struct qtree_leaf *leaf;
		if (qd->qid >= tree_node->num + 256)
			break;

		if (!data_block || data_block->num > 3)
			data_block = new_qtree_tree_block(blocks, 0, tree_node);

		data_block->num++;

		leaf = kzalloc(sizeof(*leaf), GFP_NOFS);
		leaf->qd = qd;
		INIT_LIST_HEAD(&leaf->siblings);
		list_add_tail(&leaf->siblings, &data_block->child);
	}
}

void fill_childs(struct qtree_tree_block *node,
		 struct radix_tree_root *quota_tree_root, int *blocks)
{
	unsigned long stop_key, shift = 8 * (QTREE_DEPTH - node->depth);
	unsigned long cur_key, child_shift =
	    8 * (QTREE_DEPTH - node->depth - 1);
	unsigned long dchild = 1UL << child_shift;
	int r;
	struct quota_data *qd;

	if (node->depth == QTREE_DEPTH - 1) {
		fill_leafs(node, quota_tree_root, blocks);
		return;
	}

	cur_key = node->num;
	stop_key = cur_key + (1UL << shift) - 1;

	for (; cur_key < stop_key;) {
		struct qtree_tree_block *child;

		r = radix_tree_gang_lookup(quota_tree_root, (void **)&qd,
					   cur_key, 1);
		if (!r)
			break;

		child =
		    new_qtree_tree_block(blocks, qd->qid & ~(dchild - 1), node);

		fill_childs(child, quota_tree_root, blocks);

		cur_key = (qd->qid + dchild) & ~(dchild - 1);
		printk("num = %u, cur_key = %lu\n", child->num, cur_key);
	}
}

struct qtree_tree_block *build_qtree(struct radix_tree_root *quota_tree_root)
{
	struct qtree_tree_block *root;
	int blocks = 1;

	root = kzalloc(sizeof(*root), GFP_NOFS);
	if (!root)
		return NULL;

	root->blknum = blocks;
	INIT_LIST_HEAD(&root->child);
	INIT_LIST_HEAD(&root->siblings);

	fill_childs(root, quota_tree_root, &blocks);

	return root;
}

static ssize_t zfs_aquotf_vfsv2r1_read(struct file *file,
				       char __user * buf, size_t size,
				       loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	struct inode *inode;
	struct block_device *bdev;
	struct super_block *sb;
	struct radix_tree_root *quota_tree_root;
	struct qtree_block *root;
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

	quota_tree_root = zqtree_get_tree_for_type(sb, type);

	copied = 0;
	l = l2 = 0;
	while (1) {
		bufsize = min(size, (size_t) PAGE_SIZE);
		if (bufsize <= 0)
			break;

		l = read_proc_quotafile(page, *ppos, bufsize, quota_tree_root);
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

static struct file_operations zfs_aquotf_vfsv2r1_file_operations = {
	.read = &zfs_aquotf_vfsv2r1_read,
};

int zfs_aquotq_vfsv2r1_lookset(struct inode *inode)
{
	inode->i_fop = &zfs_aquotf_vfsv2r1_file_operations;
	return 0;
}
