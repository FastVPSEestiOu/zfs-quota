
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

#define	ZQTREE_EMPTY		1
#define	ZQTREE_QUOTA		2
#define	ZQTREE_ZFSREQ		-2
#define	ZQTREE_BLKTREE		3
#define	ZQTREE_BLDBLKTREE	-3

struct zqdata {
	qid_t qid;
	uint32_t version;
	uint64_t space_used, space_quota;
#ifdef	HAVE_ZFS_OBJECT_QUOTA
	uint64_t obj_used, obj_quota;
#endif	/* HAVE_ZFS_OBJECT_QUOTA */
};

struct zqtree;

/*
 * Z(FS)Q(UOTA) tree utils
 */

/* Upgrade zqtree to the status, can sleep */
int zqtree_upgrade(struct zqtree * zqtree, int target_state);

/* Put zqtree */
void zqtree_put_quota_tree(struct zqtree *quota_tree);

/* Printing utilities */
int zqtree_print_tree(struct zqtree *root);
void zqtree_print_quota_data(struct zqdata *qd);

#endif /* TREE_H_INCLUDED */
