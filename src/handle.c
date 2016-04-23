
#include <linux/fs.h>
#include <linux/radix-tree.h>
#include <linux/quota.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mount.h>

#include "radix-tree-iter.h"
#include "proc.h"
#include "tree.h"
#include "zfs.h"

/**
 * Z(FS)Q(UOTA) part. All the handles are stored into radix-tree zqhandle_tree
 * with access protected by zqhandle_tree_mutex. This is due to the way simfs
 * frees superblocks on unmount.
 */

static DEFINE_MUTEX(zqhandle_tree_mutex);
static RADIX_TREE(zqhandle_tree, GFP_KERNEL);

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

void zqhandle_put(struct zqhandle *handle)
{
	if (atomic_dec_and_test(&handle->refcnt)) {
		int i;
		spin_lock(&handle->lock);
		for (i = 0; i < MAXQUOTAS; i++) {
			zqtree_unref_zqhandle(handle->quota[i]);
			handle->quota[i] = NULL;
		}
		spin_unlock(&handle->lock);
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

struct zqhandle *zqhandle_get_zfsh(struct zqhandle *handle)
{
	return handle->zfsh;
}

struct zqhandle *zqhandle_get(void *sb)
{
	struct zqhandle *handle;

	mutex_lock(&zqhandle_tree_mutex);
	handle = radix_tree_lookup(&zqhandle_tree, (unsigned long)sb);

	if (handle == NULL)
		goto out;

	atomic_inc(&handle->refcnt);

out:
	mutex_unlock(&zqhandle_tree_mutex);
	return handle;
}

/* Note: zqhandle don't reference the zqtree unless type has flag
 * ZQTREE_TYPE_FROM_SYNC.
 */
struct zqtree *zqhandle_get_tree(struct zqhandle *handle, int type,
				 int required_state)
{
	int err;
	struct zqtree *quota_tree;

again:
	spin_lock(&handle->lock);
	quota_tree = zqtree_get(handle->quota[type]);
	spin_unlock(&handle->lock);

	if (!quota_tree) {
		quota_tree = zqtree_new(handle, type);
		if (IS_ERR(quota_tree))
			goto out;

		spin_lock(&handle->lock);
		if (handle->quota[type]) {
			spin_unlock(&handle->lock);
			kfree(quota_tree);
			goto again;
		}
		handle->quota[type] = quota_tree;
		spin_unlock(&handle->lock);
	}

	err = zqtree_upgrade(quota_tree, required_state);
	if (err) {
		zqtree_put(quota_tree);
		quota_tree = ERR_PTR(err);
	}

out:
	return quota_tree;
}

void zqhandle_unref_tree(struct zqhandle *handle, struct zqtree *zqtree)
{
	int i = 0;

	if (!handle)
		return;

	spin_lock(&handle->lock);
	for (i = 0; i < MAXQUOTAS; i++) {
		if (handle->quota[i] != zqtree)
			continue;
		handle->quota[i] = NULL;
		break;
	}
	spin_unlock(&handle->lock);
}

/* ZQ handle get/set quota */
int zqhandle_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
{
	int err = -EIO;
	struct zqdata quota_data;
	struct zqhandle *handle = zqhandle_get(sb);

	if (!handle)
		goto out;

	if (zfs_fill_quotadata(handle->zfsh, &quota_data, type, id))
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

int zqhandle_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di)
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
