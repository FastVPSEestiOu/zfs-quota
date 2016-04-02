
#ifndef PROC_H_INCLUDED
#define PROC_H_INCLUDED

struct proc_dir_entry;

struct proc_dir_entry* zqproc_register_handle(struct super_block *sb);
int zqproc_unregister_handle(struct super_block *sb);

int zqproc_get_sb_type(struct inode *, struct super_block **,
		       int *);

#endif /* PROC_H_INCLUDED */
