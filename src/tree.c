
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "zfs.h"
#include "radix-tree-iter.h"
#include "tree.h"

#define ZQTREE_TAG_STALE	0

struct radix_tree_root zfs_handle_data_tree;
struct kmem_cache *quota_data_cachep = NULL;

struct quota_tree {
	struct radix_tree_root radix;
	struct mutex mutex;
	uint32_t version;
};

struct zfs_handle_data {
	void *zfs_handle;

	struct quota_tree quota[MAXQUOTAS];
};

int zqtree_init_superblock(struct super_block *sb)
{
	struct zfs_handle_data *data = NULL;
	int i = 0;

	rcu_read_lock();
	data = radix_tree_delete(&zfs_handle_data_tree, (unsigned long)sb);
	rcu_read_unlock();

	if (data) {
		WARN(1, "simfs sb = %p was registered already, freeing", sb);
		/* FIXME free the trees first */
		kfree(data);
	}

	data = kzalloc(sizeof(struct zfs_handle_data), GFP_KERNEL);
	data->zfs_handle = sb->s_op->get_quota_root(sb)->i_sb->s_fs_info;
	for (i = 0; i < MAXQUOTAS; ++i) {
		mutex_init(&data->quota[i].mutex);
	}

	return radix_tree_insert(&zfs_handle_data_tree, (unsigned long)sb, data);
}

int zqtree_radix_tree_destroy(struct radix_tree_root *root);
int zqtree_free_superblock(struct super_block *sb)
{
	struct zfs_handle_data *data;
	int i = 0;

	data = radix_tree_delete(&zfs_handle_data_tree, (unsigned long)sb);

	for (i = 0; i < MAXQUOTAS; ++i)
		zqtree_radix_tree_destroy(&data->quota[i].radix);

	kfree(data);

	return 0;
}

static inline struct zfs_handle_data *zqtree_get_zfs_data(void *sb)
{
	return radix_tree_lookup(&zfs_handle_data_tree, (unsigned long)sb);
}

struct quota_tree *zqtree_get_tree_for_type(void *sb, int type)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);

	if (handle_data == NULL)
		return NULL;

	return &handle_data->quota[type];
}

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id);

struct quota_data *zqtree_lookup_quota_data(
	struct quota_tree *quota_tree, qid_t id)
{
	int err;
	struct quota_data *quota_data;

	rcu_read_lock();
	quota_data = radix_tree_lookup(&quota_tree->radix, id);
	rcu_read_unlock();

	if (quota_data == NULL) {
		quota_data =
		    kmem_cache_zalloc(quota_data_cachep, GFP_KERNEL | GFP_NOFS);

		if (!quota_data)
			return NULL;

		err = radix_tree_insert(&quota_tree->radix, id, quota_data);
		if (err == -ENOMEM) {
			kmem_cache_free(quota_data_cachep, quota_data);
			return NULL;
		}

		quota_data->qid = id;
	}

	return quota_data;
}

struct quota_data *zqtree_lookup_quota_data_sb_type(void *sb, int type,
						    qid_t id)
{
	struct quota_tree *quota_tree;

	quota_tree = zqtree_get_tree_for_type(sb, type);

	if (quota_tree == NULL)
		return NULL;

	return zqtree_lookup_quota_data(quota_tree, id);
}

struct quota_data *zqtree_get_filled_quota_data(void *sb, int type, qid_t id)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct quota_data *quota_data;

	quota_data = zqtree_lookup_quota_data_sb_type(sb, type, id);

	if (zfsquota_fill_quotadata(handle_data->zfs_handle, quota_data, type,
				    id)) {
#if 0
		kmem_cache_free(quota_data_cachep, quota_data);
		/* FIXME should do locking here */
		radix_tree_delete(quota_tree_root, id);
#endif
		return NULL;
	}

	return quota_data;
}

void zqtree_print_quota_data(struct quota_data *qd)
{
	printk("qd = %p, { .qid = %u, .space_used = %Lu, .space_quota = %Lu"
#ifdef USEROBJ_QUOTA
	       ", .obj_used = %Lu, .obj_quota = %Lu"
#endif
	       " }\n", qd, qd->qid, qd->space_used, qd->space_quota
#ifdef USEROBJ_QUOTA
	       , qd->obj_used, qd->obj_quota
#endif
	    );
}

int zqtree_print_tree(struct quota_tree *quota_tree)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;

	printk("quota_tree = %p, version = %u\n", quota_tree, quota_tree->version);
	for (radix_tree_iter_start(&iter, &quota_tree->radix, 0);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {

		zqtree_print_quota_data(qd);
	}

	return 0;
}

int zqtree_print_tree_sb_type(void *sb, int type)
{
	struct quota_tree *quota_tree = zqtree_get_tree_for_type(sb, type);

	if (!quota_tree)
		return 0;

	return zqtree_print_tree(quota_tree);
}

