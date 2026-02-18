/* src/modules/virt/vm_manager.h */
#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "../storage/zfs_driver.h"


#include "vm_types.h" // 공통 타입 포함

G_BEGIN_DECLS

/* ------------------------------------------------------------------------
 * Type Definitions
 * ------------------------------------------------------------------------ */
#define PURECVISOR_TYPE_VM_MANAGER (purecvisor_vm_manager_get_type())
G_DECLARE_FINAL_TYPE(PureCVisorVmManager, purecvisor_vm_manager, PURECVISOR, VM_MANAGER, GObject)

/* ------------------------------------------------------------------------
 * Method Prototypes
 * ------------------------------------------------------------------------ */

PureCVisorVmManager *purecvisor_vm_manager_new(void);
GVirConnection *purecvisor_vm_manager_get_connection(PureCVisorVmManager *manager);

/* Async Functions */
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *manager,
                                           PureCVisorVmConfig *config,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *manager,
                                          const gchar *vm_name,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error);

void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *manager,
                                         const gchar *vm_name,
                                         gboolean force,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error);

void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *manager,
                                           const gchar *vm_name,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *manager,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

/* Helper */
gint purecvisor_vm_manager_get_vnc_port(PureCVisorVmManager *manager,
                                        GVirDomain *domain);

G_END_DECLS

#endif /* PURECVISOR_VM_MANAGER_H */
