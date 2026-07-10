/**
 * @file storage_impl.h
 * @brief 스토리지 드라이버 구현 인터페이스 — VTable + 기본 클래스
 *
 * == 설계 ==
 *   StorageOps: 드라이버별 가상 함수 테이블 (create_vol, dtor)
 *   StorageDriver: 기본 클래스 (ops 포인터 + driver_name)
 *
 * == 현재 상태 ==
 *   driver_p.h와 유사한 역할이며, 초기 설계 단계의 인터페이스입니다.
 *   실제 코드는 zfs_driver.c가 직접 함수를 제공합니다.
 */
#ifndef PURECVISOR_STORAGE_IMPL_H
#define PURECVISOR_STORAGE_IMPL_H

#include "purecvisor/storage.h"

/* VTable Definition */
typedef struct {
    bool (*create_vol)(StorageDriver *self, const char *name, size_t size_mb);
    void (*dtor)(StorageDriver *self);
} StorageOps;

/* Base Class */
struct StorageDriver {
    const StorageOps *ops;
    char *driver_name;
};

#endif