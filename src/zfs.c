
#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zfs_vfsops.h>

#include "zfs.h"
#include "tree.h"

int zfsquota_fill_quotadata(void *zfs_handle, struct quota_data *quota_data,
			    int type, qid_t id)
{
	int err;
	uint64_t rid = id;

	quota_data->qid = id;

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

#ifdef OBJECT_QUOTA
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
#endif /* OBJECT_QUOTA */

	return 0;
}

#define ZFS_PROP_ITER_BUFSIZE (sizeof(zfs_useracct_t) * 128)

static int zfs_prop_iter_next_call(zfs_prop_iter_t * iter)
{
	iter->retsize = iter->bufsize;
	iter->offset = 0;

	iter->error = zfs_userspace_many(iter->zfs_handle,
					 (zfs_userquota_prop_t) iter->prop,
					 &iter->cookie, iter->buf,
					 &iter->retsize);
	return iter->error;
}

void zfs_prop_iter_stop(zfs_prop_iter_t * iter)
{
	if (iter->buf)
		vmem_free(iter->buf, iter->bufsize);
	iter->buf = NULL;
}

void zfs_prop_iter_start(void *zfs_handle, int prop, zfs_prop_iter_t * iter)
{
	iter->zfs_handle = zfs_handle;
	iter->prop = prop;

	iter->bufsize = ZFS_PROP_ITER_BUFSIZE;
	iter->buf = vmem_alloc(iter->bufsize, KM_SLEEP);
	if (!iter->buf) {
		iter->error = ENOMEM;
		return;
	}
	iter->cookie = 0;
	iter->error = 0;

	zfs_prop_iter_next_call(iter);
}

void zfs_prop_iter_reset(int prop, zfs_prop_iter_t * iter)
{
	iter->prop = prop;
	iter->cookie = 0;

	zfs_prop_iter_next_call(iter);
}

zfs_prop_pair_t *zfs_prop_iter_item(zfs_prop_iter_t * iter)
{
	zfs_useracct_t *buf;

	if (iter->error || iter->retsize == 0)
		return NULL;

	buf = (zfs_useracct_t *) ((uintptr_t) iter->buf + iter->offset);

	iter->pair.rid = buf->zu_rid;
	iter->pair.value = buf->zu_space;

	return &iter->pair;
}

void zfs_prop_iter_next(zfs_prop_iter_t * iter)
{
	iter->offset += sizeof(zfs_useracct_t);

	if (iter->offset >= iter->retsize) {
		/* End of the last buffer */
		if (iter->retsize < iter->bufsize)
			iter->retsize = 0;
		else
			zfs_prop_iter_next_call(iter);
	}
}

int zfs_prop_iter_error(zfs_prop_iter_t * iter)
{
	return iter->error;
}
