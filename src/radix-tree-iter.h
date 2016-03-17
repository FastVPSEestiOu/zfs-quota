
#ifndef RADIX_TREE_ITER_H_INCLUDED
#define RADIX_TREE_ITER_H_INCLUDED

#define RADIX_TREE_BUFSIZE 16

typedef struct radix_tree_iter {
	struct radix_tree_root *root;
	unsigned long next_key;
	void *values[RADIX_TREE_BUFSIZE];
	int idx, count;
} radix_tree_iter_t;

void radix_tree_iter_start(radix_tree_iter_t * iter,
			   struct radix_tree_root *root,
			   unsigned long start_key);
void *radix_tree_iter_item(radix_tree_iter_t * iter);
void radix_tree_iter_next_(radix_tree_iter_t * iter);

#define radix_tree_iter_next(iter, key) \
    ((iter)->next_key = ((key) + 1), radix_tree_iter_next_(iter))

#endif /* RADIX_TREE_ITER_H_INCLUDED */
