
#include <linux/vzquota.h>
#include <linux/virtinfo.h>
#include <linux/ve_proto.h>

#include "quota.h"

static int zfsquota_notifier_call(struct vnotifier_block *self,
				  unsigned long n, void *data, int err)
{
	struct virt_info_quota *viq = (struct virt_info_quota *)data;
	int status = 0;

	switch (n) {
	case VIRTINFO_QUOTA_ON:
		status = zfsquota_setup_quota(viq->super);
		break;
	case VIRTINFO_QUOTA_OFF:
		status = zfsquota_teardown_quota(viq->super);
		break;
	}
	if (status) {
		printk("ZFSQUOTA: Action %lu returned %d\n", n, status);
		err = NOTIFY_BAD;
	}
	return err;
}

struct vnotifier_block zfsquota_notifier_block = {
	.notifier_call = zfsquota_notifier_call,
	.priority = INT_MAX / 2
};

int __init zfsquota_proc_vz_init(void);
void __exit zfsquota_proc_vz_exit(void);

int __init zfsquota_vz_init(void)
{
	zfsquota_proc_vz_init();
	virtinfo_notifier_register(VITYPE_QUOTA, &zfsquota_notifier_block);

	return 0;
}

void __exit zfsquota_vz_exit(void)
{
	zfsquota_proc_vz_exit();
	virtinfo_notifier_unregister(VITYPE_QUOTA, &zfsquota_notifier_block);
}
