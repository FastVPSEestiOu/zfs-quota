
#include <linux/radix-tree.h>
#include "radix-tree-iter.h"

#define RADIX_TREE_BUFSIZE 16

struct radix_tree_iter {
	struct radix_tree_root *root;
	int next_key;
	void *values[RADIX_TREE_BUFSIZE];
	int idx, count;
};

void radix_tree_iter_start(radix_tree_iter_t * iter,
			   struct radix_tree_root *root,
			   unsigned long start_key)
{
	iter->root = root;
	iter->next_key = start_key;
	iter->idx = 0;
	iter->count = 0;
}

void *radix_tree_iter_item(radix_tree_iter_t * iter)
{
	if (iter->count == 0)
		return NULL;

	return iter->values[iter->idx];
}

void radix_tree_iter_next_(radix_tree_iter_t * iter)
{
	iter->idx++;

	if (iter->idx == iter->count) {
		iter->idx = 0;
		iter->count = radix_tree_gang_lookup(iter->root, iter->values,
						     iter->next_key,
						     RADIX_TREE_BUFSIZE);
	}
}
