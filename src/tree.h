
#ifndef TREE_H_INCLUDED
#define TREE_H_INCLUDED

#define	ZQTREE_EMPTY		0

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

/*
 * Z(FS)Q(UOTA) tree utils
 */

struct zqtree *zqtree_new(struct zqhandle *handle, int type,
			  unsigned int qid_limit);
struct zqtree *zqtree_get(struct zqtree *qt);
void zqtree_put(struct zqtree *qt);

/* Upgrade zqtree, can sleep */
int zqtree_upgrade(struct zqtree * zqtree);

/* Printing utilities */
int zqtree_print_tree(struct zqtree *root);
void zqtree_print_quota_data(struct zqdata *qd);

/* Block tree interface */
int zqtree_output_magic(struct zqtree *zqtree, char *buf);
int zqtree_output_block(struct zqtree *zqtree, char *buf, uint32_t blknum);

#endif /* TREE_H_INCLUDED */
