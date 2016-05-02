
#ifndef PROC_COMPAT_H_INCLUDED
#define PROC_COMPAT_H_INCLUDED

void *proc_get_parent_data(const struct inode *inode);

struct proc_dir_entry *proc_mkdir_data(const char *name, umode_t mode,
		struct proc_dir_entry *parent, void *data);

#endif /* PROC_COMPAT_H_INCLUDED */
