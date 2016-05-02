
#include <linux/proc_fs.h>

#include "proc-compat.h"

#ifndef HAVE_PROC_GET_PARENT_DATA
void *proc_get_parent_data(const struct inode *inode)
{
	return PROC_I(inode)->pde->parent->data;
}
#endif /* #ifndef HAVE_PROC_GET_PARENT_DATA */

#ifndef HAVE_PROC_MKDIR_DATA
struct proc_dir_entry *proc_mkdir_data(const char *name, umode_t mode,
		struct proc_dir_entry *parent, void *data)
{
	struct proc_dir_entry *pde;
	pde = proc_mkdir(name, parent);
	if (pde) {
		if (mode)
			pde->mode = S_IFDIR | mode;
		pde->data = data;
	}
	return pde;
}
#endif /* #ifndef HAVE_PROC_MKDIR_DATA */
