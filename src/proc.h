
#ifndef PROC_H_INCLUDED
#define PROC_H_INCLUDED

static inline unsigned long zfs_aquot_getino(dev_t dev)
{
	return 0xec000000UL + dev;
}

static inline dev_t zfs_aquot_getidev(struct inode *inode)
{
	return (dev_t) (unsigned long)PROC_I(inode)->op.proc_get_link;
}

static inline void zfs_aquot_setidev(struct inode *inode, dev_t dev)
{
	PROC_I(inode)->op.proc_get_link = (void *)(unsigned long)dev;
}

extern struct file_operations zfs_aquotf_vfsold_file_operations;

#endif /* PROC_H_INCLUDED */
