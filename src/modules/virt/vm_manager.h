/* src/modules/virt/vm_manager.h */
#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _VmManager VmManager;

// 생성자 & 소멸자
VmManager* vm_manager_new(void);
void vm_manager_free(VmManager *self);

// [Async] 하이퍼바이저(QEMU) 연결
void vm_manager_connect_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data);
gboolean vm_manager_connect_finish(VmManager *self, GAsyncResult *res, GError **error);

// [Async] VM 목록 조회 (JSON 반환)
void vm_manager_list_domains_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data);
JsonNode* vm_manager_list_domains_finish(VmManager *self, GAsyncResult *res, GError **error);

G_END_DECLS

#endif // PURECVISOR_VM_MANAGER_H