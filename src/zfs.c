
#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zpl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>

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

#ifdef USEROBJ_QUOTA
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
#endif /* USEROBJ_QUOTA */

	return 0;
}

typedef int fillinfo_t(uint64_t qid, uint64_t value, void *data);

int zfsquota_query_many(void *zfs_handle, int prop, fillinfo_t cb, void *cbdata)
{
	zfs_useracct_t *buf;
	void *vbuf;
	uint64_t bufsize = sizeof(*buf) * 16;
	uint64_t retsize, i, cookie = 0;
	int error;

	vbuf = vmem_alloc(bufsize, KM_SLEEP);
	if (vbuf == NULL)
		return -ENOMEM;

	do {
		retsize = bufsize;
		buf = vbuf;

		error =
		    zfs_userspace_many(zfs_handle, (zfs_userquota_prop_t) prop,
				       &cookie, buf, &retsize);
		if (error)
			goto out;

		for (i = 0; i < retsize / sizeof(*buf); ++i) {
			if (cb(buf->zu_rid, buf->zu_space, cbdata))
				goto out;
			buf++;
		}
	} while (retsize);

out:
	vmem_free(vbuf, bufsize);

	return error;
}

static int cb_print_value(uint64_t rid, uint64_t space, void *data)
{
	uint64_t *pi = data;
	printk("rid = %Lu, space = %Lu, *pi = %Lu\n", rid, space, *pi);
	(*pi)++;
	return 0;
}

int spam_zfs_quota(struct super_block *zfs_sb)
{
	uint64_t i;
	return zfsquota_query_many(zfs_sb->s_fs_info, ZFS_PROP_USERUSED,
				   cb_print_value, &i);
}
