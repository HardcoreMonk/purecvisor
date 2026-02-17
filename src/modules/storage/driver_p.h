#ifndef PURECVISOR_STORAGE_PRIVATE_H
#define PURECVISOR_STORAGE_PRIVATE_H

#include "purecvisor/storage.h"

// 스토리지 가상 함수 테이블
typedef struct {
    bool (*create_vol)(StorageDriver *self, const char *name, size_t size);
    bool (*delete_vol)(StorageDriver *self, const char *name);
    StorageVolumeInfo* (*get_info)(StorageDriver *self, const char *name);
    void (*destructor)(StorageDriver *self);
} StorageVTable;

// 기본 드라이버 구조체
struct StorageDriver {
    const StorageVTable *vtable;
    char *pool_name;
    void *priv; // 각 드라이버별 전용 데이터 (e.g., ZFS 전용 컨텍스트)
};

#endif