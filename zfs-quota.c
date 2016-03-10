
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vzquota.h>
#include <linux/virtinfo.h>

#include <linux/quotaops.h>

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

static int zfsquota_on(struct super_block *sb, int type, int id, char *name,
		       int remount)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_off(struct super_block *sb, int type, int remount)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_get_dqblk(struct super_block *sb, int type,
			      qid_t id, struct if_dqblk *di)
{
	int err;
	struct super_block *zfs_sb = sb->s_op->get_quota_root(sb)->i_sb;
	uint64_t rid = id;
	uint64_t used, quota;

	printk("%s\n", __func__);

	err = zfs_userspace_one(zfs_sb->s_fs_info,
				type ==
				USRQUOTA ? ZFS_PROP_USERUSED :
				ZFS_PROP_GROUPUSED, "", rid, &used);
	if (err)
		return err;

	err = zfs_userspace_one(zfs_sb->s_fs_info,
				type ==
				USRQUOTA ? ZFS_PROP_USERQUOTA :
				ZFS_PROP_GROUPQUOTA, "", rid, &quota);
	if (err)
		return err;

	di->dqb_curspace = used;
	di->dqb_valid = QIF_SPACE;
	if (quota) {
		di->dqb_valid |= QIF_BLIMITS;
		di->dqb_bsoftlimit = di->dqb_bhardlimit = quota;
	}

	return 0;
}

static int zfsquota_set_dqblk(struct super_block *sb, int type,
			      qid_t id, struct if_dqblk *di)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_get_info(struct super_block *sb, int type,
			     struct if_dqinfo *ii)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_set_info(struct super_block *sb, int type,
			     struct if_dqinfo *ii)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_get_quoti(struct super_block *sb, int type, qid_t idx,
			      struct v2_disk_dqblk __user * dqblk)
{
	printk("%s\n", __func__);
	return 0;
}

static int zfsquota_sync(struct super_block *sb, int type)
{
	printk("%s\n", __func__);
	spam_zfs_quota(sb->s_op->get_quota_root(sb)->i_sb);
	return 0;
}

struct quotactl_ops zfsquota_q_cops = {
	.quota_on = zfsquota_on,
	.quota_off = zfsquota_off,
	.quota_sync = zfsquota_sync,
	.get_info = zfsquota_get_info,
	.set_info = zfsquota_set_info,
	.get_dqblk = zfsquota_get_dqblk,
	.set_dqblk = zfsquota_set_dqblk,
#ifdef CONFIG_QUOTA_COMPAT
	.get_quoti = zfsquota_get_quoti,
#endif
};

struct quota_format_type zfs_quota_empty_v2_format = {
	.qf_fmt_id = QFMT_VFS_OLD,
	.qf_ops = NULL,
	.qf_owner = THIS_MODULE,
};

static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;
	struct super_block *sb;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		err = NOTIFY_BAD;
		sb = viq->super;
		if (strcmp
		    (sb->s_op->get_quota_root(sb)->i_sb->s_type->name, "zfs")) {
			err = NOTIFY_OK;
			break;
		}
		if (!try_module_get(THIS_MODULE))
			break;

		sb->s_qcop = &zfsquota_q_cops;
		sb->s_dquot.flags =
		    dquot_state_flag(DQUOT_USAGE_ENABLED,
				     USRQUOTA) |
		    dquot_state_flag(DQUOT_USAGE_ENABLED, GRPQUOTA);
		sb->s_dquot.info[USRQUOTA].dqi_format =
		    &zfs_quota_empty_v2_format;
		sb->s_dquot.info[GRPQUOTA].dqi_format =
		    &zfs_quota_empty_v2_format;
		err = NOTIFY_OK | NOTIFY_STOP_MASK;
		break;
	case VIRTINFO_QUOTA_OFF:
		err = NOTIFY_BAD;
		sb = viq->super;
		if (sb->s_qcop != &zfsquota_q_cops) {
			err = NOTIFY_OK;
			break;
		}
		module_put(THIS_MODULE);
		err = NOTIFY_OK;
	}
	return err;
}

struct vnotifier_block zfsquota_notifier_block = {
	.notifier_call = zfsquota_notifier_call,
	.priority = INT_MAX / 2
};

static int __init zfsquota_init(void)
{
	struct proc_dir_entry *de;

	de = proc_create("zfsquota", S_IFDIR | S_IRUSR | S_IXUSR,
			 &glob_proc_root, NULL);

	if (de)
		de->proc_iops = NULL;
	else
		return -ENOMEM;

	virtinfo_notifier_register(VITYPE_QUOTA, &zfsquota_notifier_block);

	register_quota_format(&zfs_quota_empty_v2_format);
	return 0;
}

static void __exit zfsquota_exit(void)
{
	virtinfo_notifier_unregister(VITYPE_QUOTA, &zfsquota_notifier_block);

	unregister_quota_format(&zfs_quota_empty_v2_format);

	remove_proc_entry("zfsquota", NULL);
	return;
}

MODULE_AUTHOR("Pavel Boldin <boldin.pavel@gmail.com>");
MODULE_DESCRIPTION("ZFS quota <-> OpenVZ proxy");
MODULE_LICENSE("DUNNO");

module_init(zfsquota_init);
module_exit(zfsquota_exit);
