/**
 * @file driver_p.h
 * @brief 스토리지 드라이버 내부 인터페이스 — 가상 함수 테이블 (private)
 *
 * == 설계 ==
 *   C에서 다형성을 구현하기 위한 VTable 패턴입니다.
 *   StorageDriver 기본 구조체에 StorageVTable 포인터를 가지며,
 *   각 드라이버(ZFS 등)가 자신의 VTable을 등록합니다.
 *
 * == 현재 상태 ==
 *   이 인터페이스는 초기 설계 단계에서 작성되었으며,
 *   실제 프로덕션 코드는 zfs_driver.c의 직접 함수 호출을 사용합니다.
 *   향후 다중 스토리지 백엔드(Ceph, LVM 등) 지원 시 활용 가능합니다.
 *
 * 주의: storage_impl.h와 StorageDriver 정의가 중복됩니다.
 *       실제 사용 시 하나로 통합해야 합니다.
 */
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