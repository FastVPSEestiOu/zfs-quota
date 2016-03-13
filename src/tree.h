
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct quota_data {
	qid_t qid;
	uint32_t valid;
	uint64_t space_used, space_quota;
#ifdef USEROBJ_QUOTA
	uint64_t obj_used, obj_quota;
#endif				/* USEROBJ_QUOTA */
};

struct quota_data *zfsquota_get_quotadata(void *zfs_handle, int type, qid_t id,
					  int update);
int zfsquota_get_quota_dqblk(void *zfs_handle, int type, qid_t id,
			     struct if_dqblk *di);

#endif /* TREE_H_INCLUDED */
