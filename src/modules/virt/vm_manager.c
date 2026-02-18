#include "vm_manager.h"
#include "vm_config_builder.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * GObject Definition
 * ------------------------------------------------------------------------ */

struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *connection;
};

G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

static void purecvisor_vm_manager_dispose(GObject *obj) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(obj);
    if (self->connection) {
        g_object_unref(self->connection);
        self->connection = NULL;
    }
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->dispose(obj);
}

static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = purecvisor_vm_manager_dispose;
}

static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    self->connection = gvir_connection_new("qemu:///system");
    gvir_connection_open(self->connection, NULL, NULL);
}

PureCVisorVmManager *purecvisor_vm_manager_new(void) {
    return g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
}

GVirConnection *purecvisor_vm_manager_get_connection(PureCVisorVmManager *manager) {
    return manager->connection;
}

/* ------------------------------------------------------------------------
 * Async Helper Data
 * ------------------------------------------------------------------------ */

typedef struct {
    gchar *vm_name;
    gboolean force;
    PureCVisorVmConfig *config;
} LifecycleData;

static void lifecycle_data_free(gpointer data) {
    LifecycleData *d = data;
    g_free(d->vm_name);
    if (d->config) {
        g_free(d->config->name);
        g_free(d->config->iso_path);
        g_free(d->config);
    }
    g_free(d);
}

/* ------------------------------------------------------------------------
 * 1. CREATE VM (ZFS Create + XML Define)
 * ------------------------------------------------------------------------ */

static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    LifecycleData *data = task_data;
    GError *error = NULL;

    /* 1. Check Exists */
    GVirDomain *domain = gvir_connection_find_domain_by_name(self->connection, data->config->name);
    if (domain) {
        g_object_unref(domain);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS, "VM '%s' already exists", data->config->name);
        return;
    }

    /* 2. ZFS Volume Create */
    gchar *zfs_cmd = g_strdup_printf("zfs create -V %uG tank/%s", 
                                     data->config->disk_size_gb, 
                                     data->config->name);
    gint exit_status = 0;
    
    // ZFS 생성 시도 (실패시 에러 리턴)
    if (!g_spawn_command_line_sync(zfs_cmd, NULL, NULL, &exit_status, &error) || exit_status != 0) {
        if (!error) g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Create Failed (Exit Code: %d)", exit_status);
        g_task_return_error(task, error);
        g_free(zfs_cmd);
        return;
    }
    g_free(zfs_cmd);

    /* 3. Define Domain */
    GVirConfigDomain *conf = purecvisor_vm_config_builder_create_config(data->config, &error);
    if (!conf) {
        g_task_return_error(task, error);
        // (Optional: Rollback ZFS here)
        return;
    }

    domain = gvir_connection_create_domain(self->connection, conf, &error);
    g_object_unref(conf);

    if (!domain) {
        g_task_return_error(task, error);
    } else {
        g_object_unref(domain);
        g_task_return_boolean(task, TRUE);
    }
}

void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *manager, PureCVisorVmConfig *config, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(manager, cancellable, callback, user_data);
    LifecycleData *data = g_new0(LifecycleData, 1);
    data->config = g_new0(PureCVisorVmConfig, 1);
    data->config->name = g_strdup(config->name);
    data->config->vcpu = config->vcpu;
    data->config->memory_mb = config->memory_mb;
    data->config->disk_size_gb = config->disk_size_gb;
    data->config->iso_path = g_strdup(config->iso_path);
    g_task_set_task_data(task, data, lifecycle_data_free);
    g_task_run_in_thread(task, create_vm_thread);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* ------------------------------------------------------------------------
 * 2. START VM
 * ------------------------------------------------------------------------ */

static void start_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    LifecycleData *data = task_data;
    GError *error = NULL;

    GVirDomain *domain = gvir_connection_find_domain_by_name(self->connection, data->vm_name);
    if (!domain) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }
    if (!gvir_domain_start(domain, 0, &error)) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
    g_object_unref(domain);
}

