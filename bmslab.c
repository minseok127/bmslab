#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#define PAGE_SIZE	(4096)
#define PAGE_SHIFT	(12)

/*
 * The maximum number of pages per bmslab is 64. Because we manage pages per
 * slab using a single 64-bit word.
 */
#define MAX_PAGES_PER_SLAB	(64)

/*
 * Each page has 8 words (512 bits) to manage its slots. So the maximum number
 * of objects per page is 512.
 */
#define NUM_SUB_BITMAPS	(8)

/*
 * struct bmslab - Metadata for managing 64 pages of objects
 * @full_page_bitmap: 64-bit bitmap indicating which pages are fully used
 * @page_slot_bitmaps: page_slot_bitmaps[page][0..num_submpas-1]
 * @usage[page]: how many slots are used in that page
 * @obj_size: size of each object to be allocated
 * @num_submaps: how many 64-bit sub-bitmaps we actually use (1..8)
 * @max_slots: how many slots each page actually holds (<= num_submaps*64)
 * @base_addr: base address of a contigous range of 64 pages (256 KB)
 *
 * This structure centralizes all metadata to keep each page free of any
 * headers, containing only object data.
 *
 * A single bmslab allocation reserves 64 pages (256 KB) of virtual memory, with
 * the OS lazily allocating physical pages on demand.
 */
struct bmslab {
	atomic_uint_fast64_t full_page_bitmap;
	atomic_uint_fast64_t page_slot_bitmaps[MAX_PAGES_PER_SLAB][MAX_SUB_BITMAPS];
	atomic_size_t usage[MAX_PAGES_PER_SLAB];
	size_t obj_size;
	size_t num_submaps;
	size_t max_slots;
	void *base_addr;
};

/*
 * get_page_start_addr - Calculates the start address of the given page index
 * @slab: pointer to bmslab structure containing the base address
 * @page_idx: index of the page (0 to 63)
 */
static inline void *get_page_start_addr(struct bmslab *slab, size_t page_idx)
{
	return (void *)((char *)slab->base_addr + (page_idx << PAGE_SHIFT));
}

/*
 * get_obj_addr - Calculates the address of a specific object in a page
 * @slab: pointer to bmslab structure for base address and obj_size
 * @page_idx: index of the page
 * @obj_idx: index of the object in the given page
 */
static inline void *get_obj_addr(struct bmslab *slab,
	size_t page_idx, size_t obj_idx)
{
	return (void *)((char *)get_page_start_addr(slab, page_idx)
		+ obj_idx * slab->obj_size);
}

/*
 * get_page_idx - Calculates which page index a pointer belongs to
 * @slab: pointer to bmslab structure for slab->base_addr
 * @ptr: the object pointer whose page index is needed
 */
