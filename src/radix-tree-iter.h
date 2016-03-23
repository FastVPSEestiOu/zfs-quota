
#ifndef RADIX_TREE_ITER_H_INCLUDED
#define RADIX_TREE_ITER_H_INCLUDED

#define RADIX_TREE_BUFSIZE 16

typedef struct my_radix_tree_iter {
	struct radix_tree_root *root;
	unsigned long next_key;
	void *values[RADIX_TREE_BUFSIZE];
	int idx, count;
} my_radix_tree_iter_t;

void my_radix_tree_iter_start(my_radix_tree_iter_t * iter,
			   struct radix_tree_root *root,
			   unsigned long start_key);
void *my_radix_tree_iter_item(my_radix_tree_iter_t * iter);
void my_radix_tree_iter_next_(my_radix_tree_iter_t * iter);

#define my_radix_tree_iter_next(iter, key) \
    ((iter)->next_key = ((key) + 1), my_radix_tree_iter_next_(iter))

#endif /* RADIX_TREE_ITER_H_INCLUDED */
