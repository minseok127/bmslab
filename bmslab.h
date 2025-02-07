#ifndef BMSLAB_H
#define BMSLAB_H

#include <stddef.h>

typedef struct bmslab bmslab_t;

bmslab_t *bmslab_init(int obj_size, int max_page_count);
void *bmslab_alloc(bmslab_t *slab);
void bmslab_free(bmslab_t *slab, void *ptr);
void bmslab_destroy(bmslab_t *slab);

#endif /* BMSLAB_H */
