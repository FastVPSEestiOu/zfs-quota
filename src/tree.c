
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mount.h>

#include "zfs.h"
#include "radix-tree-iter.h"
#include "proc.h"
#include "tree.h"

static DEFINE_MUTEX(zqhandle_tree_mutex);
static RADIX_TREE(zqhandle_tree, GFP_KERNEL);
struct kmem_cache *quota_data_cachep = NULL;

struct zqtree {
	atomic_t refcnt;
	struct radix_tree_root radix;
};

struct zqhandle {
	struct super_block *sb;
	atomic_t refcnt;
	void *zfsh;

	spinlock_t lock;
	struct zqtree *quota[MAXQUOTAS];
};

static inline void *get_zfsh(struct super_block *sb)
{
#ifdef CONFIG_VE
	return sb->s_op->get_quota_root(sb)->i_sb->s_fs_info;
#else /* #ifdef CONFIG_VE */
	return sb->s_root->d_inode->i_sb->s_fs_info;
#endif /* #else #ifdef CONFIG_VE */
}

int zqhandle_register_superblock(struct super_block *sb)
{
	struct zqhandle *data = NULL;
	int err;

	mutex_lock(&zqhandle_tree_mutex);
	data = radix_tree_delete(&zqhandle_tree, (unsigned long)sb);
	mutex_unlock(&zqhandle_tree_mutex);

	if (data) {
		WARN(1, "simfs sb = %p was registered already, freeing", sb);
		/* FIXME free the trees first */
		kfree(data);
	}

	err = -ENOMEM;
	data = kzalloc(sizeof(struct zqhandle), GFP_KERNEL);
	if (data == NULL)
		goto out;

	data->sb = sb;
	data->zfsh = get_zfsh(sb);
	atomic_set(&data->refcnt, 1);

	mutex_lock(&zqhandle_tree_mutex);
	err = radix_tree_insert(&zqhandle_tree, (unsigned long)sb, data);
	mutex_unlock(&zqhandle_tree_mutex);
	if (err)
		goto out_free;

	zqproc_register_handle(sb);
out:
	return err;
out_free:
	kfree(data);
	goto out;
}

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

void zqhandle_put(struct zqhandle *handle)
{
	if (atomic_dec_and_test(&handle->refcnt)) {
		kfree(handle);
	}
}

int zqhandle_unregister_superblock(struct super_block *sb)
{
	struct zqhandle *handle;
	int err = -ENOENT;

	mutex_lock(&zqhandle_tree_mutex);
	handle = radix_tree_delete(&zqhandle_tree, (unsigned long)sb);

	zqproc_unregister_handle(sb);

	if (handle == NULL)
		goto out;

	err = 0;
	zqhandle_put(handle);

out:
	mutex_unlock(&zqhandle_tree_mutex);
	return 0;
}

static inline void zqhandle_ref(struct zqhandle *handle)
{
	atomic_inc(&handle->refcnt);
}

static inline struct zqhandle *zqhandle_get(void *sb)
{
	struct zqhandle *handle;

	mutex_lock(&zqhandle_tree_mutex);
	handle = radix_tree_lookup(&zqhandle_tree, (unsigned long)sb);

	if (handle == NULL)
		goto out;

	zqhandle_ref(handle);

out:
	mutex_unlock(&zqhandle_tree_mutex);
	return handle;
}

struct zqtree *zqhandle_get_tree(struct zqhandle *handle, int type)
{
	struct zqtree *quota_tree;
	if (type < 0 || type >= MAXQUOTAS)
		return NULL;

	spin_lock(&handle->lock);
again:
	quota_tree = handle->quota[type];
	spin_unlock(&handle->lock);

	if (!quota_tree) {
		quota_tree = kzalloc(sizeof(*quota_tree), GFP_KERNEL);
		if (!quota_tree)
			return NULL;

		atomic_set(&quota_tree->refcnt, 0);
		spin_lock(&handle->lock);
		if (handle->quota[type]) {
			kfree(quota_tree);
			goto again;
		}
		handle->quota[type] = quota_tree;
		spin_unlock(&handle->lock);
	}

	atomic_inc(&quota_tree->refcnt);

	return quota_tree;
}

