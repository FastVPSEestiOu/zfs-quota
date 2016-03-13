
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

int zqtree_init_superblock(struct super_block *sb);
int zqtree_free_superblock(struct super_block *sb);

#endif /* TREE_H_INCLUDED */
