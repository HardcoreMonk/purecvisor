
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

[[nodiscard]] gboolean pcv_security_group_create(const gchar *name, const gchar *description);
gboolean   pcv_security_group_delete(const gchar *name);
JsonArray *pcv_security_group_list(void);
gboolean   pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
gboolean   pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
gboolean   pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name);
gboolean   pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);
void       pcv_security_group_sync_vm(const gchar *vm_name);
gboolean   pcv_security_group_vm_is_bound(const gchar *vm);
void       pcv_security_group_restore(void);

void pcv_security_group_resync_all(void);

void pcv_security_group_resync_timer_init(void);
void pcv_security_group_resync_timer_shutdown(void);
