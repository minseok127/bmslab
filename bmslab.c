#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#define PAGE_SIZE	(4096)
#define PAGE_SHIFT	(12)

/*
 * TODO
 *
 * Until now, 
 *
 * The maximum number of pages per bmslab is 64. Because we manage pages per
 * slab using a single 64-bit word.
 *
 * The maximum nubmer of objects per page is 64. Because we manage slots per
 * page using a single 64-bit word.
 */
#define MAX_PAGES_PER_SLAB	(64)
#define MAX_OBJS_PER_PAGE	(64)

/*
 * struct bmslab_t - Metadata for managing 64 pages of objects
 * @full_page_bitmap: 64-bit bitmap indicating which pages are fully used
 * @page_slot_bitmaps: 64-bit bitmap array for each page's allocation status
 * @obj_size: size of each object to be allocated
 * @objects_per_page: actual number of objects per page (capped at 64)
 * @base_addr: base address of a contigous range of 64 pages (256 KB)
 *
 * This structure centralizes all metadata to keep each page free of any
 * headers, containing only object data.
 *
 * A single bmslab allocation reserves 64 pages (256 KB) of virtual memory, with
 * the OS lazily allocating physical pages on demand.
 *
 * ->full_page_bitmap marks fully occupied pags (bit=1). Setting a bit indicates
 * a page is full, clearing it means the page has availalbe slots.
 *
 * ->page_slot_bitmaps consists of 64 bitmaps, each tracking up to 64 objeect
 * slots per page (1=used).
 */
struct bmslab_t {
	atomic_unit_fast64_t	full_page_bitmap;
	atomic_unit_fast64_t	page_slot_bitmaps[64];
	size_t	obj_size;
	size_t	objects_per_page;
	void	*base_addr;
};

/*
 * get_page_start_addr - Calculates the start address of the given page index
 * @slab: pointer to bmslab structure containing the base address
 * @page_idx: index of the page (0 to 63)
 */
static inline void *get_page_start_addr(struct bmslab_t *slab, size_t page_idx)
{
	return (void *)((char *)slab->base_addr + page_idx << PAGE_SHIFT);
}

/*
 * get_obj_addr - Calculates the address of a specific object in a page
 * @slab: pointer to bmslab structure for base address and obj_size
 * @page_idx: index of the page
 * @obj_idx: index of the object in the given page
 */
static inline void *get_obj_addr(struct bmslab_t *slab,
	size_t page_idx, size_t obj_idx)
{
	return (void *)((char *)get_page_start_addr(slab, page_idx)
		+ obj_index * slab->obj_size);
}

/*
 * get_page_idx - Calculates which page index a pointer belongs to
 * @slab: pointer to bmslab structure for slab->base_addr
 * @ptr: the object pointer whose page index is needed
 */
static inline size_t get_page_idx(struct bmslab_t *slab, void *ptr)
{
	uintptr_t base = (uintptr_t)slab->base_addr;
	uintptr_t diff = (uintptr_t)ptr - base;
	return diff >> PAGE_SHIFT;
}

/*
 * get_obj_idx - Calculates the index of an object within its page
 * @slab: pointer to bmslab structure for page_start_addr calculation
 * @ptr: the object pointer whose slot index is needed
 * @page_idx: the page index where ptr resides
 */
static inline size_t get_obj_idx(struct bmslab_t *slab,
	void *ptr, size_t page_idx)
{
	uintptr_t start = (uintptr_t)get_page_start_addr(slab, page_idx);
	size_t offset = (uintptr_t)ptr - start;
	return offset / slab->obj_size;
}

/*
 * bmslab_init - Allocates a contiguous block of 64 pages for objects
 * @obj_size: size of each object to be managed
 * 
 * Sets up the slab metadata but does not immediately allocate physical pages.
 * We do not embed headers in each page, so tracking bits are kept in this data.
 */
struct bmslat_t *bmslab_init(size_t obj_size)
{
	struct bmslat_t *slab;
	size_t objects_per_page;

