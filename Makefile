CC		= gcc
AR		= ar
RANLIB	= ranlib


CFLAGS_RELEASE	= -Wall -Wextra -O2 -std=c11 -fPIC
CFLAGS_DEBUG	= -Wall -Wextra -O0 -g -pg -std=c11 -fPIC

BUILD_MODE ?= release

ifeq ($(BUILD_MODE), release)
	CFLAGS = $(CFLAGS_RELEASE)
else ifeq ($(BUILD_MODE), debug)
	CFLAGS = $(CFLAGS_DEBUG)
else
	$(error Unknown BUILD_MODE: $(BUILD_MODE). Use 'release' or 'debug')
endif

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
