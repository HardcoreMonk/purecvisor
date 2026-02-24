/*
 * PureCVisor Engine - VM Manager Module
 * Phase 5: Advanced Lifecycle & Monitoring
 *
 * Capabilities:
 * - Async VM Management (Create, Start, Stop, Delete) with GTask.
 * - Live State Inspection via ID check (Linker-safe).
 * - Dynamic VNC Port Parsing via XML Regex.
 * - ZFS Storage Orchestration.
 */

#include "vm_manager.h"
#include "vm_config_builder.h"
#include "../storage/zfs_driver.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *conn;
};

G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

/* --------------------------------------------------------------------------
 * [Helper] Live XML 파싱을 통한 VNC 포트 추출
 * -------------------------------------------------------------------------- */
static gint _extract_vnc_port_from_domain(GVirDomain *dom) {
    GError *err = NULL;
    GVirConfigDomain *config = NULL;
    gchar *xml_data = NULL;
    gint port = -1;

    // 실행 중인 도메인의 Live Config 가져오기 (Flag: 0 -> Current/Live)
    config = gvir_domain_get_config(dom, 0, &err);
    if (err) {
        // VM이 꺼져있거나 Config를 가져올 수 없는 경우 무시
        g_error_free(err);
        return -1;
    }

    xml_data = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(config));
    g_object_unref(config);

    if (!xml_data) return -1;

    // XML을 가져온 후 Regex 부분만 교체:
    // "port='(\d+)'" 패턴을 찾되, 앞부분에 "<graphics"가 있는지 확인하는 것이 정석이나,
    // libvirt XML에서 port 속성을 가진 주요 태그는 graphics와 serial/console 임.
    // 간단하고 강력하게: "graphics type='vnc'" 가 포함된 라인 주변의 port를 찾는 복잡한 로직 대신,
    // 전체 XML에서 "port='(\d+)'"를 찾되, VNC 포트 범위(5900~)인 것을 찾는 휴리스틱 사용 가능.
    // 또는, 속성 순서에 유연한 Regex 사용:
    
    // 패턴: <graphics [^>]*port='(\d+)'
    GRegex *regex = g_regex_new("<graphics[^>]+port='(\\d+)'",
                                G_REGEX_CASELESS | G_REGEX_MULTILINE, 0, NULL);
    
    GMatchInfo *match_info;
    if (g_regex_match(regex, xml_data, 0, &match_info)) {
        gchar *port_str = g_match_info_fetch(match_info, 1);
        if (port_str) {
            port = (gint)g_ascii_strtoll(port_str, NULL, 10);
            g_free(port_str);
        }
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);
    g_free(xml_data);

    return port;
}

/* --------------------------------------------------------------------------
 * [GObject] Initialization
 * -------------------------------------------------------------------------- */
static void purecvisor_vm_manager_finalize(GObject *object) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(object);
    if (self->conn) {
        g_object_unref(self->conn);
    }
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->finalize(object);
}

static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_vm_manager_finalize;
}

static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    self->conn = NULL;
}

PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn) {
    PureCVisorVmManager *self = g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
    if (conn) {
        self->conn = g_object_ref(conn);
    }
    return self;
}

/* --------------------------------------------------------------------------
 * [Async Task] Create VM
 * -------------------------------------------------------------------------- */
typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
    gint vcpu;
    gint ram_mb;
    gint disk_size_gb; // [Added]
    gchar *disk_path; // ZVol path usually
    gchar *iso_path;
    gchar *network_bridge; // [Added]
} CreateVmTaskData;

static void create_vm_task_data_free(CreateVmTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data->disk_path);
    g_free(data->iso_path);
    g_free(data->network_bridge); // [Added]
    g_free(data);
}

