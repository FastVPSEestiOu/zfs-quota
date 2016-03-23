
#ifndef PROC_H_INCLUDED
#define PROC_H_INCLUDED

#define INO_MASK	0xec000000UL

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

#endif /* PROC_H_INCLUDED */