static void zqhandle_put_tree(struct zqtree *qt)
{
	if (!qt)
		return;

	if (atomic_dec_and_test(&qt->refcnt)) {
		zqtree_quota_tree_destroy(qt);
		kfree(qt);
	}
}

struct zqdata *zqtree_get_quota_data(
	struct zqtree *quota_tree, qid_t id)
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

int zfsquota_fill_quotadata(void *zfsh, struct zqdata *quota_data,
			    int type, qid_t id);
int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	int err = -EIO;
	struct zqdata quota_data;
	struct zqhandle *handle = zqhandle_get(sb);

	if (!handle)
		goto out;

	if (zfsquota_fill_quotadata(handle->zfsh, &quota_data, type, id))
		goto out_zqhandle_put;

	di->dqb_curspace = quota_data.space_used;
	di->dqb_valid |= QIF_SPACE;
	if (quota_data.space_quota) {
		di->dqb_bhardlimit = di->dqb_bsoftlimit =
		    quota_data.space_quota / 1024;
		di->dqb_valid |= QIF_BLIMITS;
	}

#ifdef HAVE_ZFS_OBJECT_QUOTA
	di->dqb_curinodes = quota_data.obj_used;
	di->dqb_valid |= QIF_INODES;
	if (quota_data.obj_quota) {
		di->dqb_ihardlimit = di->dqb_isoftlimit = quota_data.obj_quota;
		di->dqb_valid |= QIF_ILIMITS;
	}
#endif /* HAVE_ZFS_OBJECT_QUOTA */

	err = 0;
out_zqhandle_put:
	zqhandle_put(handle);
out:
	return err;
}

static inline uint64_t min_except_zero(uint64_t a, uint64_t b)
{
	return min(a ?: b, b ?: a);
}

int zqtree_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	struct zqhandle *handle = zqhandle_get(sb);
	int ret = 0;
	uint64_t limit;

	if (!handle)
		return -ENOENT;

	if (di->dqb_valid & QIF_BLIMITS) {
		limit = 1024 * min_except_zero(di->dqb_bhardlimit,
					       di->dqb_bsoftlimit);
		ret = zfs_set_space_quota(handle->zfsh, type,
					  id, limit);
		if (ret)
			goto out;
	}

#ifdef HAVE_ZFS_OBJECT_QUOTA
	if (di->dqb_valid & QIF_ILIMITS) {
		limit = min_except_zero(di->dqb_ihardlimit,
					di->dqb_isoftlimit);
		ret = zfs_set_object_quota(handle->zfsh, type,
					   id, limit);
		if (ret)
			goto out;
	}
#endif /* HAVE_ZFS_OBJECT_QUOTA */

out:
	zqhandle_put(handle);
	return ret;
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
		*(uint64_t *)((void *)qd + prop->offset) = pair->value;

		zfs_prop_iter_next(&iter);
	}
	zfs_prop_iter_stop(&iter);

	return zfs_prop_iter_error(&iter);
}

struct zqtree *zqtree_get_sync_quota_tree(void *sb, int type)
{
	struct zqhandle *handle = zqhandle_get(sb);
	struct zqtree *quota_tree;
	int ret = 0;

	zfs_prop_list_t *prop;

	quota_tree = zqhandle_get_tree(handle, type);
	/* The tree is being used already, don't update it */
	if (atomic_read(&quota_tree->refcnt) > 2)
		return quota_tree;

	for (prop = zfs_get_prop_list(type); prop->prop >= 0; ++prop) {
		ret = zqtree_iterate_prop(handle->zfsh, quota_tree, prop);
		if (ret && ret != EOPNOTSUPP)
			break;
	}

	if (ret && ret != EOPNOTSUPP) {
		zqhandle_put_tree(quota_tree);
		quota_tree = NULL;
	}
	zqhandle_put(handle);
	return quota_tree;
}

void quota_tree_iter_start(
		my_radix_tree_iter_t *iter,
		struct zqtree *root,
		unsigned long start_key)
{
	my_radix_tree_iter_start(iter, &root->radix, start_key);
}

int quota_tree_gang_lookup(struct zqtree *quota_tree,
			   struct zqdata **pqd,
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
	    kmem_cache_create("zfs-quotadata", sizeof(struct zqdata), 0, 0,
			      NULL);
	return 0;
}

void __exit zfsquota_tree_exit(void)
{
	kmem_cache_destroy(quota_data_cachep);
}