// =================================================================
// [워커 스레드] ZFS 동적 할당 및 XML 뼈대/SCSI 컨트롤러 이식 엔진
// =================================================================
static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    CreateVmTaskData *data = (CreateVmTaskData *)task_data;
    GError *error = NULL;
    
    // 1. ZFS ZVOL 자동 프로비저닝 (최소 50GB 보장)
    gint final_disk_size = (data->disk_size_gb > 0) ? data->disk_size_gb : 50;
    gchar *zvol_name = g_strdup_printf("rpool/vms/%s", data->name);
    gchar *zvol_dev = g_strdup_printf("/dev/zvol/%s", zvol_name);
    
    // zfs create -V 50G tank/vms/가상머신이름
    gchar *zfs_cmd = g_strdup_printf("zfs create -V %dG %s", final_disk_size, zvol_name);
    gint exit_status = 0;
    gchar *std_err = NULL; // 🚀 신규: ZFS의 진짜 에러(stderr)를 잡을 그물

    if (!g_spawn_command_line_sync(zfs_cmd, NULL, &std_err, &exit_status, &error) || exit_status != 0) {
        gchar *err_msg = error ? error->message : (std_err ? g_strstrip(std_err) : "Unknown ZFS error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Provisioning Failed: %s", err_msg);
        
        if (error) g_error_free(error);
        if (std_err) g_free(std_err);
        g_free(zvol_name); g_free(zvol_dev); g_free(zfs_cmd);
        return;
    }
    if (std_err) g_free(std_err); // 성공 시 그물 해제
    

    // 2. VM 뼈대(XML) 조립 및 방금 만든 ZVOL 마운트
    PureCVisorVmConfig *config = purecvisor_vm_config_new(data->name, data->vcpu, data->ram_mb);
    
    // 🚀 깎아낸 ZVOL 블록 디바이스 경로를 메인 디스크로 장착!
    purecvisor_vm_config_set_disk(config, zvol_dev); 
    
    if (data->iso_path) purecvisor_vm_config_set_iso(config, data->iso_path);
    if (data->network_bridge) purecvisor_vm_config_set_network_bridge(config, data->network_bridge);

    GVirConfigDomain *domain_config = purecvisor_vm_config_build(config);
    gchar *raw_xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(domain_config));

    // 3. [흑마법] 통곡의 벽 파괴자: virtio-scsi 컨트롤러 척추 이식
    gchar *scsi_xml = "<controller type='scsi' index='0' model='virtio-scsi'/>\n  </devices>";
    gchar **xml_parts = g_strsplit(raw_xml, "</devices>", 2);
    gchar *final_xml = g_strjoinv(scsi_xml, xml_parts);

    // 4. 네이티브 Libvirt API를 통한 최종 등록
    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = virDomainDefineXML(conn, final_xml);
    
    if (!dom) {
        virErrorPtr libvirt_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
            "Failed to define VM XML: %s", libvirt_err ? libvirt_err->message : "Unknown");
    } else {
        virDomainFree(dom);
        g_task_return_boolean(task, TRUE); // 성공!
    }

    // 메모리 정리
    virConnectClose(conn);
    g_strfreev(xml_parts);
    g_free(final_xml);
    g_free(raw_xml);
    g_object_unref(domain_config);
    purecvisor_vm_config_free(config);
    g_free(zvol_name); g_free(zvol_dev); g_free(zfs_cmd);
}

void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb,
                                           const gchar *iso_path,
                                           const gchar *network_bridge, // [Added]
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    CreateVmTaskData *data = g_new0(CreateVmTaskData, 1);

    data->manager = g_object_ref(self);
    data->name = g_strdup(name);
    data->vcpu = vcpu;
    data->ram_mb = ram_mb;
    data->disk_size_gb = disk_size_gb; // [Added]
    data->iso_path = iso_path ? g_strdup(iso_path) : NULL;
    data->network_bridge = network_bridge ? g_strdup(network_bridge) : NULL; // [Added]

    g_task_set_task_data(task, data, (GDestroyNotify)create_vm_task_data_free);
    g_task_run_in_thread(task, create_vm_thread);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager G_GNUC_UNUSED,
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}


// Re-implementing Start with struct
typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
} LifecycleTaskData;

static void lifecycle_task_data_free(LifecycleTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data);
}

static void start_vm_thread_impl(GTask *task, 
                                 gpointer source_object G_GNUC_UNUSED, 
                                 gpointer task_data, 
                                 GCancellable *cancellable G_GNUC_UNUSED) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err = NULL;

    GVirDomain *dom = gvir_connection_find_domain_by_name(data->manager->conn, data->name);
    if (!dom) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found");
        return;
    }

    if (!gvir_domain_start(dom, 0, &err)) {
        g_task_return_error(task, err);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(dom);
}

void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, start_vm_thread_impl);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager G_GNUC_UNUSED,
                                               GAsyncResult *res,
                                               GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --------------------------------------------------------------------------
 * [Async Task] Stop VM
 * -------------------------------------------------------------------------- */
static void stop_vm_thread_impl(GTask *task, 
                                gpointer source_object G_GNUC_UNUSED, 
                                gpointer task_data, 
                                GCancellable *cancellable G_GNUC_UNUSED) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err = NULL;

    GVirDomain *dom = gvir_connection_find_domain_by_name(data->manager->conn, data->name);
    if (!dom) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found");
        return;
    }

    // Shutdown (ACPI) -> if fails/force needed, destroy could be used
    if (!gvir_domain_shutdown(dom, 0, &err)) {
        g_task_return_error(task, err);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(dom);
}

void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, stop_vm_thread_impl);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager G_GNUC_UNUSED,
                                              GAsyncResult *res,
                                              GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --------------------------------------------------------------------------
 * [Async Task] Delete VM
 * -------------------------------------------------------------------------- */
