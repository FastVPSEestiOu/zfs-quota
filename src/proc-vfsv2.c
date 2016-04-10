
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

static int quota_data_to_v2r1_disk_dqblk(struct zqdata *quota_data,
					 struct v2r1_disk_dqblk *v2r1)
{
	v2r1->dqb_id = cpu_to_le32(quota_data->qid);
	v2r1->dqb_bsoftlimit = v2r1->dqb_bhardlimit =
	    cpu_to_le64(quota_data->space_quota / 1024);
	v2r1->dqb_curspace = cpu_to_le64(quota_data->space_used);
#ifdef HAVE_ZFS_OBJECT_QUOTA
	v2r1->dqb_ihardlimit = v2r1->dqb_isoftlimit =
	    cpu_to_le64(quota_data->obj_quota);
	v2r1->dqb_curinodes = cpu_to_le64(quota_data->obj_used);
#endif /* HAVE_ZFS_OBJECT_QUOTA */
	v2r1->dqb_btime = v2r1->dqb_itime = cpu_to_le64(0);

	return sizeof(*v2r1);
}


#define	DATA_PER_BLOCK	\
	((QTREE_BLOCKSIZE - sizeof(struct qt_disk_dqdbheader)) / \
	 sizeof(struct v2r1_disk_dqblk))

#define QTREE_DEPTH     4

struct qtree_block {
	uint32_t		blknum;
	uint32_t		is_leaf:1;
	uint32_t		offset:7;
	uint32_t		num:24;
	struct qtree_block	*next;
	struct qtree_block	*child;
};

#define	DATA_BLOCK_MASK	2UL

struct qtree_data_block {
	uint32_t			blknum;
	struct	qtree_data_block	*next;
	uint32_t			qid_first;
	uint32_t			qid_last;
	uint32_t			n;
	struct zqdata			*data[DATA_PER_BLOCK];
};

struct qtree_root {
	struct zqtree*		zqtree;
	uint32_t		type;
	uint32_t		blknum;

	struct qtree_block	first_block;

	struct qtree_data_block *first_data_block;
	struct qtree_data_block	*data_block;

	struct radix_tree_root	blocks;
};

struct qtree_data_block *qtree_get_datablock(struct qtree_root *tree)
{
	struct qtree_data_block *data_block = tree->data_block;

	if (!data_block || data_block->n == DATA_PER_BLOCK) {
		data_block = kzalloc(sizeof(*data_block), GFP_KERNEL);
		if (!data_block)
			return NULL;

		if (tree->data_block) {
			tree->data_block->next = data_block;
			tree->data_block = data_block;
		} else {
			tree->first_data_block = tree->data_block = data_block;
		}
	}

	return data_block;
}

struct qtree_data_block *qtree_insert(struct qtree_root *tree, struct zqdata *qd)
{
	struct qtree_data_block *data_block;

	data_block = qtree_get_datablock(tree);
	if (!data_block)
		return NULL;

	data_block->data[data_block->n] = qd;
	if (!data_block->n)
		data_block->qid_first = qd->qid;
	data_block->qid_last = qd->qid;
	data_block->n++;

	return data_block;
}

struct qtree_block *qtree_new_block(struct qtree_root *root, uint32_t num)
{
	struct qtree_block *block;
	int err;

	block = kzalloc(sizeof(*block), GFP_KERNEL);
	if (!block)
		return NULL;


	block->num = num;
	block->blknum  = root->blknum++;

	if (!root->first_block.child)
		root->first_block.child = block;

	err = radix_tree_insert(&root->blocks, block->blknum, block);
	if (err)
		goto out_err;

	return block;
out_err:
	kfree(block);
	return NULL;
}

#define	QTREE_PATH	(QTREE_DEPTH - 1)

static inline uint32_t qid_to_prefix(qid_t qid, int level)
{
	return qid >> (8 * (QTREE_PATH - level));
}

static inline int is_qid_in_block(struct qtree_block **path, qid_t qid,
			      int l)
{
	return path[l] && qid_to_prefix(qid, l) == path[l]->num;
}

struct qtree_block *qtree_get_pointer_block(struct qtree_block *block,
		struct qtree_block **path, struct qtree_root *root,
		qid_t qid)
{
	int i;

	if (block && qid_to_prefix(qid, QTREE_PATH - 1) == block->num)
		return block;

	/* Go up tree until block can contain qid */
	for (i = QTREE_PATH - 1; i >= 0 && !is_qid_in_block(path, qid, i);
	     i--);

	/* Now allocate new blocks */
	for (i++; i <= QTREE_PATH - 1; i++) {
		block = qtree_new_block(root, qid_to_prefix(qid, i));
		if (i > 0 && !path[i - 1]->child)
			path[i - 1]->child = block;
		if (path[i])
			path[i]->next = block;
		path[i] = block;
		if (i < QTREE_PATH - 1)
			path[i + 1] = NULL;
	}

	return block;
}

int qtree_enumerate_data_blocks(struct qtree_root *root)
{
	struct qtree_data_block *data_block;
	int err = 0;
	for (data_block = root->first_data_block;
	     data_block; data_block = data_block->next)
	{
		data_block->blknum = root->blknum++;
		err = radix_tree_insert(&root->blocks, data_block->blknum,
					(void *)(DATA_BLOCK_MASK |
						 (unsigned long)data_block));
		if (err)
			break;
	}

	return err;
}

