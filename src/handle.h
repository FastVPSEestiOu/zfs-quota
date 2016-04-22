#ifndef HANDLE_H_INCLUDED
#define HANDLE_H_INCLUDED

struct zqhandle;

void zqhandle_put(struct zqhandle *handle);
struct zqhandle *zqhandle_get(void *sb);

/* Register and unregister fake-FS superblock  */
int zqhandle_register_superblock(struct super_block *sb);
int zqhandle_unregister_superblock(struct super_block *sb);

/* Get/set quota dqblk for given superblock, quota type and id */
int zqhandle_get_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);
int zqhandle_set_quota_dqblk(void *sb, int type, qid_t id, struct if_dqblk *di);

#endif /* #ifndef HANDLE_H_INCLUDED */
