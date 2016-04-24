
#ifndef QUOTA_H_INCLUDED
#define QUOTA_H_INCLUDED

struct zfsquota_options {
	unsigned int	qid_limit;
};

int zfsquota_setup_quota(struct super_block *sb);
int zfsquota_setup_quota_opts(struct super_block *sb,
			      struct zfsquota_options *opts);
int zfsquota_teardown_quota(struct super_block *sb);

#endif
