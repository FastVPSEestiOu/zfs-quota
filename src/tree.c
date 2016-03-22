
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mount.h>

#include "zfs.h"
#include "radix-tree-iter.h"
#include "tree.h"

#define ZQTREE_TAG_STALE	0

static DEFINE_MUTEX(zqhandle_tree_mutex);
static struct radix_tree_root zqhandle_tree;
struct kmem_cache *quota_data_cachep = NULL;

struct quota_tree {
	struct radix_tree_root radix;
	struct mutex mutex;
	uint32_t version;
};

struct zqhandle {
	struct super_block *sb;
	atomic_t refcnt;
	void *zfsh;

	struct quota_tree quota[MAXQUOTAS];
};

static inline void *get_zfsh(struct super_block *sb)
{
#ifdef CONFIG_VE
	return sb->s_op->get_quota_root(sb)->i_sb->s_fs_info;
#else /* #ifdef CONFIG_VE */
	return sb->s_root->d_inode->i_sb->s_fs_info;
#endif /* #else #ifdef CONFIG_VE */
}

int zqtree_init_superblock(struct super_block *sb)
{
	struct zqhandle *data = NULL;
	int i = 0, err;

	mutex_lock(&zqhandle_tree_mutex);
	data = radix_tree_delete(&zqhandle_tree, (unsigned long)sb);

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

	for (i = 0; i < MAXQUOTAS; ++i) {
		mutex_init(&data->quota[i].mutex);
	}

	err = radix_tree_insert(&zqhandle_tree, (unsigned long)sb, data);
	if (err)
		goto out_free;

out:
	mutex_unlock(&zqhandle_tree_mutex);
	return err;
out_free:
	kfree(data);
	goto out;
}

static int zqtree_quota_tree_destroy(struct quota_tree *quota_tree)
{
	radix_tree_iter_t iter;
	struct quota_data *qd;
	struct radix_tree_root *root;

	mutex_lock(&quota_tree->mutex);
	root = &quota_tree->radix;
	for (radix_tree_iter_start(&iter, root, 0);
	     (qd = radix_tree_iter_item(&iter));
	     radix_tree_iter_next(&iter, qd->qid)) {

		kmem_cache_free(quota_data_cachep, qd);
		radix_tree_delete(root, qd->qid);
	}
	mutex_unlock(&quota_tree->mutex);

	return 0;
}

void zqhandle_put(struct zqhandle *handle)
{
	int i;

	if (atomic_dec_and_mutex_lock(&handle->refcnt,
				      &zqhandle_tree_mutex)) {

		for (i = 0; i < MAXQUOTAS; ++i) {
			zqtree_quota_tree_destroy(&handle->quota[i]);
		}

		kfree(handle);

		mutex_unlock(&zqhandle_tree_mutex);
	}
}

int zqtree_free_superblock(struct super_block *sb)
{
	struct zqhandle *handle;

	mutex_lock(&zqhandle_tree_mutex);
	handle = radix_tree_delete(&zqhandle_tree, (unsigned long)sb);
	mutex_unlock(&zqhandle_tree_mutex);

	if (handle == NULL)
		return -ENOENT;

	zqhandle_put(handle);

	return 0;
}

static inline void zqhandle_ref(struct zqhandle *handle)
{
	atomic_inc(&handle->refcnt);
}

static inline struct zqhandle *zqhandle_get(void *sb)
{
	struct zqhandle *handle;

	/* TODO make this a RCU instead */
	mutex_lock(&zqhandle_tree_mutex);
	handle = radix_tree_lookup(&zqhandle_tree, (unsigned long)sb);

	if (handle == NULL)
		goto out;

	zqhandle_ref(handle);

out:
	mutex_unlock(&zqhandle_tree_mutex);
	return handle;
}

struct zqfs_fs_info {
	union {
		struct vfsmount *real_mnt;
		struct nameidata *nd;
	};
	char fake_dev_name[PATH_MAX];
};

int zqtree_next_mount(void *prev_sb, struct vfsmount **mnt, void **next_sb)
{
	struct zqhandle *p[1];
	struct super_block *sb;
	int ret;

	ret = radix_tree_gang_lookup(&zqhandle_tree, (void **)p, 1,
				     (unsigned long)prev_sb);
	if (!ret)
		return 0;

	sb = p[0]->sb;
	if (next_sb)
		*next_sb = sb + 1;

	if (!strcmp(sb->s_type->name, "simfs")) {
		*mnt = mntget((struct vfsmount *)sb->s_fs_info);
	} else if (!strcmp(sb->s_type->name, "zqfs")) {
		*mnt = mntget(((struct zqfs_fs_info *)sb->s_fs_info)->real_mnt);
	} else {
		*mnt = NULL;
		return 0;
	}

	return 1;
}

struct quota_tree *zqhandle_get_tree(struct zqhandle *handle, int type)
{
	if (type < 0 || type >= MAXQUOTAS)
		return NULL;

	zqhandle_ref(handle);
	return &handle->quota[type];
}

/* TODO use container_of instead, for that keep tree type in the quota_tree
 * struct */
static void zqhandle_put_tree(struct zqhandle *handle, struct quota_tree *qt)
{
	if (qt)
		zqhandle_put(handle);
}

/* must be called with quota_tree->mutex taken */
struct quota_data *zqtree_lookup_quota_data(
	struct quota_tree *quota_tree, qid_t id)
{
	int err;
	struct quota_data *quota_data;

