
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mount.h>
#include <linux/wait.h>

#include "handle.h"
#include "proc.h"
#include "radix-tree-iter.h"
#include "tree.h"
#include "zfs.h"

/* ZFS QUOTA radix-tree key qid -> value quota_data */

struct kmem_cache *quota_data_cachep = NULL;

struct blktree_root;

struct zqtree {
	int			type;

	struct zqhandle		*handle;

	atomic_t		refcnt;
	atomic_t		state;

	struct radix_tree_root	radix;
	struct blktree_root	*blktree_root;

};

struct zqtree *zqtree_new(struct zqhandle *handle, int type)
{
	struct zqtree *qt;
	if (type < 0 || type >= MAXQUOTAS)
		return ERR_PTR(-EINVAL);

	qt = kzalloc(sizeof(*qt), GFP_KERNEL);
	if (!qt)
		return ERR_PTR(-ENOMEM);

	qt->type = type;
	qt->handle = handle;
	atomic_set(&qt->refcnt, 1);
	atomic_set(&qt->state, ZQTREE_EMPTY);
	INIT_RADIX_TREE(&qt->radix, GFP_KERNEL);

	return qt;
}

struct zqtree *zqtree_get(struct zqtree *qt)
{
	if (qt && !atomic_inc_not_zero(&qt->refcnt))
		/* quota tree is in process of destruction */
		qt = NULL;
	return qt;
}

static int zqtree_quota_tree_destroy(struct zqtree *quota_tree);
static int blktree_free(struct blktree_root *root);

void zqtree_put(struct zqtree *qt)
{
	if (!qt)
		return;

	if (atomic_dec_and_test(&qt->refcnt)) {
		zqhandle_unref_tree(qt->handle, qt);

		blktree_free(qt->blktree_root);
		zqtree_quota_tree_destroy(qt);
		kfree(qt);
	}
}

void zqtree_unref_zqhandle(struct zqtree *qt)
{
	if (qt)
		qt->handle = NULL;
}

static DECLARE_WAIT_QUEUE_HEAD(zqtree_upgrade_wqh);

#define ERR_STATE(err, state)	((err) << 16 | (state))
#define GET_ERR(state)		((state) >> 16)

static int zqtree_build_qdtree(struct zqtree *zqtree);
static int zqtree_build_blktree(struct zqtree *zqtree);

/* This can be refactored into generic one */
int zqtree_upgrade(struct zqtree *qt, int target_state)
{
	int was_state, req_state = target_state;
	int err;

	if (target_state <= 0)
		return 0;

	/* Request an upgrade by changing state from previous
	 * value to the -requested, indicating that the build is
	 * in process */

again:
	was_state = atomic_cmpxchg(&qt->state, req_state - 1, -req_state);
	if (was_state >= target_state || -was_state > target_state) {
		/* We are in requested state or further already */
		return -GET_ERR(was_state);
	} else if (was_state < 0) {
		/* Another thread upgrades to a state <= than ours */
		req_state = -was_state;
		/* Wait for state update */
		err = wait_event_interruptible(zqtree_upgrade_wqh,
				 atomic_read(&qt->state) >= req_state);
		req_state = atomic_read(&qt->state);
		err = GET_ERR(req_state) ?: err;
		/* OK, we got to our target_state or further */
		if (err || req_state >= target_state)
			return err;
		req_state++;
	} else if (was_state < req_state - 1) {
		req_state = was_state + 1;
	} else if (was_state == req_state - 1) {
		/* We have locked it, let's update */
		err = -ENOSYS;
		switch (req_state) {
		case ZQTREE_QUOTA:
			err = zqtree_build_qdtree(qt);
			break;
		case ZQTREE_BLKTREE:
			err = zqtree_build_blktree(qt);
			break;
		}
		if (err)
			/* Failed we are, rest we must */
			atomic_cmpxchg(&qt->state, -req_state,
				       ERR_STATE(-err, req_state - 1));
		else
			atomic_cmpxchg(&qt->state, -req_state, req_state);
		wake_up_all(&zqtree_upgrade_wqh);
		if (err || req_state++ == target_state)
			return err;
	}
	goto again;
}

/* Private part */
static int zqtree_quota_tree_destroy(struct zqtree *quota_tree)
{
	my_radix_tree_iter_t iter;
	struct zqdata *qd;
	struct radix_tree_root *root;

	root = &quota_tree->radix;
	for (my_radix_tree_iter_start(&iter, root, 0);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid)) {

		kmem_cache_free(quota_data_cachep, qd);
		radix_tree_delete(root, qd->qid);
	}

	return 0;
}

static struct zqdata *zqtree_get_quota_data(struct zqtree *quota_tree,
					    qid_t id)
{
	int err;
	struct zqdata *quota_data;

	quota_data = radix_tree_lookup(&quota_tree->radix, id);

	if (quota_data == NULL) {
		quota_data = kmem_cache_zalloc(quota_data_cachep,
					       GFP_KERNEL | GFP_NOFS);

		if (!quota_data)
			return NULL;

		quota_data->qid = id;

		err = radix_tree_insert(&quota_tree->radix, id, quota_data);
		if (err == -ENOMEM) {
			kmem_cache_free(quota_data_cachep, quota_data);
			return NULL;
		}
	}

	return quota_data;
}