static void delete_vm_thread_impl(GTask *task, 
                                  gpointer source_object G_GNUC_UNUSED, 
                                  gpointer task_data, 
                                  GCancellable *cancellable G_GNUC_UNUSED) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err = NULL;

    // 1. Libvirt Domain Undefine (Delete)
    GVirDomain *dom = gvir_connection_find_domain_by_name(data->manager->conn, data->name);
    if (dom) {
        // Stop if running (optional, usually delete fails if running)
        gvir_domain_stop(dom, 0, NULL); // Force stop
        
        if (!gvir_domain_delete(dom, 0, &err)) {
            g_task_return_error(task, err);
            g_object_unref(dom);
            return;
        }
        g_object_unref(dom);
    }

    // 2. ZFS Volume Destroy
    if (!purecvisor_zfs_destroy_volume("tank/vms", data->name, &err)) {
        // Libvirt 삭제는 성공했으나 스토리지가 남음 -> Warning logging needed ideally
        g_task_return_error(task, err);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, delete_vm_thread_impl);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager G_GNUC_UNUSED,
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --------------------------------------------------------------------------
 * [Async Task] List VMs (Refactored for Phase 5)
 * -------------------------------------------------------------------------- */
static void list_vms_thread(GTask *task, 
                            gpointer source_object G_GNUC_UNUSED, 
                            gpointer task_data, 
                            GCancellable *cancellable G_GNUC_UNUSED) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(task_data);
    GList *domains, *l;
    JsonBuilder *builder = json_builder_new();
    GError *err = NULL;

    json_builder_begin_array(builder);

    // 1. Fetch (Refresh Cache)
    if (!gvir_connection_fetch_domains(self->conn, NULL, &err)) {
        // Log warning but proceed with cached
        if (err) g_error_free(err);
    }

    // 2. Get Domains
    domains = gvir_connection_get_domains(self->conn);

    for (l = domains; l != NULL; l = l->next) {
        GVirDomain *dom = GVIR_DOMAIN(l->data);
        const gchar *name = gvir_domain_get_name(dom);
        const gchar *uuid = gvir_domain_get_uuid(dom);
        
        // [Phase 5] ID Check for State (No Linker Error)
        // gvir_domain_get_id takes GError** as second arg
        gint dom_id = gvir_domain_get_id(dom, NULL);
        
        const gchar *state_str = "shutoff";
        gboolean is_active = FALSE;

        if (dom_id > 0) {
            state_str = "running";
            is_active = TRUE;
        }

        // [Phase 5] Dynamic VNC Port Parsing
        gint vnc_port = -1;
        if (is_active) {
            vnc_port = _extract_vnc_port_from_domain(dom);
        }

        // Build JSON Object
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        
        json_builder_set_member_name(builder, "uuid");
        json_builder_add_string_value(builder, uuid);
        
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, state_str);
        
        json_builder_set_member_name(builder, "vnc_port");
        if (vnc_port > 0) {
            json_builder_add_int_value(builder, vnc_port);
        } else {
            json_builder_add_null_value(builder);
        }
        json_builder_end_object(builder);
    }

    g_list_free_full(domains, g_object_unref);
    json_builder_end_array(builder);

    // Return the JsonNode root
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
}

void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    
    // We pass 'self' as task data to access connection in thread
    // Ref it to ensure safety
    g_task_set_task_data(task, g_object_ref(self), g_object_unref);
    
    g_task_run_in_thread(task, list_vms_thread);
    g_object_unref(task);
}

JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager G_GNUC_UNUSED,
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ========================================================================= */
/* Phase 6-2: Runtime Resource Tuning (근본 해결책: Raw Libvirt API 사용)    */
/* ========================================================================= */

// --- 헬퍼 구조체 ---
typedef struct {
    gchar *vm_name;
    guint target_value;
} ResourceTuningData;

static void resource_tuning_data_free(ResourceTuningData *data) {
    if (data) {
        g_free(data->vm_name);
        g_free(data);
    }
}

// --- 1. Memory Tuning (Worker Thread) ---
static void set_memory_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

    // 1. 스레드 독립적인 Raw Libvirt 커넥션 오픈 (Wrapper 우회)
    virConnectPtr raw_conn = virConnectOpen("qemu:///system");
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    // 2. Raw 도메인 검색
    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virConnectClose(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

    // 3. 동적 메모리 조절 (Live & Config 영구 적용)
    guint memory_kb = data->target_value * 1024;
    int ret = virDomainSetMemoryFlags(raw_domain, memory_kb, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "Memory tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    // 4. 자원 해제
    virDomainFree(raw_domain);
    virConnectClose(raw_conn);
}

void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self, const gchar *name, guint memory_mb, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = memory_mb;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_memory_thread_impl);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

// --- 2. vCPU Tuning (Worker Thread) ---
static void set_vcpu_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

    // 1. 스레드 독립 커넥션
    virConnectPtr raw_conn = virConnectOpen("qemu:///system");
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virConnectClose(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

    // 2. vCPU 개수 조절
    int ret = virDomainSetVcpusFlags(raw_domain, data->target_value, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "vCPU tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(raw_domain);
    virConnectClose(raw_conn);
}

void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self, const gchar *name, guint vcpu_count, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = vcpu_count;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_vcpu_thread_impl);
    g_object_unref(task);
}

gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}
