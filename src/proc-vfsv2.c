
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>
#include <linux/slab.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "radix-tree-iter.h"
#include "tree.h"

#define QTREE_BLOCKSIZE     1024

/* First generic header */
struct v2_disk_dqheader {
	__le32 dqh_magic;	/* Magic number identifying file */
	__le32 dqh_version;	/* File version */
};

/* Header with type and version specific information */
struct v2_disk_dqinfo {
	__le32 dqi_bgrace;	/* Time before block soft limit becomes hard limit */
	__le32 dqi_igrace;	/* Time before inode soft limit becomes hard limit */
	__le32 dqi_flags;	/* Flags for quotafile (DQF_*) */
	__le32 dqi_blocks;	/* Number of blocks in file */
	__le32 dqi_free_blk;	/* Number of first free block in the list */
	__le32 dqi_free_entry;	/* Number of block with at least one free entry */
};

struct qt_disk_dqdbheader {
	__le32 dqdh_next_free;	/* Number of next block with free entry */
	__le32 dqdh_prev_free;	/* Number of previous block with free entry */
	__le16 dqdh_entries;	/* Number of valid entries in block */
	__le16 dqdh_pad1;
	__le32 dqdh_pad2;
};

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
	v2r1->dqb_id = cpu_to_le32(quota_data->qid);
	v2r1->dqb_bsoftlimit = v2r1->dqb_bhardlimit =
	    cpu_to_le64(quota_data->space_quota / 1024);
	v2r1->dqb_curspace = cpu_to_le64(quota_data->space_used);
#ifdef HAVE_OBJECT_QUOTA
	v2r1->dqb_ihardlimit = v2r1->dqb_isoftlimit =
	    cpu_to_le64(quota_data->obj_quota);
	v2r1->dqb_curinodes = cpu_to_le64(quota_data->obj_used);
#endif /* HAVE_OBJECT_QUOTA */
	v2r1->dqb_btime = v2r1->dqb_itime = cpu_to_le64(0);

	return sizeof(*v2r1);
}


/* #define QTREE_DEBUG */

struct qtree_block;

struct qtree_block {
	uint32_t blknum;
	uint32_t depth, num;

	struct qtree_block *parent;
	struct list_head siblings;
	struct list_head child;	/* if depth < 4, lists siblings
				 * depth == 4, lists leafs */
};

struct qtree_root {
	struct quota_tree *quota_tree;
	uint32_t blocks;
	uint32_t data_per_block;
	int type;
	struct qtree_block root_block;

	struct radix_tree_root blocks_tree;
};

struct qtree_leaf {
	struct list_head siblings;
	struct quota_data *qd;
};

#define QTREE_DEPTH     4

struct qtree_block *qtree_new_block(struct qtree_root *root,
				    uint32_t num,
				    struct qtree_block *parent)
{
	struct qtree_block *block;

	block = kzalloc(sizeof(*block), GFP_NOFS);
	if (!block)
		return NULL;

	block->blknum = ++root->blocks;
	block->num = num;
	block->depth = parent ? parent->depth + 1 : 0;
	block->parent = parent;
	INIT_LIST_HEAD(&block->child);
	INIT_LIST_HEAD(&block->siblings);
	if (parent)
		list_add_tail(&block->siblings, &parent->child);

	radix_tree_insert(&root->blocks_tree, block->blknum, block);

#ifdef QTREE_DEBUG
	printk(KERN_DEBUG "new blknum = %lu, depth = %lu, num = %lu\n",
		block->blknum, block->depth, num);
#endif

	return block;
}

void fill_leafs(struct qtree_block *node_block,
		struct qtree_root *tree_root)
{
	my_radix_tree_iter_t iter;
	struct quota_data *qd;
	struct qtree_block *data_block = NULL;

	for (quota_tree_iter_start
	     (&iter, tree_root->quota_tree, node_block->num);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid)) {
		struct qtree_leaf *leaf;
		if (qd->qid >= node_block->num + 256)
			break;

		if (!data_block
		    || data_block->num + 1 > tree_root->data_per_block)
			data_block = qtree_new_block(tree_root, 0, node_block);

		data_block->num++;

		leaf = kzalloc(sizeof(*leaf), GFP_NOFS);
		leaf->qd = qd;
		INIT_LIST_HEAD(&leaf->siblings);
		list_add_tail(&leaf->siblings, &data_block->child);
	}
}