int zqtree_radix_tree_destroy(struct radix_tree_root *root)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;

	for (radix_tree_iter_start(&iter, root, 0);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {
		kmem_cache_free(quota_data_cachep, qd);
		radix_tree_delete(root, qd->qid);
	}

	return 0;
}

int zqtree_free_tree(void *sb, int type)
{
	struct quota_tree *quota_tree;

	quota_tree = zqtree_get_tree_for_type(sb, type);
	return zqtree_radix_tree_destroy(&quota_tree->radix);
}

int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	struct quota_data *quota_data;

	quota_data = zqtree_get_filled_quota_data(sb, type, id);

	if (!quota_data)
		return -EIO;	/* FIXME */

	/* FIXME check for endianness */
	di->dqb_curspace = quota_data->space_used;
	di->dqb_valid |= QIF_SPACE;
	if (quota_data->space_quota) {
		di->dqb_bhardlimit = di->dqb_bsoftlimit =
		    quota_data->space_quota / 1024;
		di->dqb_valid |= QIF_BLIMITS;
	}
#ifdef USEROBJ_QUOTA
	di->dqb_curinodes = quota_data->obj_used;
	di->dqb_valid |= QIF_INODES;
	if (quota_data->obj_quota) {
		di->dqb_ihardlimit = di->dqb_isoftlimit = quota_data->obj_quota;
		di->dqb_valid |= QIF_ILIMITS;
	}
#endif /* USEROBJ_QUOTA */

	return 0;
}

int zqtree_check_qd_version(struct quota_tree *quota_tree, struct quota_data *qd)
{
	if (likely(qd->version >= quota_tree->version)) {
		return 1;
	}

	radix_tree_tag_set(&quota_tree->radix, qd->qid, ZQTREE_TAG_STALE);
	return 0;
}

static int _zqtree_zfs_clear(struct quota_tree *quota_tree)
{
	struct quota_data *pqds[16];
	unsigned long curkey = 0;
	int ret, i;

	while (1) {
		ret = radix_tree_gang_lookup_tag(
			&quota_tree->radix,
			(void **)pqds, curkey, 16, ZQTREE_TAG_STALE);

		if (!ret)
			break;

		curkey = pqds[ret - 1]->qid + 1;

		for (i = 0; i < ret; ++i) {
			struct quota_data *qd = pqds[i];
			radix_tree_delete(&quota_tree->radix, qd->qid);
			kmem_cache_free(quota_data_cachep, qd);
		}
	}

	return 0;
}


int zqtree_zfs_sync_tree(void *sb, int type)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	int err, prop;
	uint32_t version;
	struct quota_tree *quota_tree;
	struct quota_data *qd;

	zfs_prop_iter_t iter;
	zfs_prop_pair_t *pair;


	quota_tree = zqtree_get_tree_for_type(sb, type);
	if (!quota_tree)
		return -ENOENT;

	version = quota_tree->version;

	if (!mutex_trylock(&quota_tree->mutex)) {
		/* Someone is updating the tree */
		while (version == quota_tree->version)
			cond_resched();
		return 0;
	}

	_zqtree_zfs_clear(quota_tree);

	rmb();
	version = quota_tree->version + 1;

#warning Magic numbers. Hate them.

	prop = type == USRQUOTA ? 0 : 2;
	for (zfs_prop_iter_start(handle_data->zfs_handle, prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree, pair->rid);
		qd->version = version;
		qd->space_used = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

	prop++;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree, pair->rid);
		qd->version = version;
		qd->space_quota = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

#ifdef USEROBJ_QUOTA
	prop += 3;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree, pair->rid);
		qd->version = version;
		qd->obj_used = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

	prop++;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree, pair->rid);
		qd->version = version;
		qd->obj_quota = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

#endif

out:
	zfs_prop_iter_stop(&iter);

	mutex_unlock(&quota_tree->mutex);

	quota_tree->version = version;
	wmb();

	err = zfs_prop_iter_error(&iter);

	return err;
}


void quota_tree_iter_start(
		radix_tree_iter_t *iter,
		struct quota_tree *root,
		unsigned long start_key)
{
	radix_tree_iter_start(iter, &root->radix, start_key);
}

int quota_tree_gang_lookup(struct quota_tree *quota_tree,
			   struct quota_data **pqd,
			   unsigned long start_key,
			   unsigned int max_items)
{
	return radix_tree_gang_lookup(
		&quota_tree->radix,
		(void**)pqd,
		start_key,
		max_items);
}

int __init zfsquota_tree_init(void)
{
	quota_data_cachep =
	    kmem_cache_create("zfsquota", sizeof(struct quota_data), 0, 0,
			      NULL);
	return 0;
}

void __exit zfsquota_tree_exit(void)
{
	kmem_cache_destroy(quota_data_cachep);
}
