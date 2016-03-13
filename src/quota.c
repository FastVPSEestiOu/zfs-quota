
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vzquota.h>
#include <linux/virtinfo.h>
#include <linux/ve_proto.h>

#include <linux/quotaops.h>

#include "tree.h"

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

int zfsquota_get_dqblk(struct super_block *sb, int type,
		       qid_t id, struct if_dqblk *di)
{
	printk("%s\n", __func__);

	memset(di, 0, sizeof(*di));

	return zqtree_get_quota_dqblk(sb, type, id, di);
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
	memset(ii, 0, sizeof(*ii));
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

int zqtree_zfs_sync_tree(void *sb, int type);

static int zfsquota_sync(struct super_block *sb, int type)
{
	printk("%s\n", __func__);
	if (type == -1) {
		zqtree_zfs_sync_tree(sb, USRQUOTA);
		zqtree_zfs_sync_tree(sb, GRPQUOTA);
	} else
		zqtree_zfs_sync_tree(sb, type);
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

static int zfsquota_notify_quota_on(struct super_block *sb)
{
	if (strcmp(sb->s_op->get_quota_root(sb)->i_sb->s_type->name, "zfs")) {
		return NOTIFY_OK;
	}
	if (!try_module_get(THIS_MODULE))
		return NOTIFY_BAD;

	sb->s_qcop = &zfsquota_q_cops;
	sb->s_dquot.flags = dquot_state_flag(DQUOT_USAGE_ENABLED, USRQUOTA) |
	    dquot_state_flag(DQUOT_USAGE_ENABLED, GRPQUOTA);
	sb->s_dquot.info[USRQUOTA].dqi_format = &zfs_quota_empty_v2_format;
	sb->s_dquot.info[GRPQUOTA].dqi_format = &zfs_quota_empty_v2_format;

	if (zqtree_init_superblock(sb))
		return NOTIFY_BAD;

	return NOTIFY_OK;
}

static int zfsquota_notify_quota_off(struct super_block *sb)
{
	if (sb->s_qcop != &zfsquota_q_cops) {
		return NOTIFY_OK;
	}
	zqtree_free_superblock(sb);
	module_put(THIS_MODULE);

	return NOTIFY_OK;
}

static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		err = zfsquota_notify_quota_on(viq->super);
		break;
	case VIRTINFO_QUOTA_OFF:
		err = zfsquota_notify_quota_off(viq->super);
		break;
	}
	return err;
}

struct vnotifier_block zfsquota_notifier_block = {
	.notifier_call = zfsquota_notifier_call,
	.priority = INT_MAX / 2
};

int __init zfsquota_proc_init(void);
void __exit zfsquota_proc_exit(void);
int __init zfsquota_tree_init(void);
int __init zfsquota_tree_exit(void);

static int __init zfsquota_init(void)
{
	zfsquota_proc_init();
	zfsquota_tree_init();

	virtinfo_notifier_register(VITYPE_QUOTA, &zfsquota_notifier_block);

	register_quota_format(&zfs_quota_empty_v2_format);
	return 0;
}

static void __exit zfsquota_exit(void)
{
	zfsquota_proc_exit();
	zfsquota_tree_exit();
	virtinfo_notifier_unregister(VITYPE_QUOTA, &zfsquota_notifier_block);

	unregister_quota_format(&zfs_quota_empty_v2_format);

	return;
}

MODULE_AUTHOR("Pavel Boldin <boldin.pavel@gmail.com>");
MODULE_DESCRIPTION("ZFS quota <-> OpenVZ proxy");
MODULE_LICENSE("DUNNO");

module_init(zfsquota_init);
module_exit(zfsquota_exit);
