
#ifndef PROC_H_INCLUDED
#define PROC_H_INCLUDED

#define INO_MASK	0xec000000UL

static inline int zfs_aquot_inode_masked(unsigned long i_ino)
{
	return ((i_ino) & INO_MASK) == INO_MASK;
}

static inline unsigned long zfs_aquot_getino(dev_t dev, int type)
{
	return INO_MASK | (dev << 8) | (type & 0xFF);
}

static inline dev_t zfs_aquot_getdev(unsigned long i_ino)
{
	return (INO_MASK ^ i_ino) >> 8;
}

static inline int zfs_aquot_type(unsigned long i_ino)
{
	return i_ino & 0xFF;
}

struct proc_dir_entry;

struct proc_dir_entry* zqproc_register_handle(struct super_block *sb);
int zqproc_unregister_handle(struct super_block *sb);

int zqproc_get_sb_type(struct inode *, struct super_block **,
		       int *);

#endif /* PROC_H_INCLUDED */
