CC		= gcc
CFLAGS	= -Wall -Wextra -O2 -std=c11 -fPIC
AR		= ar
RANLIB	= ranlib

STATIC_LIB = libbmslab.a
SHARED_LIB = libbmslab.so

all: $(STATIC_LIB) $(SHARED_LIB)

$(STATIC_LIB): bmslab.o
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SHARED_LIB): bmslab.o
	$(CC) -shared -o $@ $^

bmslab.o: bmslab.c bmslab.h
	$(CC) $(CFLAGS) -c bmslab.c

clean:
	rm -f *.o $(STATIC_LIB) $(SHARED_LIB)
