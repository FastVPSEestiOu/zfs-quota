
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

void zfs_prop_iter_start(void *zfs_handle, int prop, zfs_prop_iter_t * iter);
zfs_prop_pair_t *zfs_prop_iter_item(zfs_prop_iter_t * iter);
void zfs_prop_iter_next(zfs_prop_iter_t * iter);
void zfs_prop_iter_stop(zfs_prop_iter_t * iter);
void zfs_prop_iter_reset(int prop, zfs_prop_iter_t * iter);
int zfs_prop_iter_error(zfs_prop_iter_t * iter);

#endif /* ZFS_H_INCLUDED */
