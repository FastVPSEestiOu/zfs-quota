
#ifndef ZFS_H_INCLUDED
#define ZFS_H_INCLUDED

typedef struct zfs_prop_pair {
	uint64_t rid, value;
} zfs_prop_pair_t;

typedef struct zfs_prop_iter {
	void *zfs_handle;
	int prop;

	void *buf;
	uint64_t bufsize, retsize, offset;
	uint64_t cookie;
	zfs_prop_pair_t pair;
	int error;
} zfs_prop_iter_t;

typedef struct zfs_prop_list {
	int prop;
	uintptr_t offset;
} zfs_prop_list_t;
zfs_prop_list_t *zfs_get_prop_list(int quota_type);


int zfs_set_space_quota(void *zfs_handle, int quota_type, qid_t id,
			uint64_t limit);
#ifdef OBJECT_QUOTA
int zfs_set_object_quota(void *zfs_handle, int quota_type, qid_t id,
			 uint64_t limit);
#endif /* OBJECT_QUOTA */

void zfs_prop_iter_start(void *zfs_handle, int prop, zfs_prop_iter_t * iter);
zfs_prop_pair_t *zfs_prop_iter_item(zfs_prop_iter_t * iter);
void zfs_prop_iter_next(zfs_prop_iter_t * iter);
void zfs_prop_iter_stop(zfs_prop_iter_t * iter);
void zfs_prop_iter_reset(int prop, zfs_prop_iter_t * iter);
int zfs_prop_iter_error(zfs_prop_iter_t * iter);

#endif /* ZFS_H_INCLUDED */
