
















#ifndef PURECVISOR_STORAGE_PRIVATE_H
#define PURECVISOR_STORAGE_PRIVATE_H

#include "purecvisor/storage.h"


typedef struct {
    bool (*create_vol)(StorageDriver *self, const char *name, size_t size);
    bool (*delete_vol)(StorageDriver *self, const char *name);
    StorageVolumeInfo* (*get_info)(StorageDriver *self, const char *name);
    void (*destructor)(StorageDriver *self);
} StorageVTable;


struct StorageDriver {
    const StorageVTable *vtable;
    char *pool_name;
    void *priv;
};

#endif