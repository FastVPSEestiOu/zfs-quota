
#include <linux/stddef.h>

#include <spl_config.h>
#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zfs_vfsops.h>

#include "tree.h"
#include "zfs.h"

int zfs_fill_quotadata(void *zfs_handle, struct zqdata *quota_data,
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

#ifdef HAVE_ZFS_OBJECT_QUOTA
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
#endif /* HAVE_ZFS_OBJECT_QUOTA */

	return 0;
}

#define QD_OFFSET(m) offsetof(struct zqdata, m)

zfs_prop_list_t *zfs_get_prop_list(int quota_type)
{
	static zfs_prop_list_t usrquota_props[] = {
		{
			.prop = ZFS_PROP_USERUSED,
			.offset = QD_OFFSET(space_used),
		},
		{
			.prop = ZFS_PROP_USERQUOTA,
			.offset = QD_OFFSET(space_quota),
		},
#ifdef HAVE_ZFS_OBJECT_QUOTA
		{
			.prop = ZFS_PROP_USEROBJUSED,
			.offset = QD_OFFSET(obj_used),
		},
		{
			.prop = ZFS_PROP_USEROBJQUOTA,
			.offset = QD_OFFSET(obj_quota),
		},
#endif /* HAVE_ZFS_OBJECT_QUOTA */
		{
			.prop = -1,
		}
	};
	static zfs_prop_list_t grpquota_props[] = {
		{
			.prop = ZFS_PROP_GROUPUSED,
			.offset = QD_OFFSET(space_used),
		},
		{
			.prop = ZFS_PROP_GROUPQUOTA,
			.offset = QD_OFFSET(space_quota),
		},
#ifdef HAVE_ZFS_OBJECT_QUOTA
		{
			.prop = ZFS_PROP_GROUPOBJUSED,
			.offset = QD_OFFSET(obj_used),
		},
		{
			.prop = ZFS_PROP_GROUPOBJQUOTA,
			.offset = QD_OFFSET(obj_quota),
		},
#endif /* HAVE_ZFS_OBJECT_QUOTA */
		{
			.prop = -1,
		}
	};

	switch(quota_type) {
	case USRQUOTA:
		return usrquota_props;
	case GRPQUOTA:
		return grpquota_props;
	default:
		WARN(1, "Unknown quota type: %d\n", quota_type);
		return 0;
	}
}


int zfs_set_space_quota(void *zfs_handle, int quota_type, qid_t id,
			uint64_t limit)
{
	zfs_userquota_prop_t type;

	switch (quota_type) {
	case USRQUOTA:
		type = ZFS_PROP_USERQUOTA;
		break;
	case GRPQUOTA:
		type = ZFS_PROP_GROUPQUOTA;
		break;
	default:
		return -EINVAL;
	}

	return zfs_set_userquota(zfs_handle, type, "", id, limit);
}

#ifdef HAVE_ZFS_OBJECT_QUOTA
int zfs_set_object_quota(void *zfs_handle, int quota_type, qid_t id,
			 uint64_t limit)
{
	zfs_userquota_prop_t type;

	switch (quota_type) {
	case USRQUOTA:
		type = ZFS_PROP_USEROBJQUOTA;
		break;
	case GRPQUOTA:
		type = ZFS_PROP_GROUPOBJQUOTA;
		break;
	default:
		return -EINVAL;
	}

	return zfs_set_userquota(zfs_handle, type, "", id, limit);
}
#endif /* HAVE_ZFS_OBJECT_QUOTA */


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
