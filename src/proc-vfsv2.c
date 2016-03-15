
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>

#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "proc.h"
#include "tree.h"
#include "radix-tree-iter.h"

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
	    cpu_to_le64(quota_data->space_quota);
	v2r1->dqb_curspace = cpu_to_le64(quota_data->space_used);
#ifdef USEROBJ_QUOTA
	v2r1->dqb_ihardlimit = v2r1->dqb_isoftlimit =
	    cpu_to_le64(quota_data->obj_quota);
	v2r1->dqb_curinodes = cpu_to_le64(quota_data->obj_used);
#endif
	v2r1->dqb_btime = v2r1->dqb_itime = cpu_to_le64(0);

	return sizeof(*v2r1);
}

#define V2R1_DISK_DQBLK_SIZE (sizeof(struct v2r1_disk_dqblk))

/* TODO (Pavel Boldin) basically this code converts qid_t keyed radix tree
 * into blknum-keyed radix tree. A better data structure that keeps both
 * trees simultaneously should be used instead. This structure code is to be
 * copied from the radix tree with bits per level = 8 (???)
 */
struct qtree_tree_block;

#warning introduce qtree_tree_root and keep a list of free data-blocks there\
         as well as list quota_tree_root and allocated blocks count \
         and optionally radix_tree or simple list of blocks

/* FIXME this qtree is rather space-consuming since it allocates new 
 * data block for each leaf
 *
 * To fix this introduce make depth == 3 blocks reference
 * data blocks indirectly by number. (union qtree_leaf's qd with blknum)
 */
struct qtree_tree_block {
	uint32_t blknum;
	uint32_t depth, num;

	struct qtree_tree_block *parent;
	struct list_head siblings;
	struct list_head child;	/* if depth < 4, lists siblings
				 * depth == 4, lists leafs */
};

struct qtree_tree_root {
	struct radix_tree_root *quota_tree_root;
	uint32_t blocks;
	uint32_t data_per_block;
	struct qtree_tree_block root_block;

	struct radix_tree_root blocks_tree;
};

struct qtree_leaf {
	struct list_head siblings;
	struct quota_data *qd;
};

#define QTREE_DEPTH     4

struct qtree_tree_block *new_qtree_tree_block(struct qtree_tree_root *root,
					      uint32_t num,
					      struct qtree_tree_block *parent)
{
	struct qtree_tree_block *node;

	node = kzalloc(sizeof(*node), GFP_NOFS);
	if (!node)
		return NULL;

	node->blknum = ++root->blocks;
	node->num = num;
	node->depth = parent ? parent->depth + 1 : 0;
	node->parent = parent;
	INIT_LIST_HEAD(&node->child);
	INIT_LIST_HEAD(&node->siblings);
	if (parent)
		list_add_tail(&node->siblings, &parent->child);

	radix_tree_insert(&root->blocks_tree, node->blknum, node);

	//printk("blknum = %lu, depth = %lu, num = %lu\n", node->blknum, node->depth, num);

	return node;
}

void fill_leafs(struct qtree_tree_block *tree_node,
		struct qtree_tree_root *tree_root)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;
	struct qtree_tree_block *data_block = NULL;

	for (radix_tree_iter_start
	     (&iter, tree_root->quota_tree_root, tree_node->num);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {
		struct qtree_leaf *leaf;
		if (qd->qid >= tree_node->num + 256)
			break;

		if (!data_block
		    || data_block->num + 1 > tree_root->data_per_block)
			data_block =
			    new_qtree_tree_block(tree_root, 0, tree_node);

		data_block->num++;

		leaf = kzalloc(sizeof(*leaf), GFP_NOFS);
		leaf->qd = qd;
		INIT_LIST_HEAD(&leaf->siblings);
		list_add_tail(&leaf->siblings, &data_block->child);
	}
}

void fill_childs(struct qtree_tree_block *node,
		 struct qtree_tree_root *tree_root)
{
	unsigned long stop_key, shift = 8 * (QTREE_DEPTH - node->depth);
	unsigned long cur_key, child_shift =
	    8 * (QTREE_DEPTH - node->depth - 1);
	unsigned long dchild = 1UL << child_shift;
	int r;
	struct quota_data *qd;

