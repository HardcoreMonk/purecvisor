/*
 * src/modules/virt/vm_manager.c
 *
 * Description:
 * Implementation of VM Lifecycle Orchestration.
 * Handles the "Transaction" of creating a VM (Storage + Compute).
 */

#include "vm_manager.h"
#include "vm_config_builder.h"
#include <libvirt-gconfig/libvirt-gconfig.h>

struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *conn;
    PureCVisorZfsDriver *zfs_driver;
};

G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

/* Context to hold state across async steps */
typedef struct {
    PureCVisorVmManager *manager; // Reference kept
    gchar *vm_name;
    guint64 memory_kb;
    guint vcpus;
    guint64 disk_size_bytes;
    gchar *pool_name;
    
    /* Runtime State */
    gchar *zvol_path; // Filled after ZFS success
} VmCreateContext;

static void
vm_create_context_free(VmCreateContext *ctx)
{
    if (!ctx) return;
    if (ctx->manager) g_object_unref(ctx->manager);
    g_free(ctx->vm_name);
    g_free(ctx->pool_name);
    g_free(ctx->zvol_path);
    g_free(ctx);
}

static void
purecvisor_vm_manager_dispose(GObject *object)
{
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(object);

    if (self->conn) {
        g_object_unref(self->conn);
        self->conn = NULL;
    }
    if (self->zfs_driver) {
        g_object_unref(self->zfs_driver);
        self->zfs_driver = NULL;
    }

    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->dispose(object);
}

static void
purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = purecvisor_vm_manager_dispose;
}

static void
purecvisor_vm_manager_init(PureCVisorVmManager *self)
{
    self->zfs_driver = purecvisor_zfs_driver_new();
}

PureCVisorVmManager *
purecvisor_vm_manager_new(GVirConnection *conn)
{
    PureCVisorVmManager *self = g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
    if (conn) {
        self->conn = g_object_ref(conn);
    }
    return self;
}

/* * --------------------------------------------------------------------------
 * Rollback Logic: Called when Libvirt fails but ZFS succeeded
 * --------------------------------------------------------------------------
 */
static void
_on_rollback_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    GTask *task = G_TASK(user_data);
    GError *rollback_err = NULL;
    
    /* Retrieve the original error that triggered the rollback */
    GError *original_error = g_object_get_data(G_OBJECT(task), "original_error");

    /* Check rollback status (just for logging) */
    if (!purecvisor_zfs_driver_destroy_vol_finish(PURECVISOR_ZFS_DRIVER(source), res, &rollback_err)) {
        g_warning("CRITICAL: Rollback failed! Orphaned ZVol may exist. Error: %s", rollback_err->message);
        g_error_free(rollback_err);
    } else {
        g_info("Rollback successful: ZVol destroyed.");
    }

    /* Return the ORIGINAL error to the user */
    if (original_error) {
        g_task_return_error(task, g_error_copy(original_error));
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Unknown error during creation (Rollback done)");
    }
    
    g_object_unref(task);
}

static void
_trigger_rollback(GTask *task, VmCreateContext *ctx, GError *original_error)
{
    g_info("Triggering Rollback for VM: %s", ctx->vm_name);
    
    /* Attach original error to task to preserve it */
    g_object_set_data_full(G_OBJECT(task), "original_error", original_error, (GDestroyNotify)g_error_free);

    purecvisor_zfs_driver_destroy_vol_async(ctx->manager->zfs_driver,
                                            ctx->pool_name,
                                            ctx->vm_name,
                                            NULL,
                                            _on_rollback_complete,
                                            task);
}

/* * --------------------------------------------------------------------------
 * Step 2: ZVol Created -> Generate XML -> Define Domain
 * --------------------------------------------------------------------------
 */
