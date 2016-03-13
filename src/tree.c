
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>

#include "tree.h"

struct radix_tree_root zfs_handle_data_tree;
struct kmem_cache *quota_data_cachep = NULL;

struct zfs_handle_data {
	struct radix_tree_root user_quota_tree;
	struct radix_tree_root group_quota_tree;
};

struct zfs_handle_data *zfsquota_get_data(void *zfs_handle)
{
	struct zfs_handle_data *data = NULL;

	rcu_read_lock();
	data = radix_tree_lookup(&zfs_handle_data_tree, (long)zfs_handle);
	rcu_read_unlock();

	if (data == NULL) {
		data = kzalloc(sizeof(struct zfs_handle_data), GFP_KERNEL);
		radix_tree_insert(&zfs_handle_data_tree, (long)zfs_handle,
				  data);
	}

	return data;
}

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id);

struct quota_data *zfsquota_get_quotadata(void *zfs_handle, int type, qid_t id,
					  int update)
{
	struct zfs_handle_data *handle_data = zfsquota_get_data(zfs_handle);
	struct quota_data *quota_data;
	struct radix_tree_root *quota_tree_root;

	if (handle_data == NULL)
		return NULL;

	quota_tree_root = type == USRQUOTA ? &handle_data->user_quota_tree
	    : &handle_data->group_quota_tree;

	rcu_read_lock();
	quota_data = radix_tree_lookup(quota_tree_root, id);
	rcu_read_unlock();

	if (quota_data && !update)
		return quota_data;

	if (quota_data == NULL) {
		quota_data = kmem_cache_zalloc(quota_data_cachep, GFP_KERNEL);
		update = 0;
	}

	if (zfsquota_fill_quotadata(zfs_handle, quota_data, type, id)) {
		kmem_cache_free(quota_data_cachep, quota_data);
		/* FIXME should do locking here */
		radix_tree_delete(quota_tree_root, id);
		return NULL;
	}

	return quota_data;
}

int zfsquota_get_quota_dqblk(void *zfs_handle, int type, qid_t id,
			     struct if_dqblk *di)
{
	struct quota_data *quota_data;

	quota_data = zfsquota_get_quotadata(zfs_handle, type, id, 0);

	if (!quota_data)
		return -EIO;	/* FIXME */

	/* FIXME check for endianness */
	di->dqb_curspace = quota_data->space_used;
	di->dqb_valid |= QIF_SPACE;
	if (quota_data->space_quota) {
		di->dqb_bhardlimit = di->dqb_bsoftlimit =
		    quota_data->space_quota;
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
	WARN(1, "Forgot to destroy the tree!");
}
