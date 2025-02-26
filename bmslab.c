/*
 * bmslab: A Multithreaded Bitmap based Slab Allocator
 *
 * This code implements a slab allocator (bmslab) designed for managing fixed-size
 * objects in a multithreaded environment. The allocator organizes memory into pages,
 * each subdivided into slots tracked by a per-page bitmap.
 *
 * Key features include:
 *
 * 1. Memory Management:
 *    - Memory is allocated in pages using mmap.
 *    - Each page is divided into a number of fixed-size slots, determined by PAGE_SIZE
 *      divided by the object size (obj_size), with a maximum of 512 slots per page.
 *
 * 2. Bitmap Tracking:
 *    - Each page contains 16 submaps (arrays of 32-bit integers), where each bit
 *      represents a slot (0 indicates free, 1 indicates used).
 *
 * 3. Dynamic Physical Page Expansion and Shrinkage:
 *    - When the allocated slot count exceeds a predefined threshold (PAGE_EXPAND_THRESHOLD),
 *      adaptive_phys_page_expand() adds a new physical page.
 *    - When usage drops below a threshold (PAGE_SHRINK_THRESHOLD), adaptive_phys_page_shrink()
 *      attempts to free the last physical page.
 *
 * 4. Randomized Allocation:
 *    - The allocator uses a variant of the MurmurHash3 (murmurhash32) to distribute
 *      allocation attempts across pages and submaps, reducing contention.
 */

#define _GNU_SOURCE
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include <string.h>
#include <assert.h>

#include "bmslab.h"

#ifndef __cacheline_aligned
#define __cacheline_aligned __attribute__((aligned(64)))
#endif /* __cacheline_aligned */

#define PAGE_SIZE	(4096)
#define PAGE_SHIFT	(12)

#define PAGE_EXPAND_THRESHOLD(max_page_cnt) (max_page_cnt >> 1)
#define PAGE_SHRINK_THRESHOLD(max_page_cnt) (max_page_cnt >> 3)

#define PAGE_LOCK_MASK (0x8000000000000000ULL)
#define IS_PAGE_LOCKED(page_lock_ref) ((page_lock_ref) & PAGE_LOCK_MASK)

#define SUBMAP_COUNT (16)

_Thread_local static uint32_t tls_murmur_seed = 0;

/*
 * bmslab_bitmap - per-page bitmap
 * @submap: 16 arrays of 32-bit bitmaps (each = 4 bytes)
 *
 * Each page has 16 submaps, each submap covers 32 slots
 * (bit=0 => free, bit=1 => used).
 *
 * Total capacity per page = 16 * 32 = 512 slots (max).
 */
struct bmslab_bitmap {
	_Atomic uint32_t submap[SUBMAP_COUNT];
} __cacheline_aligned;

/*
 * bmslab - top-level structure
 * @page_lock_refs: array of lock bit and reference count for each page
 * @allocated_slot_count: global count of allocated slots
 * @phys_page_count_flag: flag to enable only one thread to control page count
 * @phys_page_count: number of physical pages
 * @virt_page_count: number of virtual pages
 * @slot_count_per_page: number of valid slots per page
 * @obj_size: size of each object
 * @base_addr: base address of the contiguos pages
 * @bitmaps: array of bmslab_bitmap, each describing one page's submaps
 */
struct bmslab {
	_Atomic uint64_t *page_lock_refs;
	_Atomic uint32_t allocated_slot_count;
	_Atomic uint32_t phys_page_count_flag;
	_Atomic uint32_t phys_page_count;
	uint32_t virt_page_count;
	uint32_t slot_count_per_page;
	uint32_t obj_size;
	void *base_addr;
	struct bmslab_bitmap *bitmaps;
};

int get_bmslab_phys_page_count(struct bmslab *slab)
{
	return atomic_load(&slab->phys_page_count_flag);
}

int get_bmslab_allocated_slots(struct bmslab *slab)
{
	return atomic_load(&slab->allocated_slot_count);
}

/*
 * bmslab_init - initializes a bmslab
 * @obj_size: size of each object (must be >= 8 and <= PAGE_SIZE)
 * @phys_page_count: number of pages to allocate
 *
 * Returns pointer to a bmslab on sucess, or NULL on failure.
 *
 * We compute how many slots actually fit (PAGE_SIZE / obj_siz), capped at 512.
 * Then we mark only those bits as (0 => free), the rest as (1 => unavailable)
 * for simple exception handling.
 */
struct bmslab *bmslab_init(int obj_size, int max_page_count)
{
	int submap_idx, bit_idx;
	uint32_t mask, oldv;
	struct bmslab *slab;

