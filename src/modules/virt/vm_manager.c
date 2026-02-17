/* src/modules/virt/vm_manager.c */
#include "vm_manager.h"
#include <libvirt-gobject/libvirt-gobject.h>

struct _VmManager {
    GVirConnection *conn; // Libvirt 연결 객체
};

VmManager* vm_manager_new(void) {
    VmManager *self = g_new0(VmManager, 1);
    // conn은 connect_async 시점에 생성
    return self;
}

void vm_manager_free(VmManager *self) {
    if (!self) return;
    if (self->conn) {
        g_object_unref(self->conn);
    }
    g_free(self);
}

/* --- Connection Logic --- */

void vm_manager_connect_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data) {
    // 1. QEMU System 연결 객체 생성
    if (!self->conn) {
        self->conn = gvir_connection_new("qemu:///system");
    }

    // 2. 비동기 연결 시작
    gvir_connection_open_async(self->conn, NULL, callback, user_data);
}

gboolean vm_manager_connect_finish(VmManager *self, GAsyncResult *res, GError **error) {
    return gvir_connection_open_finish(self->conn, res, error);
}

/* --- List Domains Logic --- */

void vm_manager_list_domains_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data) {
    if (!self->conn) {
        g_task_report_new_error(self, callback, user_data, vm_manager_list_domains_async, 
                                G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Hypervisor not connected");
        return;
    }

    // 도메인 목록 Fetch (정보 갱신)
    gvir_connection_fetch_domains_async(self->conn, NULL, callback, user_data);
}

JsonNode* vm_manager_list_domains_finish(VmManager *self, GAsyncResult *res, GError **error) {
    // 1. Fetch 결과 확인
    if (!gvir_connection_fetch_domains_finish(self->conn, res, error)) {
        return NULL;
    }

    // 2. 캐시된 도메인 리스트 가져오기
    GList *domains = gvir_connection_get_domains(self->conn);
    
    // 3. JSON 변환
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);

    for (GList *iter = domains; iter; iter = iter->next) {
        GVirDomain *dom = GVIR_DOMAIN(iter->data);
        const gchar *name = gvir_domain_get_name(dom);
        // UUID 등 추가 정보 가져오기 가능
        
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        // 상태 정보 등은 추후 추가
        json_builder_end_object(builder);
        
        g_object_unref(dom);
    }
    json_builder_end_array(builder);
    g_list_free(domains);

    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}