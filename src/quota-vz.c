
#include <linux/vzquota.h>
#include <linux/virtinfo.h>
#include <linux/ve_proto.h>

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
