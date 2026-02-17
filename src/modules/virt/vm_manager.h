/* src/modules/virt/vm_manager.h */
#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "../storage/zfs_driver.h"

G_BEGIN_DECLS

#define PURECVISOR_TYPE_VM_MANAGER (purecvisor_vm_manager_get_type())
G_DECLARE_FINAL_TYPE(PureCVisorVmManager, purecvisor_vm_manager, PURECVISOR, VM_MANAGER, GObject)

/* [FIX 1] 호환성을 위한 Typedef 추가 */
typedef PureCVisorVmManager VmManager;

/* [FIX 2] 생성자 시그니처 변경 (이미 반영되어 있음, 확인용) */

PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn);

/* --- Phase 3: Create VM --- */
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           JsonNode *params,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *self,
                                                GAsyncResult *res,
                                                GError **error);

/* --- Restored: List VMs --- */
void purecvisor_vm_manager_list_domains_async(PureCVisorVmManager *self,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

JsonNode *purecvisor_vm_manager_list_domains_finish(PureCVisorVmManager *self,
                                                    GAsyncResult *res,
                                                    GError **error);

G_END_DECLS

#endif /* PURECVISOR_VM_MANAGER_H */