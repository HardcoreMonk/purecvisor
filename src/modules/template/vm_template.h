
#ifndef PCV_VM_TEMPLATE_H
#define PCV_VM_TEMPLATE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar  *name;
    gint    vcpu;
    gint    memory_mb;
    gint    disk_gb;
    gchar  *os_variant;
    gchar  *iso_path;
    gchar  *network_bridge;
    gchar  *cloud_init_user_data;
    gchar  *description;
} PcvVmTemplate;

void pcv_vm_template_init(void);

void pcv_vm_template_shutdown(void);

gboolean pcv_vm_template_create(PcvVmTemplate *tmpl, GError **error);

gboolean pcv_vm_template_delete(const gchar *name, GError **error);

PcvVmTemplate *pcv_vm_template_get(const gchar *name);

GPtrArray *pcv_vm_template_list(void);

void pcv_vm_template_free(PcvVmTemplate *t);

G_END_DECLS

#endif
