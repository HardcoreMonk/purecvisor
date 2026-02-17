#ifndef PURECVISOR_STORAGE_H
#define PURECVISOR_STORAGE_H

#include <glib.h>
#include <stdbool.h>

/**
 * @brief 스토리지 드라이버 Opaque Handle
 */
typedef struct StorageDriver StorageDriver;

/**
 * @brief 스토리지 볼륨 정보 (Arena에서 할당 권장)
 */
typedef struct {
    char *name;
    size_t capacity_bytes;
    size_t used_bytes;
    char *mount_point;
} StorageVolumeInfo;

/* --- 공용 API --- */

// 드라이버 인스턴스 생성 (ZFS 등)
StorageDriver* storage_driver_create(const char *type, const char *pool_name);

// VTable을 통한 다형성 호출
bool storage_volume_create(StorageDriver *self, const char *name, size_t size);
bool storage_volume_delete(StorageDriver *self, const char *name);
StorageVolumeInfo* storage_get_info(StorageDriver *self, const char *name);

void storage_driver_destroy(StorageDriver *self);

#endif // PURECVISOR_STORAGE_H