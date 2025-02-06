## bmslab
Bitmap based slab allocator
- lock-free allocation
- wait-free deallocation
- cache Line distribution

Note: the object size must be (8 <= and 4096 >=), page size is 4096.

## build
```
$ git clone https://github.com/minseok127/bmslab.git
$ cd bmslab
$ make
=> libbmslab.a, libbmslab.so, bmslab.h
```

## API
```
bmslab_t *bmslab_init(int obj_size, int page_count);
void *bmslab_alloc(bmslab_t *slab);
void bmslab_free(bmslab_t *slab, void *ptr);
void bmslab_destroy(bmslab_t *slab);
```
