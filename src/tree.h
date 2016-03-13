
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct zfs_handle_data {
	struct radix_tree_root user_quota_tree;
	struct radix_tree_root group_quota_tree;
};

struct quota_data {
	qid_t qid;
	uint32_t valid;
	uint64_t space_used, space_quota;
#ifdef USEROBJ_QUOTA
	uint64_t obj_used, obj_quota;
#endif				/* USEROBJ_QUOTA */
};

#endif /* TREE_H_INCLUDED */
