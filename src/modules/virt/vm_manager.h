/* src/modules/virt/vm_manager.h */

#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>

G_BEGIN_DECLS

#define PURECVISOR_TYPE_VM_MANAGER (purecvisor_vm_manager_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorVmManager, purecvisor_vm_manager, PURECVISOR, VM_MANAGER, GObject)

/* 생성자: Connection 인자 추가됨 */
PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn);

/* Async Method Definitions (Phase 5 Signature) */

// Create: Config 객체 대신 개별 인자 사용
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb, // [Added]
                                           const gchar *iso_path,
                                           const gchar *network_bridge, // [Added]
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

// Start
void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error);

// Stop
void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error);

// Delete
void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

// List
void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

G_END_DECLS

#endif /* PURECVISOR_VM_MANAGER_H */