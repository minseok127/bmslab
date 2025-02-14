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

