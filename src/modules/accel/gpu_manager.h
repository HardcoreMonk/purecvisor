
































#ifndef PURECVISOR_GPU_MANAGER_H
#define PURECVISOR_GPU_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS


void        pcv_gpu_init(void);

void        pcv_gpu_shutdown(void);


JsonArray  *pcv_gpu_list(void);


JsonObject *pcv_gpu_info(const gchar *pci_addr);


JsonArray  *pcv_gpu_vgpu_types(const gchar *pci_addr);


gboolean    pcv_gpu_vgpu_create(const gchar *pci_addr, const gchar *type,
                                 gchar **uuid_out, GError **error);


gboolean    pcv_gpu_vgpu_delete(const gchar *uuid, GError **error);


JsonArray  *pcv_gpu_vgpu_list(void);


gboolean    pcv_gpu_attach(const gchar *vm_name, const gchar *pci_addr, GError **error);


gboolean    pcv_gpu_detach(const gchar *vm_name, const gchar *pci_addr, GError **error);

G_END_DECLS

#endif