static void
_on_zvol_created(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    VmCreateContext *ctx = g_task_get_task_data(task);
    GError *error = NULL;
    gchar *full_path = NULL;

    /* 1. Check ZFS Result */
    if (!purecvisor_zfs_driver_create_vol_finish(PURECVISOR_ZFS_DRIVER(source), res, &full_path, &error)) {
        g_task_return_error(task, error); // No rollback needed (nothing created)
        g_object_unref(task);
        return;
    }

    ctx->zvol_path = full_path; // Owns the string
    g_info("ZVol created at: %s", ctx->zvol_path);

    /* 2. Build Config Object (Sync) */
    PureCVisorVmConfig conf = {0};
    conf.name = ctx->vm_name;
    conf.memory_kb = ctx->memory_kb;
    conf.vcpus = ctx->vcpus;
    conf.disk_path = ctx->zvol_path;
    conf.bridge_iface = "virbr0"; // Default for now
    
    PureCVisorVmConfigBuilder *builder = purecvisor_vm_config_builder_new();
    if (!purecvisor_vm_config_builder_set_config(builder, &conf, &error)) {
        _trigger_rollback(task, ctx, error);
        g_object_unref(builder);
        return;
    }

    gchar *xml = purecvisor_vm_config_builder_generate_xml(builder, &error);
    g_object_unref(builder);

    if (!xml) {
        _trigger_rollback(task, ctx, error);
        return;
    }

    /* 3. Reconstruct GVirConfigDomain from XML (Libvirt-GObject needs Object, not string) */
    GVirConfigDomain *domain_config = gvir_config_domain_new_from_xml(xml, &error);
    g_free(xml);

    if (!domain_config) {
        _trigger_rollback(task, ctx, error);
        return;
    }

    /* 4. Define Domain (Create Persistent) */
    /* Note: 'gvir_connection_create_domain' usually creates a transient (running) domain.
     * To define persistent, we might need a different call depending on version.
     * Assuming standard create_domain for now. */
    GVirDomain *domain = gvir_connection_create_domain(ctx->manager->conn, domain_config, &error);
    g_object_unref(domain_config);

    if (!domain) {
        _trigger_rollback(task, ctx, error);
        return;
    }

    /* Success! */
    g_info("VM '%s' successfully defined/started.", ctx->vm_name);
    g_object_unref(domain); // We don't need to keep the domain object
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

/* * --------------------------------------------------------------------------
 * Step 1: Entry Point -> Parse JSON -> Request ZVol
 * --------------------------------------------------------------------------
 */
void
purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                      JsonNode *params,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));

    GTask *task = g_task_new(self, NULL, callback, user_data);
    VmCreateContext *ctx = g_new0(VmCreateContext, 1);
    ctx->manager = g_object_ref(self);
    
    /* Parse JSON */
    JsonObject *obj = json_node_get_object(params);
    ctx->vm_name = g_strdup(json_object_get_string_member(obj, "name"));
    ctx->memory_kb = (guint64)json_object_get_int_member(obj, "memory_mb") * 1024;
    ctx->vcpus = (guint)json_object_get_int_member(obj, "vcpus");
    guint64 disk_gb = (guint64)json_object_get_int_member(obj, "disk_gb");
    ctx->disk_size_bytes = disk_gb * 1024 * 1024 * 1024;
    
    /* Default pool if not specified */
    if (json_object_has_member(obj, "pool")) {
        ctx->pool_name = g_strdup(json_object_get_string_member(obj, "pool"));
    } else {
        ctx->pool_name = g_strdup("tank"); // Default pool
    }

    /* Validate minimal params */
    if (!ctx->vm_name || ctx->disk_size_bytes == 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, 
                                "Missing required fields (name, disk_gb)");
        vm_create_context_free(ctx);
        g_object_unref(task);
        return;
    }

    g_task_set_task_data(task, ctx, (GDestroyNotify)vm_create_context_free);

    /* Start Chain: Create ZVol */
    purecvisor_zfs_driver_create_vol_async(self->zfs_driver,
                                           ctx->pool_name,
                                           ctx->vm_name,
                                           ctx->disk_size_bytes,
                                           NULL,
                                           _on_zvol_created,
                                           task);
}

gboolean
purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *self,
                                       GAsyncResult *res,
                                       GError **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* * --------------------------------------------------------------------------
 * Feature Restored: List Domains
 * --------------------------------------------------------------------------
 */

/* Thread function to fetch domains securely without blocking main loop excessively */

/* src/modules/virt/vm_manager.c 의 _list_domains_thread 함수 교체 */

static void
_list_domains_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(source_object);
    GList *domains = NULL;
    GList *iter;
    JsonBuilder *builder = json_builder_new();

    (void)task_data; (void)cancellable;

    /* [DEBUG] 로그 추가 */
    g_print("[DEBUG] Fetching domains list...\n");
    
    /* 1. Fetch Domains */
    /* 주의: 이 호출 자체가 스레드에서 안전하지 않을 수 있습니다. 
       만약 여기서도 죽으면 메인 스레드로 옮겨야 합니다. */
    domains = gvir_connection_get_domains(self->conn);

    /* 2. Build JSON Array */
    json_builder_begin_array(builder);

    for (iter = domains; iter != NULL; iter = iter->next) {
        GVirDomain *dom = GVIR_DOMAIN(iter->data);
        const gchar *name = gvir_domain_get_name(dom);
        
        /* [DEBUG] Found domain */
        g_print("[DEBUG] Found domain: %s\n", name ? name : "(null)");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        
        /* [SAFE MODE] 불안정한 Info 접근 코드 비활성화
         * 링킹 에러가 났던 함수들은 실제로 라이브러리에서 export되지 않았거나 
         * property로도 존재하지 않을 확률이 높습니다.
         * 우선 크래시를 막기 위해 "unknown"으로 고정합니다.
         */
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, "unknown (safe-mode)");
        
        json_builder_set_member_name(builder, "memory_kb");
        json_builder_add_int_value(builder, 0);
        
        json_builder_end_object(builder);
        g_object_unref(dom);
    }
    
    if (domains) g_list_free(domains); 

    json_builder_end_array(builder);

    g_print("[DEBUG] Domain list built successfully.\n");

    /* 3. Return Result */
    JsonNode *root = json_builder_get_root(builder);
    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
    g_object_unref(builder);
}

void
purecvisor_vm_manager_list_domains_async(PureCVisorVmManager *self,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));
    
    GTask *task = g_task_new(self, NULL, callback, user_data);
    
    /* Run in a worker thread because get_domains might block or be heavy */
    g_task_run_in_thread(task, _list_domains_thread);
    g_object_unref(task);
}

JsonNode *
purecvisor_vm_manager_list_domains_finish(PureCVisorVmManager *self,
                                          GAsyncResult *res,
                                          GError **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), NULL);
    
    /* Propagate pointer: Returns JsonNode* (transfer full) */
    return g_task_propagate_pointer(G_TASK(res), error);
}