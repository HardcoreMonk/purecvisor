#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "purecvisor/core.h" // 또는 필요한 헤더

// 64비트 시스템 확인 (Static Assert)
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
    // 8바이트 메모리 정렬 (Alignment)
    size_t padding = (8 - (a->offset % 8)) % 8;
    if (a->offset + padding + size > a->size) return NULL;
    
    a->offset += padding;
    void *ptr = &a->buffer[a->offset];
    a->offset += size;
    
    // 할당된 메모리를 0으로 초기화 (Safety)
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