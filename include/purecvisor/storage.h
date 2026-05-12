








































#ifndef PURECVISOR_STORAGE_H
#define PURECVISOR_STORAGE_H

#include <glib.h>
#include <stdbool.h>


typedef struct StorageDriver StorageDriver;


StorageDriver* storage_driver_new_zfs(const char *pool_name);


bool storage_create_vol(StorageDriver *self, const char *name, size_t size_mb);
void storage_destroy(StorageDriver *self);

#endif