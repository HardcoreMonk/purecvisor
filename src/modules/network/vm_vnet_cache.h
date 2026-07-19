
#pragma once
#include <glib.h>

void        pcv_vm_vnet_cache_put(const gchar *vm, GPtrArray *vnets);

GPtrArray  *pcv_vm_vnet_cache_get(const gchar *vm);
void        pcv_vm_vnet_cache_evict(const gchar *vm);