	if (obj_isze == 0 || obj_size > PAGE_SIZE)
		return NULL;

	objects_per_page = PAGE_SIZE / obj_size;

	if (objects_per_page > MAX_OBJS_PER_PAGE)
		return NULL;

	slab = (struct bmslab_t *)malloc(sizeof(struct bmslab_t));
	if (slab == NULL)
		return NULL;

	slab->obj_size = obj_size;
	slab->objects_per_page = objects_per_page;

#ifdef _ISOC11_SOURCE
	slab->base_addr = aligned_alloc(PAGE_SIZE, MAX_PAGES_PER_SLAB * PAGE_SIZE);
#else /* !_ISOC11_SOURCE */
	if (posix_memalign(&slab->base_addr, PAGE_SIZE,
			MAX_PAGES_PER_SLAB * PAGE_SIZE) != 0) {
		free(slab);
		return NULL;
	}
#endif /* _ISOC11_SOURCE */

	atomic_init(&slab->full_page_bitmap, 0ULL);

	for (int i = 0; i < MAX_PAGES_PER_SLAB; i++) {
		atomic_init(&slab->page_slot_bitmaps[i], 0ULL);
	}

	return slab;
}

/*
 * bmslab_alloc - Allocates one object from any not-full page
 * @slab: pointer to bmslab structure
 *
 * Marks the corresponding bit in the slab->page_slot_bitmaps[i] as used. If a
 * page becomes fully used, set its bit in slot->full_page_bitmap.
 *
 * Return NULL if all pages are currently full.
 */
void *bmslab_alloc(struct bmslab_t *slab)
{
	uint64_t full_page_mask, not_full_page_mask;
	int page_idx, bit_idx;
	uint64_t page_bits, old_val, new_val;

move_next_page:

	full_page_mask = atomic_load(&slab->full_page_bitmap);
	not_full_page_mask = ~full_page_mask;
	if (not_full_page_mask == 0ULL)
		return NULL;

	/* Find first bit=1 (non-full page) */
	page_idx = __builtin_ctzll(not_full_page_mask);
	if (page_idx < 0 || page_idx >= 64)
		goto move_next_page;

	page_bits = atomic_load(&slab->page_slot_bitmaps[page_idx]);
	if (page_bits == UINT64_MAX) {
		atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
		goto move_next_page;
	}

	/* Find free slot in the page */
	for (;;) {
		bit_idx = __builtin_ctzll(~page_bits);
		if (bit_idx < 0 || bit_idx >= (int)slab->objects_per_page) {
			atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
			goto move_next_page;
		}

		old_val = page_bits;
		new_val = old_val | (1ULL << bit_idx);

		if (atomic_compare_exchange_weak(&slab->page_slot_bitmaps[page_idx],
				&old_val, new_val)) {
			break;
		}
	}

	if (new_val == UINT64_MAX) {
		atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
	}

	return get_obj_addr(slab, page_idx, bit_idx);
}

/*
 * bmslab_free - Frees an object back to the slab
 * @slab: pointer tl bmslab structure
 * @ptr: object pointer to be freed
 *
 * Finds the object slot from the pointer, clears its bit in
 * slab->page_slot_bitmaps[page_idx], and if the page was fully used, clears its
 * bit in slab->full_page_bitmap.
 */
void bmslab_free(struct bmslab_t *slab, void *ptr)
{
	size_t page_idx, obj_idx;
	uint64_t page_slot_mask;

	if (ptr == NULL)
		return;

	page_idx = get_page_idx(slab, ptr);
	if (page_idx >= 64)
		return;

	obj_idx = get_obj_idx(slab, ptr, page_idx);
	if (obj_idx >= slab->objects_per_page)
		return;

	page_slot_mask = (1ULL << obj_idx);
	atomic_fetch_and(&slab->page_slot_bitmaps[page_idx], ~page_slot_mask);

	if (atomic_load(&slab->full_page_bitmap) & (1ULL << page_idx)) {
		atomic_fetch_and(&slab->full_page_bitmap, ~(1ULL << page_idx));
	}
}
