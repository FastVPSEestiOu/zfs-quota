
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct quota_data {
	qid_t qid;
	uint32_t version;
	uint64_t space_used, space_quota;
#ifdef	HAVE_ZFS_OBJECT_QUOTA
	uint64_t obj_used, obj_quota;
#endif	/* HAVE_ZFS_OBJECT_QUOTA */
};

struct quota_tree;
struct zqhandle;

/* Register and unregister fake-FS superblock  */
int zqtree_init_superblock(struct super_block *sb);
int zqtree_free_superblock(struct super_block *sb);

/*
 * Z(FS)Q(UOTA) tree utils
 */

/* Get synced zqtree */
struct quota_tree *zqtree_get_sync_quota_tree(void *sb, int type);

/* Put zqtree */
void zqtree_put_quota_tree(struct quota_tree *quota_tree);

/* Sync zqtree for superblock and type */
void zqtree_zfs_sync_tree(void *sb, int type);


/* Get/set quota dqblk for given superblock, quota type and id */
int zqtree_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);
int zqtree_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

/* Checks if quota_data version is OK and marks data for removal otherwise */
int zqtree_check_qd_version(struct quota_tree *root, struct quota_data *qd);

/* Printing utilities */
int zqtree_print_tree(struct quota_tree *root);
int zqtree_print_tree_sb_type(void *sb, int type);

void zqtree_print_quota_data(struct quota_data *qd);


#ifdef RADIX_TREE_ITER_H_INCLUDED
/* Start my_radix_tree_iter for the quota_tree */
void quota_tree_iter_start(
		my_radix_tree_iter_t *iter,
		struct quota_tree *quota_tree,
		unsigned long start_key);
/* Lookup for a gang in quota_tree */
int quota_tree_gang_lookup(struct quota_tree *root,
			   struct quota_data **pqd,
			   unsigned long start_key,
			   unsigned int max_items);
#endif

#endif /* TREE_H_INCLUDED */