void fill_childs(struct qtree_block *node_block,
		 struct qtree_root *tree_root)
{
	unsigned long stop_key, shift = 8 * (QTREE_DEPTH - node_block->depth);
	unsigned long cur_key;
	unsigned long child_shift = 8 * (QTREE_DEPTH - node_block->depth - 1);
	unsigned long dchild = 1UL << child_shift;
	int r;
	struct quota_data *qd;
	struct quota_tree *root = tree_root->quota_tree;

	if (node_block->depth == QTREE_DEPTH - 1) {
		fill_leafs(node_block, tree_root);
		return;
	}

	cur_key = node_block->num;
	stop_key = cur_key + (1UL << shift) - 1;

	for (; cur_key < stop_key;) {
		struct qtree_block *child;
		qid_t qid;

		r = quota_tree_gang_lookup(root, &qd, cur_key, 1);
		if (!r || qd->qid >= stop_key)
			break;

		qid = qd->qid;

		if (!zqtree_check_qd_version(root, qd)) {
			cur_key = qid + 1;
			continue;
		}

		child = qtree_new_block(tree_root, qid & ~(dchild - 1),
					node_block);

		fill_childs(child, tree_root);

		cur_key = (qid + dchild) & ~(dchild - 1);
	}
}

void qtree_print(struct qtree_block *node)
{
	printk("blknum = %d, parentblknum = %d\n", node->blknum,
	       node->parent ? node->parent->blknum : 0);
	if (node->depth < QTREE_DEPTH) {
		struct qtree_block *child;
		list_for_each_entry(child, &node->child, siblings) {
			qtree_print(child);
		}
	} else {
		struct qtree_leaf *leaf;
		list_for_each_entry(leaf, &node->child, siblings) {
			printk("leaf = %p, qd = %p, qid = %u\n",
			       leaf, leaf->qd, leaf->qd->qid);
		}
	}
}

static int qtree_output_block_node(char *buf, struct qtree_block *node)
{
	struct qtree_block *child;
	uint32_t shift = 8 * (QTREE_DEPTH - node->depth - 1);

	__le32 *ref = (__le32 *) buf;
	list_for_each_entry(child, &node->child, siblings) {
		uint32_t refnum = (child->num >> shift) & 255;
		ref[refnum] = cpu_to_le32(child->blknum);
#ifdef QTREE_DEBUG
		printk(KERN_DEBUG "ref[%d] = %d\n", refnum, child->blknum);
#endif /* QTREE_DEBUG */
	}

	return QTREE_BLOCKSIZE;
}

static int qtree_output_block_leaf(char *buf, struct qtree_block *leaf_block)
{
	__le32 *ref = (__le32 *) buf;
	struct qtree_block *data_block;

	list_for_each_entry(data_block, &leaf_block->child, siblings) {
		struct qtree_leaf *leaf;

		list_for_each_entry(leaf, &data_block->child, siblings) {
			if (leaf_block->num > leaf->qd->qid
			    || leaf->qd->qid > leaf_block->num + 255)
				/* Skip the leaf  */
				continue;

			ref[leaf->qd->qid & 255] =
			    cpu_to_le32(data_block->blknum);
#ifdef QTREE_DEBUG
			printk(KERN_DEBUG "ref[%d] = %d\n",
			       leaf->qd->qid & 255, data_block->blknum);
#endif
		}
	}

	return QTREE_BLOCKSIZE;
}

static int qtree_output_block_data(char *buf, struct qtree_block *data_block)
{
	struct qt_disk_dqdbheader *dh =
	    (struct qt_disk_dqdbheader *)buf;
	struct v2r1_disk_dqblk *db =
	    (struct v2r1_disk_dqblk *)(buf + sizeof(*dh));
	struct qtree_leaf *leaf;

	dh->dqdh_entries = data_block->num;

	list_for_each_entry(leaf, &data_block->child, siblings) {
		quota_data_to_v2r1_disk_dqblk(leaf->qd, db);
		db++;
	}

	return QTREE_BLOCKSIZE;
}

#define V2_INITQMAGICS {\
	0xd9c01f11,     /* USRQUOTA */\
	0xd9c01927      /* GRPQUOTA */\
}