	quota_data = radix_tree_lookup(&quota_tree->radix, id);

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

struct quota_data *zqtree_lookup_quota_type(struct zqhandle *handle, int type,
					    qid_t id)
{
	struct quota_tree *quota_tree;
	struct quota_data *quota_data;

	quota_tree = zqhandle_get_tree(handle, type);

	if (quota_tree == NULL)
		return NULL;

	mutex_lock(&quota_tree->mutex);
	quota_data = zqtree_lookup_quota_data(quota_tree, id);
	mutex_unlock(&quota_tree->mutex);

	zqhandle_put_tree(handle, quota_tree);

	return quota_data;
}

int zfsquota_fill_quotadata(void *zfsh, struct quota_data *quota_data,
			    int type, qid_t id);

struct quota_data *zqtree_get_filled_quota_data(void *sb, int type, qid_t id)
{
	struct zqhandle *handle = zqhandle_get(sb);
	struct quota_data *quota_data;

	quota_data = zqtree_lookup_quota_type(handle, type, id);

	if (zfsquota_fill_quotadata(handle->zfsh, quota_data, type,
				    id)) {
		quota_data = NULL;
	}

	zqhandle_put(handle);

	return quota_data;
}

void zqtree_print_quota_data(struct quota_data *qd)
{
	printk("qd = %p, { .qid = %u, .space_used = %Lu, .space_quota = %Lu"
#ifdef OBJECT_QUOTA
	       ", .obj_used = %Lu, .obj_quota = %Lu"
#endif /* OBJECT_QUOTA */
	       " }\n", qd, qd->qid, qd->space_used, qd->space_quota
#ifdef OBJECT_QUOTA
	       , qd->obj_used, qd->obj_quota
#endif /* OBJECT_QUOTA */
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
	struct zqhandle *handle = zqhandle_get(sb);
	struct quota_tree *quota_tree = zqhandle_get_tree(handle, type);
	int ret = -ENOENT;

	if (quota_tree)
		ret = zqtree_print_tree(quota_tree);

	zqhandle_put_tree(handle, quota_tree);
	zqhandle_put(handle);

	return ret;
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
#ifdef OBJECT_QUOTA
	di->dqb_curinodes = quota_data->obj_used;
	di->dqb_valid |= QIF_INODES;
	if (quota_data->obj_quota) {
		di->dqb_ihardlimit = di->dqb_isoftlimit = quota_data->obj_quota;
		di->dqb_valid |= QIF_ILIMITS;
	}
#endif /* OBJECT_QUOTA */

	return 0;
}

static inline uint64_t min_except_zero(uint64_t a, uint64_t b)
{
	return min(a ? a : b, b ? b : a);
}

int zqtree_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	struct zqhandle *handle = zqhandle_get(sb);
	int ret;
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

#ifdef OBJECT_QUOTA
	if (di->dqb_valid & QIF_ILIMITS) {
		limit = min_except_zero(di->dqb_ihardlimit,
					di->dqb_isoftlimit);
		ret = zfs_set_object_quota(handle->zfsh, type,
					   id, limit);
		if (ret)
			goto out;
	}
#endif /* OBJECT_QUOTA */

out:
	zqhandle_put(handle);
	return ret;
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


static int zqtree_iterate_prop(void *zfsh,
			       struct quota_tree *quota_tree,
			       zfs_prop_list_t *prop,
			       uint32_t version)
{
	zfs_prop_iter_t iter;
	zfs_prop_pair_t *pair;

	struct quota_data *qd;

	for (zfs_prop_iter_start(zfsh, prop->prop, &iter);
	     (pair = zfs_prop_iter_item(&iter)); zfs_prop_iter_next(&iter)) {

		qd = zqtree_lookup_quota_data(quota_tree, pair->rid);
		qd->version = version;
		*(uint64_t *)((void *)qd + prop->offset) = pair->value;
	}

	zfs_prop_iter_stop(&iter);

	return zfs_prop_iter_error(&iter);
}

struct quota_tree *zqtree_get_sync_quota_tree(void *sb, int type)
{
	struct zqhandle *handle = zqhandle_get(sb);
	struct quota_tree *quota_tree = zqhandle_get_tree(handle, type);
	int ret = -EINVAL;
	uint32_t version;

	zfs_prop_list_t *props, *prop;


	if (!quota_tree)
		goto out;

	rmb();
	version = quota_tree->version;

	if (!mutex_trylock(&quota_tree->mutex)) {
		/* Someone is updating the tree */
		while (version == quota_tree->version)
			cond_resched();
		ret = 0;
		goto out;
	}

	_zqtree_zfs_clear(quota_tree);

	version++;
	props = zfs_get_prop_list(type);
	for (prop = props; prop->prop >= 0; ++prop) {
		ret = zqtree_iterate_prop(handle->zfsh, quota_tree, prop,
					  version);
		if (ret)
			break;
	}

	mutex_unlock(&quota_tree->mutex);

	quota_tree->version = version;
	wmb();

out:
	if (ret) {
		zqhandle_put_tree(handle, quota_tree);
		quota_tree = NULL;
	}
	zqhandle_put(handle);
	return quota_tree;
}

void zqtree_put_quota_tree(struct quota_tree *quota_tree, int type)
{
	struct zqhandle *handle = container_of(quota_tree,
					       struct zqhandle,
					       quota[type]);

	zqhandle_put_tree(handle, quota_tree);
}

void zqtree_zfs_sync_tree(void *sb, int type)
{
	struct quota_tree *quota_tree;

	quota_tree = zqtree_get_sync_quota_tree(sb, type);
	if (quota_tree)
		zqtree_put_quota_tree(quota_tree, type);
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
