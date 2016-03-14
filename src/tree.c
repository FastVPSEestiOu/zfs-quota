
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>

#include "zfs.h"
#include "radix-tree-iter.h"
#include "tree.h"

struct radix_tree_root zfs_handle_data_tree;
struct kmem_cache *quota_data_cachep = NULL;

struct zfs_handle_data {
	void *zfs_handle;
	struct radix_tree_root user_quota_tree;
	struct radix_tree_root group_quota_tree;
};

int zqtree_init_superblock(struct super_block *sb)
{
	struct zfs_handle_data *data = NULL;

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

	radix_tree_insert(&zfs_handle_data_tree, (unsigned long)sb, data);

	return 0;
}

int zqtree_radix_tree_destroy(struct radix_tree_root *root);
int zqtree_free_superblock(struct super_block *sb)
{
	struct zfs_handle_data *data;

	data = radix_tree_delete(&zfs_handle_data_tree, (unsigned long)sb);

	zqtree_radix_tree_destroy(&data->user_quota_tree);
	zqtree_radix_tree_destroy(&data->group_quota_tree);

	kfree(data);

	return 0;
}

static inline struct zfs_handle_data *zqtree_get_zfs_data(void *sb)
{
	return radix_tree_lookup(&zfs_handle_data_tree, (unsigned long)sb);
}

struct radix_tree_root *zqtree_get_tree_for_type(void *sb, int type)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);

	if (handle_data == NULL)
		return NULL;

	return type == USRQUOTA ? &handle_data->user_quota_tree
	    : &handle_data->group_quota_tree;
}

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id);

struct quota_data *zqtree_lookup_quota_data(struct radix_tree_root
					    *quota_tree_root, qid_t id,
					    int *new)
{
	struct quota_data *quota_data;

	if (new)
		*new = 0;

	rcu_read_lock();
	quota_data = radix_tree_lookup(quota_tree_root, id);
	rcu_read_unlock();

	if (quota_data == NULL) {
		quota_data =
		    kmem_cache_zalloc(quota_data_cachep, GFP_KERNEL | GFP_NOFS);
		if (new)
			*new = 1;
		radix_tree_insert(quota_tree_root, id, quota_data);
		quota_data->qid = id;
	}

	return quota_data;
}

struct quota_data *zqtree_lookup_quota_data_sb_type(void *sb, int type,
						    qid_t id, int *new)
{
	struct radix_tree_root *quota_tree_root;

	quota_tree_root = zqtree_get_tree_for_type(sb, type);

	if (quota_tree_root == NULL)
		return NULL;

	return zqtree_lookup_quota_data(quota_tree_root, id, new);
}

struct quota_data *zqtree_get_quota_data(void *sb, int type, qid_t id,
					 int update)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct quota_data *quota_data;
	int new;

	quota_data = zqtree_lookup_quota_data_sb_type(sb, type, id, &new);

	if ((new || update) &&
	    zfsquota_fill_quotadata(handle_data->zfs_handle, quota_data, type,
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

int zqtree_print_tree(struct radix_tree_root *quota_tree_root)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;

	for (radix_tree_iter_start(&iter, quota_tree_root, 0);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {

		printk
		    ("qd = %p, { .qid = %u, .space_used = %Lu, .space_quota = %Lu"
#ifdef USEROBJ_QUOTA
		     ", .obj_used = %Lu, .obj_quota = %Lu"
#endif
		     " }\n", qd, qd->qid, qd->space_used, qd->space_quota
#ifdef USEROBJ_QUOTA
		     , qd->obj_used, qd->obj_quota
#endif
		    );
	}

	return 0;
}

int zqtree_print_tree_sb_type(void *sb, int type)
{
	struct radix_tree_root *quota_tree_root =
	    zqtree_get_tree_for_type(sb, type);
	if (!quota_tree_root)
		return 0;

	return zqtree_print_tree(quota_tree_root);
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
	struct radix_tree_root *quota_tree_root;

	quota_tree_root = zqtree_get_tree_for_type(sb, type);
	return zqtree_radix_tree_destroy(quota_tree_root);
}

int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	struct quota_data *quota_data;

	quota_data = zqtree_get_quota_data(sb, type, id, 0);

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

int zqtree_zfs_sync_tree(void *sb, int type)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	int err, prop;
	struct radix_tree_root *quota_tree_root;
	struct quota_data *qd;

	zfs_prop_iter_t iter;
	zfs_prop_pair_t *pair;

	quota_tree_root = zqtree_get_tree_for_type(sb, type);

	if (!quota_tree_root)
		return -ENOENT;

#warning Magic numbers. Hate them.

	prop = type == USRQUOTA ? 0 : 2;
	for (zfs_prop_iter_start(handle_data->zfs_handle, prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree_root, pair->rid, NULL);
		qd->space_used = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

	prop++;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree_root, pair->rid, NULL);
		qd->space_quota = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

#ifdef USEROBJ_QUOTA
	prop += 3;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree_root, pair->rid, NULL);
		qd->obj_used = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

	prop++;

	for (zfs_prop_iter_reset(prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree_root, pair->rid, NULL);
		qd->obj_quota = pair->value;
	}

	if (zfs_prop_iter_error(&iter))
		goto out;

#endif

out:
	zfs_prop_iter_stop(&iter);

	err = zfs_prop_iter_error(&iter);

	if (!err) {
		printk("tree = %d\n", type);
		zqtree_print_tree_sb_type(sb, type);
	}

	return err;
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
