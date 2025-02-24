#ifndef BMSLAB_H
#define BMSLAB_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct bmslab bmslab_t;

bmslab_t *bmslab_init(int obj_size, int max_page_count);

void bmslab_destroy(bmslab_t *slab);

void *bmslab_alloc(bmslab_t *slab);

void bmslab_free(bmslab_t *slab, void *ptr);

/* stat */
int get_bmslab_phys_page_count(struct bmslab *slab);
int get_bmslab_allocated_slots(struct bmslab *slab);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* BMSLAB_H */
