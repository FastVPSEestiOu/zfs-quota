
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

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id);

struct quota_data *zqtree_lookup_quota_data(void *sb, int type,
					    qid_t id, int *new)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct quota_data *quota_data;
	struct radix_tree_root *quota_tree_root;

	if (new)
		*new = 0;

	if (handle_data == NULL)
		return NULL;

	quota_tree_root = type == USRQUOTA ? &handle_data->user_quota_tree
	    : &handle_data->group_quota_tree;

	rcu_read_lock();
	quota_data = radix_tree_lookup(quota_tree_root, id);
	rcu_read_unlock();

	if (quota_data == NULL) {
		quota_data = kmem_cache_zalloc(quota_data_cachep, GFP_KERNEL);
		if (new)
			*new = 1;
		radix_tree_insert(quota_tree_root, id, quota_data);
	}

	return quota_data;
}

struct quota_data *zqtree_get_quota_data(void *sb, int type, qid_t id,
					 int update)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct quota_data *quota_data;
	int new;

	quota_data = zqtree_lookup_quota_data(sb, type, id, &new);

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

int zqtree_print_tree(void *sb, int type)
{
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct radix_tree_root *quota_tree_root;
	radix_tree_iter_t iter;
	struct quota_data *qd;

	if (handle_data == NULL)
		return 0;

	quota_tree_root = type == USRQUOTA ? &handle_data->user_quota_tree
	    : &handle_data->group_quota_tree;

	for (radix_tree_iter_start(&iter, quota_tree_root, 0);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {

		printk("qd = %p, qd->qid = %Lu, qd->space_used = %Lu\n",
		       qd, qd->qid, qd->space_used);
	}

	return 0;
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
	struct zfs_handle_data *handle_data = zqtree_get_zfs_data(sb);
	struct radix_tree_root *quota_tree_root;

	if (handle_data == NULL)
		return 0;

	quota_tree_root = type == USRQUOTA ? &handle_data->user_quota_tree
	    : &handle_data->group_quota_tree;

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
}
