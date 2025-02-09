## BMSLAB
Bitmap based slab allocator.
- lock-free allocation
- wait-free deallocation
- cacheline distribution to reduce contention
- adaptive physical memory expanding and shrinking

Note: the object size must be (8<= and <=4096), page size is 4096.

## Build
```
$ git clone https://github.com/minseok127/bmslab.git
$ cd bmslab
$ make
=> libbmslab.a, libbmslab.so, bmslab.h
```

## API
```
bmslab_t *bmslab_init(int obj_size, int max_page_count);
void *bmslab_alloc(bmslab_t *slab);
void bmslab_free(bmslab_t *slab, void *ptr);
void bmslab_destroy(bmslab_t *slab);
```

## Usage
The user calls bmslab_init to obtain a bmslab structure. This initialization function takes the byte size of objects to be allocated and the maximum number of pages the slab can use. The maximum page count represents the virtual memory limit, while the actual number of physical pages is adjusted gradually based on slab usage.

Objects are allocated with bmslab_alloc, and pointers obtained through it must be returned to the slab using bmslab_free. Once the slab is no longer needed, bmslab_destroy should be called to release the bmslab structure.