static int qtree_output_header(char *buf, struct qtree_root *root)
{
	static const uint quota_magics[] = V2_INITQMAGICS;

	struct v2_disk_dqheader *dqh = (struct v2_disk_dqheader *)buf;
	struct v2_disk_dqinfo *dq_disk_info;

	dqh->dqh_magic = quota_magics[root->type];
	dqh->dqh_version = 1;

	dq_disk_info =
	    (struct v2_disk_dqinfo *)(buf +
				      sizeof(struct v2_disk_dqheader));
	dq_disk_info->dqi_blocks = root->blocks + 1;

	return 1024;
}

int qtree_output_block(char *buf, uint32_t blknum,
		       struct qtree_root *tree_root)
{
	struct qtree_block *node;

	if (blknum == 0)
		return qtree_output_header(buf, tree_root);

	node = radix_tree_lookup(&tree_root->blocks_tree, blknum);
	if (!node)
		return 0;

#ifdef QTREE_DEBUG
	printk(KERN_DEBUG "outblk = %d\n", blknum);
#endif
	if (node->depth < QTREE_DEPTH - 1) {
		/* tree node */
		return qtree_output_block_node(buf, node);
	} else if (node->depth == QTREE_DEPTH - 1) {
		/* tree leaf, points to data blocks */
		return qtree_output_block_leaf(buf, node);
	} else if (node->depth == QTREE_DEPTH) {
		/* data block */
		return qtree_output_block_data(buf, node);
	}

	return 0;
}

struct qtree_root *qtree_build(int type, struct quota_tree *quota_tree)
{
	struct qtree_root *root;
	int blocks = 1;		//, i;

	root = kzalloc(sizeof(*root), GFP_NOFS);
	if (!root)
		return NULL;

	root->blocks = 1;
	root->quota_tree = quota_tree;
	root->data_per_block =
		(QTREE_BLOCKSIZE - sizeof(struct qt_disk_dqdbheader))
		 / sizeof(struct v2r1_disk_dqblk);
	root->type = type;

	memset(&root->root_block, 0, sizeof(root->root_block));
	root->root_block.blknum = blocks;
	INIT_LIST_HEAD(&root->root_block.child);
	INIT_LIST_HEAD(&root->root_block.siblings);

	radix_tree_insert(&root->blocks_tree, 1, &root->root_block);
	fill_childs(&root->root_block, root);

#ifdef QTREE_DEBUG
	qtree_print(&root->root_block);
#endif

	return root;
}

static int qtree_free_block(struct qtree_block *node)
{
	struct qtree_leaf *leaf, *tmp;

	if (node->depth == QTREE_DEPTH) {
		/* Free leafs for the data block */
		list_for_each_entry_safe(leaf, tmp, &node->child, siblings) {
			list_del(&leaf->siblings);
			kfree(leaf);
		}
	}

	kfree(node);

	return 0;
}

static int qtree_free(struct qtree_root *root)
{
	int i;
	struct qtree_block *node;
	if (!root)
		return 0;

	/* Tree root block is root->root_block, skip it */
	for (i = 2; i < root->blocks; ++i) {
		node = radix_tree_delete(&root->blocks_tree, i);
		qtree_free_block(node);
	}

	kfree(root);

	return 0;
}

static int zfs_aquotf_vfsv2r1_open(struct inode *inode, struct file *file)
{
	int err, type;
	struct super_block *sb;
	struct quota_tree *quota_tree;
	struct qtree_root *root;

	err = zqproc_get_sb_type(inode, &sb, &type);
	if (err)
		goto out_err;

	quota_tree = zqtree_get_sync_quota_tree(sb, type);
	root = qtree_build(type, quota_tree);
	zqtree_put_quota_tree(quota_tree, type);

	file->private_data = root;

	return 0;

out_err:
	return err;
}

static int zfs_aquotf_vfsv2r1_release(struct inode *inode, struct file *file)
{
	struct qtree_root *root;

	root = file->private_data;
	file->private_data = NULL;

	return qtree_free(root);
}

static ssize_t read_proc_quotafile(char *page, off_t blknum,
				   struct qtree_root *root)
{
	memset(page, 0, QTREE_BLOCKSIZE);
	return qtree_output_block(page, blknum, root);
}

static ssize_t zfs_aquotf_vfsv2r1_read(struct file *file,
				       char __user * buf, size_t size,
				       loff_t * ppos)
{
	char *page;
	size_t bufsize;
	ssize_t l, l2, copied;
	int err;
	struct qtree_root *root = file->private_data;

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
