
#ifndef QUOTA_H_INCLUDED
#define QUOTA_H_INCLUDED

int zfsquota_setup_quota(struct super_block *sb);
int zfsquota_teardown_quota(struct super_block *sb);

#endif
