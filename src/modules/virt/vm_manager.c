/* src/modules/virt/vm_manager.c */
#include "vm_manager.h"
#include <libvirt-gobject/libvirt-gobject.h>

struct _VmManager {
    GVirConnection *conn;
};

VmManager* vm_manager_new(void) {
    VmManager *self = g_new0(VmManager, 1);
    return self;
}

void vm_manager_free(VmManager *self) {
    if (!self) return;
    if (self->conn) g_object_unref(self->conn);
    g_free(self);
}

/* --- Connection Logic --- */

static void on_gvir_open_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GVirConnection *conn = GVIR_CONNECTION(source_object);
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    if (gvir_connection_open_finish(conn, res, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
    g_object_unref(task);
}

void vm_manager_connect_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data) {
    if (!self->conn) {
        self->conn = gvir_connection_new("qemu:///system");
    }

    // [핵심] source는 NULL로 하되, self는 task_data에 저장하지 않아도 됨
    // (연결 완료 후 self를 쓸 일이 없으므로)
    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    
    gvir_connection_open_async(self->conn, NULL, on_gvir_open_finished, task);
}

gboolean vm_manager_connect_finish(VmManager *self, GAsyncResult *res, GError **error) {
    (void)self; 
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --- List Domains Logic (Fix Segfault) --- */

static void on_gvir_fetch_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GVirConnection *conn = GVIR_CONNECTION(source_object);
    GTask *task = G_TASK(user_data);
    
    // [Fix] source_object 대신 task_data에서 self 복구
    VmManager *self = (VmManager *)g_task_get_task_data(task); 
    GError *error = NULL;

    if (!gvir_connection_fetch_domains_finish(conn, res, &error)) {
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    // 이제 self->conn 접근이 안전함
    GList *domains = gvir_connection_get_domains(self->conn);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);

    for (GList *iter = domains; iter; iter = iter->next) {
        GVirDomain *dom = GVIR_DOMAIN(iter->data);
        const gchar *name = gvir_domain_get_name(dom);
        
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);
        
        g_object_unref(dom);
    }
    json_builder_end_array(builder);
    g_list_free(domains);

    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);

    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
    g_object_unref(task);
}

void vm_manager_list_domains_async(VmManager *self, GAsyncReadyCallback callback, gpointer user_data) {
    if (!self->conn) {
        g_task_report_new_error(self, callback, user_data, vm_manager_list_domains_async, 
                                G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Hypervisor not connected");
        return;
    }

    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    
    // [핵심] self 포인터를 task 데이터로 탑재 (DestroyNotify는 NULL)
    // 이렇게 하면 내부 콜백에서 안전하게 꺼내 쓸 수 있음
    g_task_set_task_data(task, self, NULL);

    gvir_connection_fetch_domains_async(self->conn, NULL, on_gvir_fetch_finished, task);
}

JsonNode* vm_manager_list_domains_finish(VmManager *self, GAsyncResult *res, GError **error) {
    (void)self;
    return g_task_propagate_pointer(G_TASK(res), error);
}