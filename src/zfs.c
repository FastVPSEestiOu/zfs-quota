
#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zpl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>

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

int zfsquota_get_quota_dqblk(void *zfs_handle, int type, qid_t id,
			     struct if_dqblk *di)
{
	int err;
	uint64_t rid = id;
	uint64_t used, quota;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USERUSED :
				ZFS_PROP_GROUPUSED, "", rid, &used);
	if (err)
		return err;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USERQUOTA :
				ZFS_PROP_GROUPQUOTA, "", rid, &quota);
	if (err)
		return err;

	di->dqb_curspace = used;
	di->dqb_valid |= QIF_SPACE;
	if (quota) {
		di->dqb_bsoftlimit = di->dqb_bhardlimit = quota;
		di->dqb_valid |= QIF_BLIMITS;
	}
#ifdef DNODE_FLAG_USEROBJUSED_ACCOUNTED
	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USEROBJUSED :
				ZFS_PROP_GROUPOBJUSED, "", rid, &used);
	if (err == EOPNOTSUPP)
		goto no_obj_quota;

	if (err)
		return err;

	err = zfs_userspace_one(zfs_handle,
				type ==
				USRQUOTA ? ZFS_PROP_USEROBJQUOTA :
				ZFS_PROP_GROUPOBJQUOTA, "", rid, &quota);
	if (err)
		return err;

	di->dqb_curinodes = used;
	di->dqb_valid |= QIF_INODES;
	if (quota) {
		di->dqb_isoftlimit = di->dqb_ihardlimit = quota;
		di->dqb_valid |= QIF_ILIMITS;
	}

no_obj_quota:
	;

#endif /* DNODE_FLAG_USEROBJUSED_ACCOUNTED */
	return 0;
}
