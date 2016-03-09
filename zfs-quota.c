
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vzquota.h>
#include <linux/virtinfo.h>

const struct super_operations zpl_super_operations;

static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;
	struct super_block *sb;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		err = NOTIFY_OK;
		sb = viq->super;
		if (sb->s_op == &zpl_super_operations) {
			printk(KERN_INFO "ZFSQuota: got sb = %p", sb);
		}
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
