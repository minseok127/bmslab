# BMSLAB
Bitmap based slab allocator, designed for a multi-threaded environment.
- lock-free allocation
- wait-free deallocation
- cacheline distribution to reduce contention
- adaptive physical memory expanding and shrinking

Note: the object size must be (8<= and <=4096), page size is 4096.

# Build
```
$ git clone https://github.com/minseok127/bmslab.git
$ cd bmslab
$ make
$ tree
|
-- libbmslab.a
|
-- libbmslab.so
|
-- bmslab.h
```

# API

- bmslab_init(int obj_size, int max_page_count)
  - Initializes a new slab allocator instance.
  - Arguments:
    - obj_size: The size (in bytes) of each fixed-size object. Must be between 8 and 4096.
    - max_page_count: The maximum number of pages to allocate.
  - Returns: A pointer to the newly created slab (bmslab_t *), or NULL on failure.

- bmslab_destroy(bmslab_t *slab)
  - Destroys the slab allocator.
  - Frees all allocated resources (e.g., memory maps, bitmaps).
  - Does nothing if slab is NULL.

- bmslab_alloc(bmslab_t *slab)
  - Allocates one object from the slab.
  - Returns: A pointer to the allocated object or NULL if allocation fails.

- bmslab_free(bmslab_t *slab, void *ptr)
  - Frees a previously allocated object.
  - If ptr is invalid or NULL, the function simply returns.

# Evaluation

## Environment

- Hardware
	- CPU: Intel Core i5-13400F (16 cores)
	- RAM: 16GB DDR5 5600MHz

- Software
	- OS: Ubuntu 24.04.1 LTS
	- Compiler: GCC 13.3.0
	- Build System: GNU Make 4.3