static inline size_t get_page_idx(struct bmslab *slab, void *ptr)
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
static inline size_t get_obj_idx(struct bmslab *slab,
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
 * We do not embed headers in each page, so tracking bits are kept in the
 * inside of struct bmslab.
 */
struct bmslab *bmslab_init(size_t obj_size)
{
	struct bmslab *slab;
	size_t slots_per_page;

	if (obj_size < 8 || obj_size > PAGE_SIZE)
		return NULL;

	slab = (struct bmslab *)calloc(1, sizeof(struct bmslab));
	if (slab == NULL)
		return NULL;

	slab->obj_size = obj_size;

#ifdef _ISOC11_SOURCE
	slab->base_addr = aligned_alloc(PAGE_SIZE, MAX_PAGES_PER_SLAB * PAGE_SIZE);
#else /* !_ISOC11_SOURCE */
	if (posix_memalign(&slab->base_addr, PAGE_SIZE,
			MAX_PAGES_PER_SLAB * PAGE_SIZE) != 0) {
		free(slab);
		return NULL;
	}
#endif /* _ISOC11_SOURCE */

	slots_per_page = PAGE_SIZE / obj_size;
	assert(slots_per_page <= MAX_SUB_BITMAPS * 64);

	slab->num_submaps = (slots_per_page + 63U) / 64U;
	assert(slab->num_submaps >= 1);

	if (slab->num_submaps * 64 > slots_per_page) { 
		slab->max_slots = slots_per_page;
	} else {
		slab->max_slots = slab->num_submaps * 64;
	}

	atomic_init(&slab->full_page_bitmap, 0ULL);

	for (int i = 0; i < MAX_PAGES_PER_SLAB; i++) {
		atomic_init(&slab->usage[i], 0U);

		for (int b = 0; b < MAX_SUB_BITMAPS; b++) {
			atomic_init(&slab->page_slot_bitmaps[i][b], 0ULL);
		}
	}

	return slab;
}


/*
 * submap_hash - Hashing function to distribute submap
 * @sp: stack pointer
 * @nsub: the number of submap
 */
static inline size_t submap_hash(void *sp, size_t nsub)
{
	uint64_t x = (uint64_t)sp * 114007
}

/*
 * get_free_page_idx - Searches for a page that is not full
 * @slab: pointer to bmslab structur
 *
 * Return -1 if all pages are full.
 */
static int get_free_page_idx(struct bmslab *slab)
{
	uint64_t full_page_mask, not_full_page_mask;
	int page_idx;

	full_page_mask = atomic_load(&slab->full_page_bitmap);
	not_full_page_mask = ~full_page_mask;
	if (not_full_page_mask == 0ULL)
		return -1;

	page_idx = __builtin_ctzll(not_full_page_mask);
	assert(page_idx >= 0 && page_idx < 64);

	if (atomic_load(&slab->usage[page_idx]) >= slab->max_slots) {
		atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
		return -1;
	}

	return page_idx;
}

/*
 * get_free_bit_idx - Searches for a bit index that is not allocated
 * @slab: pointer to bmslab structure
 * @word: target bitmap
 *
 * Return -1 if all slots are allocated.
 */
static int get_free_bit_idx(struct bmslab *slab, uint64_t word)
{
	int bit_idx;

	if (word == UINT64_MAX)
		return -1;

	bit_idx = __builtin_ctzll(~word);
	if (bit_idx >= (int)slab->max_slots || bit_idx >= 64)
		return -1;

	return bit_idx;
}

/*
 * bmslab_alloc - Allocates one object from any not-full page
 * @slab: pointer to bmslab structure
 *
 * Marks the corresponding bit in the slab->page_slot_bitmaps[page_idx] as used.
 * If the page becomes fully used, set its bit in slot->full_page_bitmap.
 *
 * Return NULL if all pages are currently full.
 */
void *bmslab_alloc(struct bmslab *slab)
{
	int page_idx, bit_idx, slot_idx, map_idx;
	size_t start_submap_idx, usage;
	uint64_t old_val, new_val;

	while ((page_idx = get_free_page_idx(slab)) != -1) {
		start_submap_idx = submap_hash(
			__builtin_frame_address(0), slab->num_submaps);

		/* Traverse bitmaps using round robin from the random point */
		for (int i = 0; i < (int)slab->num_submaps; i++) {
			map_idx = (start_submap_idx + i) % slab->num_submaps;
			old_val = atomic_load(&slab->page_slot_bitmaps[page_idx][map_idx]);

			bit_idx = get_free_bit_idx(slab, old_val);
			if (bit_idx == -1)
				continue;

			new_val = old_val | (1ULL << bit_idx);
			if (atomic_compare_exchange_weak(
					&slab->page_slot_bitmaps[page_idx][b] &old_val, new_val)) {
				usage = 1U + atomic_fetch_add(&slab->usage[page_idx], 1U);
				if (usage == slab->max_slots) {
					atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
				}
				
				slot_idx = b * 64U + (size_t)bit_idx;
				return get_obj_addr(slab, page_idx, slot_idx);
			}
		}

		/* This page is full */
		atomic_fetch_or(&slab->full_page_bitmap, 1ULL << page_idx);
	}

	return NULL;
}

/*
 * bmslab_free - Frees an object back to the slab
 * @slab: pointer tl bmslab structure
 * @ptr: object pointer to be freed
 *
 * Finds the object slot from the pointer, clears its bit in
 * slab->page_slot_bitmaps[page_idx][submap_idx]. If the page was fully used,
 * clears its bit in slab->full_page_bitmap[page_idx].
 */
void bmslab_free(struct bmslab *slab, void *ptr)
{
	size_t page_idx, obj_idx, submap_idx, bit_idx, old_usage;
	uint64_t page_slot_mask;

	if (ptr == NULL)
		return;

	page_idx = get_page_idx(slab, ptr);
	if (page_idx >= 64)
		return;

	obj_idx = get_obj_idx(slab, ptr, page_idx);
	if (obj_idx >= slab->max_slots)
		return;

	submap_idx = obj_idx / 64;
	bit_idx = obj_idx % 64;

	atomic_fetch_and(&slab->page_slot_bitmaps[page_idx][submap_idx],
		 ~(1ULL << bit_idx));

	usage = atomic_fetch_sub(&slab->usage[page_idx], 1U);
	if (old_usage == slab->max_slots) {
		atomic_fetch_and(&slab->full_page_bitmap, ~(1ULL << page_idx));
	}
}

/*
 * bmslab_destroy - Release the slab and its 64-page allocation
 * @slab: pointer tl bmslab structure
 */
void bmslab_destroy(struct bmslab *slab)
{
	if (slab == NULL)
		return;

	free(slab->base_addr);
	free(slab);
}
