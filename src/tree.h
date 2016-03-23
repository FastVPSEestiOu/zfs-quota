
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct quota_data {
	qid_t qid;
	uint32_t version;
	uint64_t space_used, space_quota;
#ifdef	OBJECT_QUOTA
	uint64_t obj_used, obj_quota;
#endif	/* OBJECT_QUOTA */
};

struct quota_tree;

struct quota_data *zqtree_get_quota_data(void *sb, int type, qid_t id);
int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);
int zqtree_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

int zqtree_check_qd_version(struct quota_tree *root, struct quota_data *qd);

struct quota_tree *zqtree_get_sync_quota_tree(void *sb, int type);
void zqtree_put_quota_tree(struct quota_tree *quota_tree, int type);
void zqtree_zfs_sync_tree(void *sb, int type);

void zqtree_print_quota_data(struct quota_data *qd);
int zqtree_print_tree(struct quota_tree *root);
int zqtree_print_tree_sb_type(void *sb, int type);

int zqtree_init_superblock(struct super_block *sb);
int zqtree_free_superblock(struct super_block *sb);

#ifdef RADIX_TREE_ITER_H_INCLUDED
void quota_tree_iter_start(
		my_radix_tree_iter_t *iter,
		struct quota_tree *root,
		unsigned long start_key);
int quota_tree_gang_lookup(struct quota_tree *root,
			   struct quota_data **pqd,
			   unsigned long start_key,
			   unsigned int max_items);
#endif

#endif /* TREE_H_INCLUDED */
