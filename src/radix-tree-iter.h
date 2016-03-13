
#ifndef RADIX_TREE_ITER_H_INCLUDED
#define RADIX_TREE_ITER_H_INCLUDED

typedef struct radix_tree_iter radix_tree_iter_t;

void radix_tree_iter_start(radix_tree_iter_t * iter,
			   struct radix_tree_root *root,
			   unsigned long start_key);
void *radix_tree_iter_item(radix_tree_iter_t * iter);
void radix_tree_iter_next_(radix_tree_iter_t * iter);

#define radix_tree_iter_next(iter, key) (do { \
    iter->next_key = key; \
    radix_tree_iter_next_(iter); \
while(0))

#endif /* RADIX_TREE_ITER_H_INCLUDED */
