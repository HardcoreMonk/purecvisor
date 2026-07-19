
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "purecvisor/core.h"

_Static_assert(sizeof(void*) == 8, "PureCVisor requires 64-bit system");

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t offset;
} Arena;

Arena* arena_create(size_t size) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->buffer = malloc(size);
    if (!a->buffer) {
        free(a);
        return NULL;
    }
    a->size = size;
    a->offset = 0;
    return a;
}

void* arena_alloc(Arena *a, size_t size) {

    size_t padding = (8 - (a->offset % 8)) % 8;

    if (a->offset + padding + size > a->size) return NULL;

    a->offset += padding;

    void *ptr = &a->buffer[a->offset];

    a->offset += size;

    memset(ptr, 0, size);
    return ptr;
}

void arena_reset(Arena *a) {
    if (a) a->offset = 0;
}

void arena_destroy(Arena *a) {
    if (a) {
        free(a->buffer);
        free(a);
    }
}
