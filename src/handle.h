#ifndef HANDLE_H_INCLUDED
#define HANDLE_H_INCLUDED

struct zqhandle;
struct zqtree;

void zqhandle_put(struct zqhandle *handle);
struct zqhandle *zqhandle_get_by_sb(void *sb);
void *zqhandle_get_zfsh(struct zqhandle *handle);

struct zqtree *zqhandle_get_tree(struct zqhandle *handle, int type,
				 int required_state);
/* Unreference tree from the handle */
void zqhandle_unref_tree(struct zqhandle *handle, struct zqtree *zqtree);

/* Register and unregister fake-FS superblock  */
int zqhandle_register_superblock(struct super_block *sb);
int zqhandle_unregister_superblock(struct super_block *sb);

/* Get/set quota dqblk for given superblock, quota type and id */
int zqhandle_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);
int zqhandle_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

#endif /* #ifndef HANDLE_H_INCLUDED */