static int zqtree_iterate_prop(void *zfsh,
			       struct zqtree *quota_tree,
			       zfs_prop_list_t *prop)
{
	zfs_prop_iter_t iter;
	zfs_prop_pair_t *pair;

	struct zqdata *qd;

	zfs_prop_iter_start(zfsh, prop->prop, &iter);
	while ((pair = zfs_prop_iter_item(&iter))) {

		qd = zqtree_get_quota_data(quota_tree, pair->rid);
		if (!qd)
			break;
		*(uint64_t *)((void *)qd + prop->offset) = pair->value;

		zfs_prop_iter_next(&iter);
	}
	zfs_prop_iter_stop(&iter);

	return zfs_prop_iter_error(&iter);
}

static int zqtree_build_qdtree(struct zqtree *zqtree)
{
	int ret = 0;
	/* FIXME should ref handle */
	void *zfsh = zqhandle_get_zfsh(zqtree->handle);
	zfs_prop_list_t *prop;

	for (prop = zfs_get_prop_list(zqtree->type); prop->prop >= 0; ++prop) {
		ret = zqtree_iterate_prop(zfsh, zqtree, prop);
		if (ret && ret != EOPNOTSUPP)
			break;
	}

	if (ret == EOPNOTSUPP)
		ret = 0;

	return ret;
}

/**
 * Printing utililities
 */
void zqtree_print_quota_data(struct zqdata *qd)
{
	printk(KERN_DEBUG "qd = %p, "
	       "{ .qid = %u, .space_used = %Lu, .space_quota = %Lu"
#ifdef HAVE_ZFS_OBJECT_QUOTA
	       ", .obj_used = %Lu, .obj_quota = %Lu"
#endif /* HAVE_ZFS_OBJECT_QUOTA */
	       " }\n", qd, qd->qid, qd->space_used, qd->space_quota
#ifdef HAVE_ZFS_OBJECT_QUOTA
	       , qd->obj_used, qd->obj_quota
#endif /* HAVE_ZFS_OBJECT_QUOTA */
	    );
}

int zqtree_print(struct zqtree *quota_tree)
{
	my_radix_tree_iter_t iter;
	struct zqdata *qd;

	printk(KERN_DEBUG "quota_tree = %p\n", quota_tree);
	for (my_radix_tree_iter_start(&iter, &quota_tree->radix, 0);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid)) {

		zqtree_print_quota_data(qd);
	}

	return 0;
}


/****************************************************************************
 *			Block Tree of VFSv2 format			    *
 ****************************************************************************/

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

#define	DATA_BLOCK_MASK	2UL

struct blktree_data_block {
	uint32_t			blknum;
	struct	blktree_data_block	*next;
	uint32_t			qid_first;
	uint32_t			qid_last;
	uint32_t			n;
	struct zqdata			*data[DATA_PER_BLOCK];
};

struct blktree_block {
	uint32_t		blknum;
	uint32_t		is_leaf:1;
	uint32_t		offset:7;
	uint32_t		num:24;
	struct blktree_block	*next;
	union {
		struct blktree_block		*child;
		struct blktree_data_block	*data_child;
	};
};

struct blktree_root {
	struct zqtree			*zqtree;
	uint32_t			blknum;

	struct blktree_block		first_block;

	struct blktree_data_block	*first_data_block;
	struct blktree_data_block	*data_block;

	struct radix_tree_root		blocks;
};

