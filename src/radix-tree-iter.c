
#include <linux/radix-tree.h>
#include "radix-tree-iter.h"

void my_radix_tree_iter_next_(my_radix_tree_iter_t * iter);
void my_radix_tree_iter_start(my_radix_tree_iter_t * iter,
			   struct radix_tree_root *root,
			   unsigned long start_key)
{
	iter->root = root;
	iter->next_key = start_key;
	iter->idx = 0;
	iter->count = 0;

	my_radix_tree_iter_next_(iter);
}

void *my_radix_tree_iter_item(my_radix_tree_iter_t * iter)
{
	if (iter->count == 0)
		return NULL;

	return iter->values[iter->idx];
}

void my_radix_tree_iter_next_(my_radix_tree_iter_t * iter)
{
	iter->idx++;

	if (iter->idx >= iter->count) {
		iter->idx = 0;
		iter->count = radix_tree_gang_lookup(iter->root, iter->values,
						     iter->next_key,
						     RADIX_TREE_BUFSIZE);
	}
}
