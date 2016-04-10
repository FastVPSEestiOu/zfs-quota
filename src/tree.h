
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

struct zqdata {
	qid_t qid;
	uint32_t version;
	uint64_t space_used, space_quota;
#ifdef	HAVE_ZFS_OBJECT_QUOTA
	uint64_t obj_used, obj_quota;
#endif	/* HAVE_ZFS_OBJECT_QUOTA */
};

struct zqtree;
struct zqhandle;

/* Register and unregister fake-FS superblock  */
int zqhandle_register_superblock(struct super_block *sb);
int zqhandle_unregister_superblock(struct super_block *sb);

/*
 * Z(FS)Q(UOTA) tree utils
 */

/* Get synced zqtree */
struct zqtree *zqtree_get_sync_quota_tree(void *sb, int type);

/* Put zqtree */
void zqtree_put_quota_tree(struct zqtree *quota_tree);

/* Get/set quota dqblk for given superblock, quota type and id */
int zqhandle_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);
int zqhandle_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

/* Printing utilities */
int zqtree_print_tree(struct zqtree *root);

void zqtree_print_quota_data(struct zqdata *qd);

void zqtree_put(struct zqtree *qt);


#ifdef RADIX_TREE_ITER_H_INCLUDED
/* Start my_radix_tree_iter for the quota_tree */
void quota_tree_iter_start(
		my_radix_tree_iter_t *iter,
		struct zqtree *quota_tree,
		unsigned long start_key);
/* Lookup for a gang in quota_tree */
int quota_tree_gang_lookup(struct zqtree *root,
			   struct zqdata **pqd,
			   unsigned long start_key,
			   unsigned int max_items);
#endif

#endif /* TREE_H_INCLUDED */