struct qtree_root *qtree_build(struct zqtree *zqtree, uint32_t type)
{
	struct qtree_block *path[QTREE_PATH] = {
		[0 ... QTREE_PATH - 1]	= 0
	};

	struct qtree_root *root;
	struct qtree_block *block = NULL;
	struct qtree_data_block *data_block;
	struct zqdata *qd;
	my_radix_tree_iter_t iter;

	root = kzalloc(sizeof(*root), GFP_KERNEL);
	if (!root)
		goto out_mem;

	root->blknum = 2;
	root->type = type;
	root->first_block.blknum = 1;
	root->zqtree = zqtree;

	INIT_RADIX_TREE(&root->blocks, GFP_KERNEL);
	radix_tree_insert(&root->blocks, 1, &root->first_block);

	for (quota_tree_iter_start(&iter, zqtree, 0);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid))
	{
		data_block = qtree_insert(root, qd);
		if (!data_block)
			goto out_mem;
		block = qtree_get_pointer_block(block, path, root, qd->qid);
		if (!block)
			goto out_mem;
		if (!block->child) {
			block->child = (struct qtree_block *)data_block;
			block->offset = data_block->n - 1;
			block->is_leaf = 1;
		}
	}

	/* renumerate data_blocks & insert them */
	if (qtree_enumerate_data_blocks(root))
		goto out_mem;

	return root;

out_mem:
	printk(KERN_WARNING "Leaky\n");
	return NULL;
}

static int qtree_output_block_node(char *buf, struct qtree_block *node)
{
	__le32 *ref = (__le32 *) buf;

	node = node->child;
	while (node) {
		ref[node->num & 255] = cpu_to_le32(node->blknum);
		node = node->next;
	}

	return QTREE_BLOCKSIZE;
}

static int qtree_output_block_leaf(char *buf, struct qtree_block *leaf)
{
	__le32 *ref = (__le32 *) buf;
	uint32_t first_num = leaf->num << 8, last_num = first_num + 256,
		 offset = leaf->offset, i;
	struct qtree_data_block *data_block;
	data_block = (struct qtree_data_block *)leaf->child;

	while (data_block && data_block->qid_first < last_num) {
		for (i = offset; i < data_block->n; i++) {
			struct zqdata *qd = data_block->data[i];
			if (last_num <= qd->qid)
				break;

			ref[qd->qid & 255] = cpu_to_le32(data_block->blknum);
		}
		if (i != data_block->n)
			break;
		data_block = data_block->next;
		offset = 0;
	}

	return QTREE_BLOCKSIZE;
}

static int qtree_output_block_data(char *buf, struct qtree_data_block *data_block)
{
	struct qt_disk_dqdbheader *dh =
	    (struct qt_disk_dqdbheader *)buf;
	struct v2r1_disk_dqblk *db =
	    (struct v2r1_disk_dqblk *)(buf + sizeof(*dh));
	int i;

	dh->dqdh_entries = data_block->n;


	for (i = 0; i < data_block->n; i++, db++) {
		quota_data_to_v2r1_disk_dqblk(data_block->data[i], db);
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
	dq_disk_info->dqi_blocks = root->blknum;

	return QTREE_BLOCKSIZE;
}

static int is_data_block_ptr(void *ptr)
{
	return DATA_BLOCK_MASK & (unsigned long)ptr;
}

static void *to_ptr(void *ptr)
{
	return (void *)(~DATA_BLOCK_MASK & (unsigned long)ptr);
}

static struct qtree_data_block *to_data_block_ptr(void *ptr)
{
	return (struct qtree_data_block *)to_ptr(ptr);
}

int qtree_output_block(char *buf, uint32_t blknum,
		       struct qtree_root *tree_root)
{
	struct qtree_block *node;

	if (blknum == 0)
		return qtree_output_header(buf, tree_root);

	node = radix_tree_lookup(&tree_root->blocks, blknum);
	if (!node)
		return 0;

	if (is_data_block_ptr(node)) {
		/* data block */
		return qtree_output_block_data(buf, to_data_block_ptr(node));
	} else if (node->is_leaf) {
		/* tree leaf, points to data blocks */
		return qtree_output_block_leaf(buf, node);
	} else {
		/* tree node */
		return qtree_output_block_node(buf, node);
	}

	return 0;
}

static int zfs_aquotf_vfsv2r1_open(struct inode *inode, struct file *file)
{
	int err, type;
	struct super_block *sb;
	struct zqtree *quota_tree;
	struct qtree_root *root;

	err = zqproc_get_sb_type(inode, &sb, &type);
	if (err)
		goto out_err;

	quota_tree = zqtree_get_sync_quota_tree(sb, type);
	root = qtree_build(quota_tree, type);

	file->private_data = root;

	return 0;

out_err:
	return err;
}

static uint32_t _get_blknum(void *ptr)
{
	if (is_data_block_ptr(ptr)) {
		return to_data_block_ptr(ptr)->blknum;
	} else {
		return ((struct qtree_block *)ptr)->blknum;
	}
}

static int qtree_free(struct qtree_root *root)
{
	void *blocks[32];
	size_t n, blknum = 2, i;

	zqtree_put(root->zqtree);

	while (true) {
		n = radix_tree_gang_lookup(&root->blocks, blocks, blknum,
					ARRAY_SIZE(blocks));

		if (!n)
			break;

		for (i = 0; i < n; i++) {
			blknum = _get_blknum(blocks[i]);
			radix_tree_delete(&root->blocks, blknum);
			kfree(to_ptr(blocks[i]));
		}
	}
	kfree(root);
	return 0;
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