	if (node->depth == QTREE_DEPTH - 1) {
		fill_leafs(node, tree_root);
		return;
	}

	cur_key = node->num;
	stop_key = cur_key + (1UL << shift) - 1;

	for (; cur_key < stop_key;) {
		struct qtree_tree_block *child;

		r = radix_tree_gang_lookup(tree_root->quota_tree_root,
					   (void **)&qd, cur_key, 1);
		if (!r || qd->qid >= stop_key)
			break;

		child =
		    new_qtree_tree_block(tree_root, qd->qid & ~(dchild - 1),
					 node);

		fill_childs(child, tree_root);

		cur_key = (qd->qid + dchild) & ~(dchild - 1);
	}
}

void print_tree(struct qtree_tree_block *node)
{
	printk("blknum = %d, parentblknum = %d\n", node->blknum,
	       node->parent ? node->parent->blknum : 0);
	if (node->depth < QTREE_DEPTH) {
		struct qtree_tree_block *child;
		list_for_each_entry(child, &node->child, siblings) {
			print_tree(child);
		}
	} else {
		struct qtree_leaf *leaf;
		list_for_each_entry(leaf, &node->child, siblings) {
			printk("leaf = %p, qd = %p, qid = %u\n", leaf, leaf->qd,
			       leaf->qd->qid);
		}
	}
}

int output_block(char *buf, uint32_t blknum, struct qtree_tree_root *tree_root)
{
	struct qtree_tree_block *node, *child;

	node = radix_tree_lookup(&tree_root->blocks_tree, blknum);
	if (!node)
		return -ENOENT;

	memset(buf, 0, 1024);
	//printk("outblk = %d\n", blknum);
	if (node->depth < QTREE_DEPTH - 1) {
		__le32 *ref = (__le32 *) buf;
		list_for_each_entry(child, &node->child, siblings) {
			uint32_t n =
			    (child->num >> 8 *
			     (QTREE_DEPTH - child->depth)) & 255;
			ref[n] = cpu_to_le32(child->blknum);
			//       printk("ref[%d] = %d\n", refnum, child->blknum);
		}
	} else if (node->depth == QTREE_DEPTH - 1) {
		/* tree leaf, points to data blocks */
		__le32 *ref = (__le32 *) buf;

		struct qtree_tree_block *data_block;
		list_for_each_entry(data_block, &node->child, siblings) {
			struct qtree_leaf *leaf;

			list_for_each_entry(leaf, &data_block->child, siblings) {
				if (node->num <= leaf->qd->qid &&
				    leaf->qd->qid <= node->num + 255) {

					ref[leaf->qd->qid % 255] =
					    cpu_to_le32(data_block->blknum);
#if 0
					printk("ref[%d] = %d\n",
					       leaf->qd->qid & 255,
					       data_block->blknum);
#endif
				}
			}
		}
	} else if (node->depth == QTREE_DEPTH) {
		/* data block */
		struct qt_disk_dqdbheader *dh =
		    (struct qt_disk_dqdbheader *)buf;
		struct v2r1_disk_dqblk *db =
		    (struct v2r1_disk_dqblk *)(buf + sizeof(*dh));
		struct qtree_leaf *leaf;
		int i = 0;

		dh->dqdh_entries = node->num;

		list_for_each_entry(leaf, &node->child, siblings) {
			quota_data_to_v2r1_disk_dqblk(leaf->qd, &db[i++]);
		}
	}

	return 0;
}

struct qtree_tree_root *build_qtree(struct radix_tree_root *quota_tree_root)
{
	struct qtree_tree_root *root;
	int blocks = 1;		//, i;

	root = kzalloc(sizeof(*root), GFP_NOFS);
	if (!root)
		return NULL;

	root->blocks = 1;
	root->quota_tree_root = quota_tree_root;
	root->data_per_block = 4;

	memset(&root->root_block, 0, sizeof(root->root_block));
	root->root_block.blknum = blocks;
	INIT_LIST_HEAD(&root->root_block.child);
	INIT_LIST_HEAD(&root->root_block.siblings);

	radix_tree_insert(&root->blocks_tree, 1, &root->root_block);
	fill_childs(&root->root_block, root);

	print_tree(&root->root_block);
#if 0
	for (i = 1; i < root->blocks; ++i) {
		output_block(i, root);
	}
#endif

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
