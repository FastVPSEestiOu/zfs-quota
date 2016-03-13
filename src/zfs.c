
#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zpl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>

struct radix_tree_root zfs_handle_data_tree;

struct zfs_handle_data {
	struct radix_tree_root user_quota_tree;
	struct radix_tree_root group_quota_tree;
};

struct quota_data {
	qid_t qid;
	uint32_t valid;
	uint64_t space_used, space_quota;
#ifdef DNODE_FLAG_USEROBJUSED_ACCOUNTED
	uint64_t obj_used, obj_quota;
#endif				/* DNODE_FLAG_USEROBJUSED_ACCOUNTED */
};

struct kmem_cache *quota_data_cachep = NULL;

int spam_zfs_quota(struct super_block *zfs_sb)
{
	zfs_useracct_t *buf;
	void *vbuf;
	int error, i;
	uint64_t bufsize = sizeof(*buf) * 10;
	uint64_t cookie = 0;

	vbuf = buf = vmem_alloc(bufsize, KM_SLEEP);
	if (buf == NULL)
		return -ENOMEM;

	error =
	    zfs_userspace_many(zfs_sb->s_fs_info, ZFS_PROP_USERUSED, &cookie,
			       buf, &bufsize);

	for (i = 0; i < bufsize / sizeof(*buf); ++i) {
		printk("domain = %s, rid = %d, space = %Lu\n", buf->zu_domain,
		       buf->zu_rid, buf->zu_space);
		buf++;
	}
	vmem_free(vbuf, sizeof(*buf) * 10);

	return 0;
}

struct zfs_handle_data *zfsquota_get_data(void *zfs_handle)
{
	struct zfs_handle_data *data = NULL;

	rcu_read_lock();
	data = radix_tree_lookup(&zfs_handle_data_tree, (long)zfs_handle);
	rcu_read_unlock();

	return data;
}

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id)
{
	int err;
	uint64_t rid = id;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USERUSED :
				ZFS_PROP_GROUPUSED, "", rid,
				&quota_data->space_used);
	if (err)
		return err;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USERQUOTA :
				ZFS_PROP_GROUPQUOTA, "", rid,
				&quota_data->space_quota);
	if (err)
		return err;

#ifdef DNODE_FLAG_USEROBJUSED_ACCOUNTED
	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USEROBJUSED :
				ZFS_PROP_GROUPOBJUSED, "", rid,
				&quota_data->obj_used);
	if (err == EOPNOTSUPP)
		goto no_obj_quota;

	if (err)
		return err;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USEROBJQUOTA :
				ZFS_PROP_GROUPOBJQUOTA, "", rid,
				&quota_data->obj_quota);
	if (err)
		return err;

no_obj_quota:
	;
#endif /* DNODE_FLAG_USEROBJUSED_ACCOUNTED */

	return 0;
}

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
		kmem_cache_free(quota_data, quota_data_cachep);
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
#ifdef DNODE_FLAG_USEROBJUSED_ACCOUNTED
	di->dqb_curinodes = quota_data->obj_used;
	di->dqb_valid |= QIF_INODES;
	if (quota_data->obj_quota) {
		di->dqb_ihardlimit = di->dqb_isoftlimit = quota_data->obj_quota;
		di->dqb_valid |= QIF_ILIMITS;
	}
#endif /* DNODE_FLAG_USEROBJUSED_ACCOUNTED */

	return 0;
}

int __init zfsquota_zfs_init(void)
{
	quota_data_cachep = kmem_cache_create("zfsquota", sizeof(quota_data), 0,
					      GFP_KERNEL, NULL);
	return 0;
}