static struct blktree_data_block *
blktree_get_datablock(struct blktree_root *tree)
{
	struct blktree_data_block *data_block = tree->data_block;

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

static struct blktree_data_block *
blktree_insert(struct blktree_root *tree, struct zqdata *qd)
{
	struct blktree_data_block *data_block;

	data_block = blktree_get_datablock(tree);
	if (!data_block)
		return NULL;

	data_block->data[data_block->n] = qd;
	if (!data_block->n)
		data_block->qid_first = qd->qid;
	data_block->qid_last = qd->qid;
	data_block->n++;

	return data_block;
}

static struct blktree_block *
blktree_new_block(struct blktree_root *root, uint32_t num)
{
	struct blktree_block *block;
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

static inline uint32_t
qid_to_prefix(qid_t qid, int level)
{
	return qid >> (8 * (QTREE_PATH - level));
}

static inline int
is_qid_in_block(struct blktree_block **path, qid_t qid, int l)
{
	return path[l] && qid_to_prefix(qid, l) == path[l]->num;
}

struct blktree_block *
blktree_get_pointer_block(struct blktree_block *block,
			  struct blktree_block **path,
			  struct blktree_root *root,
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
		block = blktree_new_block(root, qid_to_prefix(qid, i));
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

static int
blktree_enumerate_data_blocks(struct blktree_root *root)
{
	struct blktree_data_block *data_block;
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

static struct blktree_root *
blktree_build(struct zqtree *zqtree)
{
	struct blktree_block *path[QTREE_PATH] = {
		[0 ... QTREE_PATH - 1]	= 0
	};

	struct blktree_root *root;
	struct blktree_block *block = NULL;
	struct blktree_data_block *data_block;
	struct zqdata *qd;
	my_radix_tree_iter_t iter;

	root = kzalloc(sizeof(*root), GFP_KERNEL);
	if (!root)
		goto out_mem;

	root->blknum = 2;
	root->first_block.blknum = 1;
	root->zqtree = zqtree;

	INIT_RADIX_TREE(&root->blocks, GFP_KERNEL);
	radix_tree_insert(&root->blocks, 1, &root->first_block);

	for (my_radix_tree_iter_start(&iter, &zqtree->radix, 0);
	     (qd = my_radix_tree_iter_item(&iter));
	     my_radix_tree_iter_next(&iter, qd->qid))
	{
		data_block = blktree_insert(root, qd);
		if (!data_block)
			goto out_mem;
		block = blktree_get_pointer_block(block, path, root, qd->qid);
		if (!block)
			goto out_mem;
		if (!block->child) {
			block->data_child = data_block;
			block->offset = data_block->n - 1;
			block->is_leaf = 1;
		}
	}

	/* renumerate data_blocks & insert them */
	if (blktree_enumerate_data_blocks(root))
		goto out_mem;

	return root;

out_mem:
	printk(KERN_WARNING "Leaky\n");
	return NULL;
}

static int zqtree_build_blktree(struct zqtree *zqtree)
{
	struct blktree_root *blktree_root;

	blktree_root = blktree_build(zqtree);
	if (IS_ERR_OR_NULL(blktree_root))
		return PTR_ERR(blktree_root);

	zqtree->blktree_root = blktree_root;
	return 0;
}

static int
blktree_output_block_node(struct blktree_block *node, char *buf)
{
	__le32 *ref = (__le32 *) buf;

	node = node->child;
	while (node) {
		ref[node->num & 255] = cpu_to_le32(node->blknum);
		node = node->next;
	}

	return QTREE_BLOCKSIZE;
}

static int
blktree_output_block_leaf(struct blktree_block *leaf, char *buf)
{
	__le32 *ref = (__le32 *) buf;
	uint32_t first_num = leaf->num << 8, last_num = first_num + 256,
		 offset = leaf->offset, i;
	struct blktree_data_block *data_block = leaf->data_child;

	BUG_ON(!leaf->is_leaf);

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

static int
blktree_output_block_data(struct blktree_data_block *data_block, char *buf)
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

static int
blktree_output_header(struct blktree_root *root, char *buf)
{
	static const uint quota_magics[] = V2_INITQMAGICS;

	struct v2_disk_dqheader *dqh = (struct v2_disk_dqheader *)buf;
	struct v2_disk_dqinfo *dq_disk_info;

	dqh->dqh_magic = cpu_to_le32(quota_magics[root->zqtree->type]);
	dqh->dqh_version = cpu_to_le32(1);

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

static struct blktree_data_block *to_data_block_ptr(void *ptr)
{
	return (struct blktree_data_block *)to_ptr(ptr);
}

int zqtree_output_block(struct zqtree *zqtree,
			char *buf, uint32_t blknum)
{
	struct blktree_root *blktree = zqtree->blktree_root;
	struct blktree_block *node;

	if (!blktree)
		return -EIO;

	if (blknum == 0)
		return blktree_output_header(blktree, buf);

	node = radix_tree_lookup(&blktree->blocks, blknum);
	if (!node)
		return 0;

	if (is_data_block_ptr(node)) {
		/* data block */
		return blktree_output_block_data(to_data_block_ptr(node), buf);
	} else if (node->is_leaf) {
		/* tree leaf, points to data blocks */
		return blktree_output_block_leaf(node, buf);
	} else {
		/* tree node */
		return blktree_output_block_node(node, buf);
	}

	return 0;
}

static uint32_t _get_blknum(void *ptr)
{
	if (is_data_block_ptr(ptr)) {
		return to_data_block_ptr(ptr)->blknum;
	} else {
		return ((struct blktree_block *)ptr)->blknum;
	}
}

static int blktree_free(struct blktree_root *root)
{
	void *blocks[32];
	size_t n, blknum = 2, i;

	if (!root)
		return 0;

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

/****************************************************************************
 *			Common init functions				    *
 ****************************************************************************/
int __init zfsquota_tree_init(void)
{
	quota_data_cachep =
	    kmem_cache_create("zfs-quotadata", sizeof(struct zqdata), 0, 0,
			      NULL);
	return 0;
}

void __exit zfsquota_tree_exit(void)
{
	kmem_cache_destroy(quota_data_cachep);
}
