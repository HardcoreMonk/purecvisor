#include "driver_p.h"
#include <stdio.h>

// ZFS 전용 컨텍스트
typedef struct {
    int last_exit_status;
    GError *last_error;
} ZfsContext;

/* --- 내부 구현 함수 (Static) --- */

static bool zfs_create_vol(StorageDriver *self, const char *name, size_t size) {
    bool success = false;
    gchar *cmd = NULL;
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    GError *error = NULL;

    // 예: zfs create -V 10G pool/vm-disk-01
    cmd = g_strdup_printf("zfs create -V %zuB %s/%s", size, self->pool_name, name);
    
    // Phase 1: 우선 동기 실행 후 Phase 2에서 g_spawn_async_with_pipes로 고도화
    if (!g_spawn_command_line_sync(cmd, &stdout_buf, &stderr_buf, NULL, &error)) {
        g_warning("ZFS Create Failed: %s", error->message);
        goto out;
    }

    success = true;
    g_info("ZFS Volume created: %s/%s", self->pool_name, name);

out:
    g_free(cmd);
    g_free(stdout_buf);
    g_free(stderr_buf);
    if (error) g_error_free(error);
    return success;
}

static void zfs_destructor(StorageDriver *self) {
    if (!self) return;
    g_free(self->pool_name);
    g_free(self->priv);
    g_free(self);
    g_debug("ZFS Driver destroyed.");
}

// ZFS 전용 VTable 정의
static const StorageVTable zfs_vtable = {
    .create_vol = zfs_create_vol,
    .delete_vol = NULL, // TODO: 구현 예정
    .get_info   = NULL, // TODO: 구현 예정
    .destructor = zfs_destructor
};

/* --- Factory Function --- */

StorageDriver* storage_driver_create_zfs(const char *pool_name) {
    StorageDriver *driver = g_malloc0(sizeof(StorageDriver));
    ZfsContext *ctx = g_malloc0(sizeof(ZfsContext));

    driver->vtable = &zfs_vtable;
    driver->pool_name = g_strdup(pool_name);
    driver->priv = ctx;

    return driver;
}

/* --- Public Wrapper (Dynamic Dispatch) --- */

bool storage_volume_create(StorageDriver *self, const char *name, size_t size) {
    g_return_val_if_fail(self && self->vtable->create_vol, false);
    return self->vtable->create_vol(self, name, size);
}

void storage_driver_destroy(StorageDriver *self) {
    if (self && self->vtable->destructor) {
        self->vtable->destructor(self);
    }
}