	if (obj_size < 8 || obj_size > PAGE_SIZE) {
		fprintf(stderr, "bmslab_init: invalid obj_size\n");
		return NULL;
	}

	if (max_page_count == 0) {
		fprintf(stderr, "bmslab_init: invalid max_page_count\n");
		return NULL;
	}

	slab = calloc(1, sizeof(struct bmslab));
	if (slab == NULL) {
		fprintf(stderr, "bmslab_init: slab allocation failed\n");
		return NULL;
	}

	atomic_store(&slab->phys_page_count_flag, 0);
	atomic_store(&slab->virt_page_count, max_page_count);
	atomic_store(&slab->phys_page_count, 1); /* initial page usage */
	atomic_store(&slab->allocated_slot_count, 0);

	slab->obj_size = obj_size;
	slab->slot_count_per_page = PAGE_SIZE / obj_size;

	slab->bitmaps = calloc(slab->virt_page_count, sizeof(struct bmslab_bitmap));
	if (slab->bitmaps == NULL) {
		fprintf(stderr, "bmslab_init: slab->bitmaps allocation failed\n");
		free(slab);
		return NULL;
	}

	slab->page_lock_refs = calloc(slab->virt_page_count, sizeof(uint64_t));
	if (slab->page_lock_refs == NULL) {
		fprintf(stderr, "bmslab_init: slab->page_lock_refs allocation failed\n");
		free(slab->bitmaps);
		free(slab);
		return NULL;
	}

