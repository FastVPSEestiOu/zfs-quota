
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct quota_data {
	qid_t qid;
	uint32_t version;
	uint64_t space_used, space_quota;
#ifdef USEROBJ_QUOTA
	uint64_t obj_used, obj_quota;
#endif				/* USEROBJ_QUOTA */
};

struct quota_data *zqtree_get_quota_data(void *sb, int type, qid_t id,
					 int update);
int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

struct radix_tree_root *zqtree_get_tree_for_type(void *sb, int type, uint32_t *pversion);

int zqtree_remove_old_quota_data(struct radix_tree_root *root, struct quota_data *qd);

int zqtree_zfs_sync_tree(void *sb, int type);

void zqtree_print_quota_data(struct quota_data *qd);
int zqtree_print_tree(struct radix_tree_root *root);
int zqtree_print_tree_sb_type(void *sb, int type);

int zqtree_init_superblock(struct super_block *sb);
int zqtree_free_superblock(struct super_block *sb);

#endif /* TREE_H_INCLUDED */
