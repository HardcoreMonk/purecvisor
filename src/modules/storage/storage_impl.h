#ifndef PURECVISOR_STORAGE_IMPL_H
#define PURECVISOR_STORAGE_IMPL_H

#include <purecvisor/storage.h>

/* [VTable Pattern]
 * C++의 virtual function table과 동일한 원리입니다.
 */
typedef struct {
    bool (*create_vol)(StorageDriver *self, const char *name, size_t size_mb);
    void (*dtor)(StorageDriver *self); // Destructor
} StorageOps;

/* [Base Class]
 * 모든 드라이버 구조체는 이 구조체를 멤버로 포함해야 합니다.
 */
struct StorageDriver {
    const StorageOps *ops; // 가상 함수 테이블 포인터
    char *driver_name;     // 공통 속성: 드라이버 이름 (예: "zfs", "lvm")
};

#endif