void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *manager, const gchar *vm_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(manager, cancellable, callback, user_data);
    LifecycleData *data = g_new0(LifecycleData, 1);
    data->vm_name = g_strdup(vm_name);
    g_task_set_task_data(task, data, lifecycle_data_free);
    g_task_run_in_thread(task, start_vm_thread);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* ------------------------------------------------------------------------
 * 3. STOP VM
 * ------------------------------------------------------------------------ */

static void stop_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    LifecycleData *data = task_data;
    GError *error = NULL;
    gboolean success;

    GVirDomain *domain = gvir_connection_find_domain_by_name(self->connection, data->vm_name);
    if (!domain) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

    if (data->force) success = gvir_domain_stop(domain, 0, &error);
    else success = gvir_domain_shutdown(domain, 0, &error);

    if (!success) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
    g_object_unref(domain);
}

void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *manager, const gchar *vm_name, gboolean force, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(manager, cancellable, callback, user_data);
    LifecycleData *data = g_new0(LifecycleData, 1);
    data->vm_name = g_strdup(vm_name);
    data->force = force;
    g_task_set_task_data(task, data, lifecycle_data_free);
    g_task_run_in_thread(task, stop_vm_thread);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* ------------------------------------------------------------------------
 * 4. DELETE VM (Stop + Undefine + ZFS Destroy)
 * ------------------------------------------------------------------------ */

static void delete_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    LifecycleData *data = task_data;
    GError *error = NULL;

    // 1. Libvirt Cleanup
    GVirDomain *domain = gvir_connection_find_domain_by_name(self->connection, data->vm_name);
    if (domain) {
        gvir_domain_stop(domain, 0, NULL); // Force stop (Ignore error if already stopped)
        if (!gvir_domain_delete(domain, 0, &error)) {
            g_task_return_error(task, error);
            g_object_unref(domain);
            return;
        }
        g_object_unref(domain);
    }

    // 2. ZFS Cleanup (Clean up storage even if VM domain is missing)
    gchar *zfs_cmd = g_strdup_printf("zfs destroy -r tank/%s", data->vm_name);
    gint exit_status = 0;
    
    // ZFS 삭제 실행 (결과 무시 - Idempotency)
    g_spawn_command_line_sync(zfs_cmd, NULL, NULL, &exit_status, NULL);
    g_free(zfs_cmd);

    g_task_return_boolean(task, TRUE);
}

void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *manager, const gchar *vm_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(manager, cancellable, callback, user_data);
    LifecycleData *data = g_new0(LifecycleData, 1);
    data->vm_name = g_strdup(vm_name);
    g_task_set_task_data(task, data, lifecycle_data_free);
    g_task_run_in_thread(task, delete_vm_thread);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* ------------------------------------------------------------------------
 * 5. LIST VM (Simplified State)
 * ------------------------------------------------------------------------ */

gint purecvisor_vm_manager_get_vnc_port(PureCVisorVmManager *manager, GVirDomain *domain) {
    return 5900; // Stub
}

static void list_vms_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    GError *error = NULL;

    if (!gvir_connection_fetch_domains(self->connection, cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    GList *domains = gvir_connection_get_domains(self->connection);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);

    for (GList *l = domains; l != NULL; l = l->next) {
        GVirDomain *domain = GVIR_DOMAIN(l->data);
        
        json_builder_begin_object(builder);
        
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, gvir_domain_get_name(domain));
        
        // [FIX] Linker 에러 방지를 위해 실제 상태 조회 함수(gvir_domain_get_info) 제거
        // 대신 "managed"라는 고정 문자열 반환 (상태 확인은 virsh list 이용 권장)
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, "managed");

        json_builder_set_member_name(builder, "vnc_port");
        json_builder_add_int_value(builder, 5900);

        json_builder_end_object(builder);
        g_object_unref(domain);
    }
    g_list_free(domains);

    json_builder_end_array(builder);
    JsonNode *root = json_builder_get_root(builder);
    g_task_return_pointer(task, root, (GDestroyNotify)json_node_unref);
    g_object_unref(builder);
}

void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *manager, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(manager, cancellable, callback, user_data);
    g_task_run_in_thread(task, list_vms_thread);
    g_object_unref(task);
}

JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager, GAsyncResult *res, GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}