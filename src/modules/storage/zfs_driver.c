#include "storage_impl.h"
#include <stdio.h>
#include <stdlib.h>

/* Derived Class */
typedef struct {
    StorageDriver parent; // Inheritance
    char *zpool_name;     // Private Member
} ZfsDriver;

/* Helper Macro for Downcasting */
#define TO_ZFS(ptr) ((ZfsDriver*)(ptr))

/* --- Implementation --- */

static bool zfs_create_vol_impl(StorageDriver *self, const char *name, size_t size_mb) {
    ZfsDriver *zfs = TO_ZFS(self);
    g_autofree char *cmd = NULL;

    // 실제 명령 생성 (Log로 확인)
    cmd = g_strdup_printf("zfs create -V %zuM %s/%s", size_mb, zfs->zpool_name, name);
    g_info("[Storage/ZFS] Executing: %s", cmd);

    // TODO: Phase 2에서 g_spawn_async로 실제 실행
    return true;
}

static void zfs_dtor_impl(StorageDriver *self) {
    ZfsDriver *zfs = TO_ZFS(self);
    g_info("[Storage/ZFS] Shutting down driver for pool: %s", zfs->zpool_name);
    
    free(zfs->zpool_name);
    free(zfs->parent.driver_name);
    free(zfs);
}

/* VTable Binding */
static const StorageOps zfs_ops = {
    .create_vol = zfs_create_vol_impl,
    .dtor = zfs_dtor_impl,
};

/* --- Factory --- */
StorageDriver* storage_driver_new_zfs(const char *pool_name) {
    ZfsDriver *zfs = calloc(1, sizeof(ZfsDriver));
    zfs->parent.ops = &zfs_ops;
    zfs->parent.driver_name = strdup("zfs");
    zfs->zpool_name = strdup(pool_name);
    
    g_info("[Storage] ZFS Driver Initialized (Pool: %s)", pool_name);
    return (StorageDriver*)zfs;
}

/* --- Public Wrapper --- */
bool storage_create_vol(StorageDriver *self, const char *name, size_t size_mb) {
    if (!self || !self->ops || !self->ops->create_vol) return false;
    return self->ops->create_vol(self, name, size_mb);
}

void storage_destroy(StorageDriver *self) {
    if (self && self->ops && self->ops->dtor) self->ops->dtor(self);
}