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