	slab->base_addr = mmap(NULL, slab->virt_page_count * PAGE_SIZE,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (slab->base_addr == MAP_FAILED) {
		fprintf(stderr, "bmslab_init: slab->base_addr allocation failed\n");
		free(slab->bitmaps);
		free(slab->page_lock_refs);
		free(slab);
		return NULL;
	}

	/* Initialize each page's submaps */
	for (uint32_t page_idx = 0; page_idx < slab->virt_page_count; page_idx++) {
		for (uint32_t i = 0; i < SUBMAP_COUNT; i++) {
			atomic_init(&slab->bitmaps[page_idx].submap[i], 0xffffffffU);
		}

		/* Distribute slots across the submaps */
		for (uint32_t s = 0; s < slab->slot_count_per_page; s++) {
			submap_idx = s % SUBMAP_COUNT;
			bit_idx = s / SUBMAP_COUNT;
			
			mask = ~(1U << bit_idx);
			oldv = atomic_load(&slab->bitmaps[page_idx].submap[submap_idx]);

			atomic_store(&slab->bitmaps[page_idx].submap[submap_idx],
				oldv & mask);
		}
	}

	return slab;
}

/*
 * bmslab_destroy - fress the bmslab
 * @slab: pointer to bmslab
 */
void bmslab_destroy(struct bmslab *slab)
{
	if (slab == NULL)
		return;

	free(slab->page_lock_refs);
	free(slab->bitmaps);
	munmap(slab->base_addr, slab->virt_page_count * PAGE_SIZE);
	free(slab);
}

/*
 * murmurhash32 - small MurmurHash3 variant for 32-bit output
 * @key: pointer to data to be hashed
 * @len: length of the data (in bytes)
 * @seed: initial seed value
 */
static inline uint32_t murmurhash32(const void *key, size_t len, uint32_t seed)
{
	const uint8_t *data = (const uint8_t *)key;
	uint32_t h = seed;
	const uint32_t c1 = 0xcc9e2d51;
	const uint32_t c2 = 0x1b873593;
	size_t i;

	for (i = 0; i + 4 <= len; i += 4) {
		uint32_t k;
		memcpy(&k, data + i, 4);
		k *= c1;
		k = (k << 15) | (k >> 17);
		k *= c2;
		h ^= k;
		h = (h << 13) | (h >> 19);
		h = h * 5 + 0xe6546b64;
	}

	{
		uint32_t k = 0;
		int tail_len = len - i;

		if (tail_len > 0) {
			memcpy(&k, data + i, tail_len);
			k *= c1;
			k = (k << 15) | (k >> 17);
			k *= c2;
			h ^= k;
		}
	}

	h ^= (uint32_t)len;
	h ^= (h >> 16);
	h *= 0x85ebca6b;
	h ^= (h >> 13);
	h *= 0xc2b2ae35;
	h ^= (h >> 16);

	return h;
}

static inline void *page_start(struct bmslab *slab, int page_idx)
{
	return (void *)((char *)slab->base_addr + (page_idx << PAGE_SHIFT));
}

static inline uint32_t get_max_slot_count(struct bmslab *slab)
{
	return atomic_load(&slab->phys_page_count) * slab->slot_count_per_page;
}

static inline void lock_page(struct bmslab *slab, int page_idx)
{
	atomic_fetch_or(&slab->page_lock_refs[page_idx], PAGE_LOCK_MASK);
}

static inline void unlock_page(struct bmslab *slab, int page_idx)
{
	atomic_fetch_and(&slab->page_lock_refs[page_idx], ~PAGE_LOCK_MASK);
}

static inline bool is_page_reclaimable(uint64_t page_lock_ref)
{
	return (page_lock_ref == PAGE_LOCK_MASK);
}

/*
 * adaptive_phys_page_expand - expand physical page count if needed
 * @slab: pointer to bmslab
 *
 * Gradually increase the number of physical pages when slot usage exceeds the
 * threshold, but ensure that only one thread performs this operation to prevent
 * exceeding the user-defined memory limit.
 */
static void adaptive_phys_page_expand(struct bmslab *slab)
{
	uint32_t slot_count = atomic_load(&slab->allocated_slot_count);
	uint32_t max_slot_count = get_max_slot_count(slab);
	uint32_t expected = 0;
	int new_page_idx;

	if (slot_count < PAGE_EXPAND_THRESHOLD(max_slot_count))
		return;

	if (!atomic_compare_exchange_weak(&slab->phys_page_count_flag,
			&expected, 1))
		return;	

	if (atomic_load(&slab->phys_page_count)
			< atomic_load(&slab->virt_page_count)) {
		new_page_idx = atomic_fetch_add(&slab->phys_page_count, 1U);
		unlock_page(slab, new_page_idx);
	}

	atomic_store(&slab->phys_page_count_flag, 0);
}

/*
 * adaptive_phys_page_shrink - shrink physical page count if needed
 * @slab: pointer to bmslab
 *
 * Gradually decrease the number of physical pages when slot usage falls below
 * the threshold.
 *
 * This is performed from the last physical page backward. In this process, the
 * lock bit of page_lock_ref is set first to prevent new allocations. If the
 * reference count also reaches zero, madvise with MADV_FREE is used to release
 * the physical page.
 *
 * Use slab->phys_page_count_flag to prevent sudden fluctuations in the number
 * of physical pages.
 */
static void adaptive_phys_page_shrink(struct bmslab *slab)
{
	uint32_t slot_count = atomic_load(&slab->allocated_slot_count);
	uint32_t max_slot_count = get_max_slot_count(slab);
	uint32_t expected = 0;
	uint64_t page_lock_ref;
	int last_page_idx;	

	if (slot_count > PAGE_SHRINK_THRESHOLD(max_slot_count))
		return;

	if (!atomic_compare_exchange_weak(&slab->phys_page_count_flag,
			&expected, 1))
		return;	

	last_page_idx = atomic_load(&slab->phys_page_count) - 1;
	if (last_page_idx == 0) { /* do not free the first page */
		atomic_store(&slab->phys_page_count_flag, 0);
		return;
	}

	lock_page(slab, last_page_idx);
	atomic_thread_fence(memory_order_seq_cst);

	page_lock_ref = atomic_load(&slab->page_lock_refs[last_page_idx]);
	if (is_page_reclaimable(page_lock_ref)) {
		/*
		 * At this point, no new threads can allocate slots on this page, and
		 * all currently allocated slots have been returned.
		 *
		 * Applying the MADV_FREE flag allows the physical memory of this page
		 * to be freed when memory pressure occurs. Note that if the page is
		 * accessed before being freed, a write operation will cancle the
		 * MADV_FREE status.
		 */
		madvise(page_start(slab, last_page_idx), PAGE_SIZE, MADV_FREE);
		atomic_fetch_sub(&slab->phys_page_count, 1U);
	}

	atomic_store(&slab->phys_page_count_flag, 0);
}

/*
 * try_ref_page - try to reference the given page
 * @slab: pointer to bmslab
 * @page_idx: target page index
 *
 * Increase the reference counter of the given page
 * (slab->page_lock_refs[page_idx]).
 *
 * If the page was locked, return false. Otherwise return true.
 */
static bool try_ref_page(struct bmslab *slab, int page_idx)
{
	uint64_t page_lock_ref
		= atomic_fetch_add(&slab->page_lock_refs[page_idx], 1U);

	if (IS_PAGE_LOCKED(page_lock_ref)) {
		atomic_fetch_sub(&slab->page_lock_refs[page_idx], 1U);
		return false;
	}

	return true;
}

/*
 * bmslab_alloc - allocate one object from bmslab
 * @slab: pointer to bmslab
 *
 * We use hashing to randomly determine both the page index and submap index to
 * reduce CAS contention.
 *
 * If we find a free bit (0), we set it to 1 with a CAS. On success, compute the
 * slot index => pointer and return.
 *
 * If a slot is allocated, increment the used slot counter. If necessary,
 * increase the number of physical pages. Keeping too few pages may increase the
 * time complexity of allocation.
 *
 * If we exhaust all pages without success, return NULL.
 */
void *bmslab_alloc(struct bmslab *slab)
{
	uint32_t page_start_idx, page_idx;
	uint32_t submap_start_idx, submap_idx, slot_idx;
	int bit_idx;
	uint32_t oldv, newv;
	void *sp;

	if (slab == NULL)
		return NULL;

	sp = __builtin_frame_address(0);
	
retry:

	/* Distribute the cache-lines */
	page_start_idx = murmurhash32(&sp, sizeof(sp), tls_murmur_seed++)
		% slab->phys_page_count;
	
	for (uint32_t i = 0; i < slab->phys_page_count; i++) {
		page_idx = (page_start_idx + i) % slab->phys_page_count;

		/* If this page is locked, move to the next page */
		if (!try_ref_page(slab, page_idx))
			continue;

		/* Distribute the addresses within the cache-line */
		submap_start_idx
			= murmurhash32(&sp, sizeof(sp), tls_murmur_seed++) % SUBMAP_COUNT;

		for (uint32_t sub_i = 0; sub_i < SUBMAP_COUNT; sub_i++) {
			submap_idx = (submap_start_idx + sub_i) % SUBMAP_COUNT;
			oldv = atomic_load(&slab->bitmaps[page_idx].submap[submap_idx]);

			/* Move to the next submap */
			if (oldv == 0xFFFFFFFFU)
				continue;

			bit_idx = __builtin_ctz(~oldv);
			if (bit_idx < 0 || bit_idx >= 32)
				continue;

			newv = oldv | (1U << bit_idx);
			if (atomic_compare_exchange_weak(
					&slab->bitmaps[page_idx].submap[submap_idx],
					&oldv, newv)) {
				slot_idx = bit_idx * SUBMAP_COUNT + submap_idx;
				assert(slot_idx < slab->slot_count_per_page);

				/*
				 * Increase the global allocated slot counter and expand the
				 * number of physical page if needed.
				 */
				atomic_fetch_add(&slab->allocated_slot_count, 1U);
				adaptive_phys_page_expand(slab);

				return (void *)((char *)page_start(slab, page_idx)
					+ slot_idx * slab->obj_size);
			}
		}

		/* Move to the next page */
		atomic_fetch_sub(&slab->page_lock_refs[page_idx], 1U);
	}

	if (atomic_load(&slab->phys_page_count)
		< atomic_load(&slab->virt_page_count)) {
		adaptive_phys_page_expand(slab);
		goto retry;
	}

	return NULL;
}

/*
 * bmslab_free - frees an object pointer
 * @slab: pointer to bmslab
 * @ptr: object pointer to free
 *
 * We compute page_idx from (ptr - slab->base_addr) >> PAGE_SHIFT, then slot_idx
 * from ((ptr - page_start) / obj_size).
 *
 * The submap index is (slot_idx % 16), bit index is (slot_idx / 16). Clear this
 * bit (1->0) with fetch_and.
 */
void bmslab_free(struct bmslab *slab, void *ptr)
{
	uintptr_t base, diff, page_base;
	uint32_t page_idx, submap_idx, slot_idx, bit_idx;
	size_t offset;

	if (slab == NULL || ptr == NULL)
		return;

	base = (uintptr_t)slab->base_addr;
	diff = (uintptr_t)ptr - base;

	page_idx = diff >> PAGE_SHIFT;
	if (page_idx >= slab->virt_page_count) {
		fprintf(stderr, "bmslab_free: invalid page_idx\n");
		return;
	}

	page_base = base + (page_idx << PAGE_SHIFT);
	offset = (uintptr_t)ptr - page_base;

	slot_idx = (int)(offset / slab->obj_size);
	assert(slot_idx < slab->slot_count_per_page);

	submap_idx = slot_idx % SUBMAP_COUNT;
	bit_idx = slot_idx / SUBMAP_COUNT;

	atomic_fetch_and(&slab->bitmaps[page_idx].submap[submap_idx],
		~(1U << bit_idx));

	atomic_fetch_sub(&slab->allocated_slot_count, 1U);

	atomic_fetch_sub(&slab->page_lock_refs[page_idx], 1);

	adaptive_phys_page_shrink(slab);
}





