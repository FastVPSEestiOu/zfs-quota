
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vzquota.h>
#include <linux/virtinfo.h>

#include <zfs_config.h>
#include <sys/zfs_context.h>
#include <sys/types.h>
#include <sys/zpl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_vfsops.h>

static int spam_zfs_quota(struct super_block *zfs_sb)
{
	zfs_useracct_t *buf;
	void *vbuf;
	int error, i;
	uint64_t bufsize = sizeof(*buf) * 10;
	uint64_t cookie = 0;

	vbuf = buf = vmem_alloc(10, KM_SLEEP);
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

static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;
	struct super_block *sb, *real_sb;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		err = NOTIFY_OK;
		sb = viq->super;
		real_sb = sb->s_op->get_quota_root(sb)->i_sb;
		printk("ZFSQuota: got sb = %p, fstype = %s\n", sb,
		       sb->s_type->name);
		printk("ZFSQuota: got real_sb = %p, fstype = %s\n", real_sb,
		       real_sb->s_type->name);
		if (strcmp(real_sb->s_type->name, "zfs") == 0)
			spam_zfs_quota(real_sb);
		break;
	}
	return err;
}

struct vnotifier_block zfsquota_notifier_block = {
	.notifier_call = zfsquota_notifier_call,
	.priority = INT_MAX / 2
};

static int __init zfsquota_init(void)
{
	virtinfo_notifier_register(VITYPE_QUOTA, &zfsquota_notifier_block);

	return 0;
}

static void __exit zfsquota_exit(void)
{
	virtinfo_notifier_unregister(VITYPE_QUOTA, &zfsquota_notifier_block);

	return;
}

MODULE_AUTHOR("Pavel Boldin <boldin.pavel@gmail.com>");
MODULE_DESCRIPTION("ZFS quota <-> OpenVZ proxy");
MODULE_LICENSE("DUNNO");

module_init(zfsquota_init);
module_exit(zfsquota_exit);
