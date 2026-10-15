#ifndef IFUSE_POOL_H
#define IFUSE_POOL_H

/**
 * Fixed-size pool allocator for I-Fuse tables.
 *
 * The pool makes one calloc() call at init. It then allocates only by
 * bump pointer and free list. It never grows or relocates its chunk.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char*  arena;
    size_t capacity;
    size_t bump;
    size_t elem_size;
    void*  free_list;
} IfusePool;

static inline void ifuse_pool_init(IfusePool* a, size_t elem_size) {
    memset(a, 0, sizeof(*a));
    a->elem_size = elem_size < sizeof(void*) ? sizeof(void*) : elem_size;
}

static inline bool ifuse_pool_init_fixed(IfusePool* a, size_t elem_size,
                                                size_t max_elems) {
    ifuse_pool_init(a, elem_size);
    if (max_elems == 0U) {
        return false;
    }
    a->capacity = max_elems;
    a->arena    = (char*)calloc(max_elems, a->elem_size);
    return a->arena != NULL;
}

static inline void* ifuse_pool_get(IfusePool* a) {
    if (a->free_list) {
        void* out      = a->free_list;
        a->free_list = *(void**)out;
        return out;
    }
    if (!a->arena || a->bump >= a->capacity) {
        return NULL;
    }
    void* out = a->arena + (a->bump++ * a->elem_size);
    return out;
}

static inline void ifuse_pool_put(IfusePool* a, void* ptr) {
    if (!ptr) {
        return;
    }
    *(void**)ptr = a->free_list;
    a->free_list = ptr;
}

#endif /* IFUSE_POOL_H */