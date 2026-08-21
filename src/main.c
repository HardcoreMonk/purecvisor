   
               
                                                   
  
                           
                                                   
                                                    
                                        
  
          
                                                   
                                               
                                            
                                                          
                                              
                                              
  
                                                                       
              
                                                                       
                                           
                                                          
                                           
  
               
                                                                 
                                                         
                                                         
                                                                  
                                                          
                                                              
                                                           
                                                            
                                                          
  
                                                                       
         
                                                                       
                                                      
                                                       
                                                       
                                         
  
                  
                                                  
                                                        
                                           
                                                    
  
                                                                       
                                     
                                                                       
                                                              
                                               
                                                                  
                                                             
                                                            
                                                                    
                                                 
                                                            
                                                                  
                                                                
                                                             
                                                
                                                   
                                            
                                                            
                                                  
  
                                                                       
         
                                                                       
                                                                 
                                                            
                                                 
  
                                                                       
            
                                                                       
                                               
                                                                
                                                       
                                                    
  
                                                                       
        
                                                                       
                                                      
                                                         
                                                             
                                                   
                                                                    
  
                                                                       
            
                                                                       
                                 
                                                 
                                    
                                                 
                                              
                 
  
                                              
                                              
                            
   

                                                              
#include <unistd.h>                                            
#include <glib.h>                                                    
#include <glib-unix.h>                                                
#include <libvirt-gobject/libvirt-gobject.h>                              
#include <stdio.h>                           
#include <libvirt/libvirt.h>                               

                                                               
#include "api/uds_server.h"                                           
#include "api/dispatcher.h"                                      
#include "api/rest_server.h"                                                  
#include "api/grpc_server.h"                                         
#include "modules/virt/vm_manager.h"                               
#include "modules/network/pcv_qos.h"                                                              
#include "modules/network/pcv_qos_chaos.h"                                                              
#include "modules/daemons/pcv_trace.h"                                                                       
#include "api/drain.h"                                                                     
#include "utils/logger.h"                     
#include "utils/pcv_log.h"                                                
#include "utils/pcv_config.h"                                              
#include "utils/pcv_privdrop.h"                                                  
#include "utils/pcv_validate.h"                                    
#include "utils/pcv_secure.h"                                                         

                                                                 
#include "modules/core/vm_state.h"                                           
#include "modules/core/cpu_allocator.h"                             

                                                              
#include "modules/daemons/telemetry.h"                                                         
#include "modules/daemons/virt_events.h"                                    
#include "modules/daemons/ebpf_telemetry.h"                                                   
#include "modules/daemons/alert_engine.h"                                               
#include "modules/daemons/pcv_webpush.h"                                       
#include "modules/daemons/process_monitor.h"                                      
#include "modules/daemons/update_check.h"                                    

                                                                 
#include "utils/pcv_spawn.h"                                             
#include "utils/pcv_worker_pool.h"                                               
#include "purecvisor/pcv_validate.h"                                       
#include "purecvisor/version.h"                                       
#include "utils/pcv_jwt.h"                                          
#include "utils/pcv_bpf.h"                                                              

                                                             
#include "modules/virt/virt_conn_pool.h"                                         
#include "modules/virt/cancellable_map.h"                                        

                                                               
#include "bootstrap/pcv_bootstrap.h"                                 

                                                           
#include "modules/network/ovs_overlay.h"                                          
#include "modules/network/network_manager.h"                                     
#include "modules/network/pcv_shared_bridge.h"                                        
#include "modules/network/ovn_manager.h"                                  
#include "modules/storage/iscsi_manager.h"                                        
#include "modules/network/dpdk_manager.h"                                  
#include "modules/network/sriov_manager.h"                                          
#include "modules/network/security_group.h"                                            
#include "modules/network/vpc/vpc_manager.h"                                                     
#include "modules/network/vpc/vpc_model.h"                                       
#include "modules/security/security_store.h"                                                      
#include "modules/security/pcv_suricata.h"                                                   
#include "modules/security/pcv_suricata_ips.h"                                      
#include "modules/security/pcv_suricata_ips_rules.h"                                            
#include "modules/network/tenant_overlay.h"                                
#include "io/pcv_uring.h"                                                    
#include "api/hot_reload.h"                                          
#include "api/ws_server.h"                                               
#include "modules/storage/storage_tier.h"                       
#include "modules/daemons/prometheus_exporter.h"                            
#include "modules/audit/pcv_audit.h"                         
#include "utils/pcv_job_queue.h"                        
#include "modules/accel/gpu_manager.h"                                 
#include "modules/plugin/pcv_plugin_manager.h"                      
#include "utils/pcv_tls.h"                                     
#include "modules/network/nfv_manager.h"                               
#include "modules/backup/backup_scheduler.h"                         
#include "modules/auth/pcv_rbac.h"                              
#include "modules/template/vm_template.h"                   

                                                                      
        
  
                
                                             
                                               
                                            
                    
                                                                         

   
                                     
  
                  
                                            
                                  
                                      
                                                           
                                                    
  
                
                                                           
                                             
                          
  
       
                                                             
                                                     
                             
   
static GMainLoop *loop;

   
                                       
  
                 
                                                                                   
                                                    
                                   
  
             
                                                                  
                                               
  
       
                                                                         
                                                                       
   
CpuAllocator *global_allocator = NULL;

                                                                      
          
  
                            
                                                              
                                                        
                                                  
                                                         
                                                                
                                                                         

   
                                                                
  
                                                
                                                  
  
                          
                                                     
                                                           
                                                         
                                                               
                                               
  
                    
                           
                                                   
                                                      
                                       
  
                                                         
                                                   
   

   
                                                                  
                 
  
              
                                                                         
                                                 
                                                     
  
         
                                                                          
                                                            
                                          
  
                
                                                       
                                                                 
   
static GThread   *g_watchdog_thread = NULL;
static volatile gint g_watchdog_stop = 0;                             

   
                                                                 
  
                                                      
                                                   
  
                                                                    
                                       
                                                               
                                                               
                                                               
   
static gpointer
_watchdog_thread_func(gpointer data)
{
                                                           
    guint64 interval_us = GPOINTER_TO_SIZE(data);
    if (interval_us < 1000000) interval_us = 1000000;                      

    while (!g_atomic_int_get(&g_watchdog_stop)) {
        pcv_drain_notify_watchdog();                                
                                                          
                                                         
                                                 
        guint64 slept = 0;
        while (slept < interval_us &&
               !g_atomic_int_get(&g_watchdog_stop)) {
            g_usleep(100000);             
            slept += 100000;
        }
    }
    return NULL;
}

                                                   
                                                                
                                                      
static gboolean on_signal_received(gpointer user_data) {
    (void)user_data;                                           
    if (!loop) return G_SOURCE_REMOVE;
    g_message("Signal received, initiating graceful shutdown...");

      
                                                               
                                
                                        
                                             
                                                        
       
    pcv_drain_begin(loop, pcv_config_get_drain_timeout());

    return FALSE;                                             
}

   
                                                
  
                                                
                                                 
  
                                                                  
                                                  
                                                                           
                                                    
  
                                                 
   
static gboolean on_sighup_received(gpointer user_data) {
    (void)user_data;
    g_message("[main] SIGHUP received, reloading configuration");
    if (pcv_alert_engine_reload_daemon_config()
        != PCV_ALERT_CONFIG_SET_OK) {
        g_warning("[main] SIGHUP configuration reload rejected: "
                  "invalid alert daemon config");
    }
    pcv_log_load_module_levels();                     
    return TRUE;                             
}

                                                                      
                          
  
                   
                                                        
                                            
                                               
                              
  
                                               
                                       
                      
                                                                         

   
                                                  
  
                                                     
                                                    
  
                                                                 
                                                      
  
                              
                                                      
   
static guint read_cpu_numa_node(guint logical_id) {
    gchar *dirpath = g_strdup_printf("/sys/devices/system/cpu/cpu%u", logical_id);
    guint node = 0;
    GDir *dir = g_dir_open(dirpath, 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_prefix(name, "node")) {
                gchar *end = NULL;
                guint64 n = g_ascii_strtoull(name + 4, &end, 10);
                if (end != name + 4 && *end == '\0') { node = (guint)n; break; }
            }
        }
        g_dir_close(dir);
    }
    g_free(dirpath);
    return node;
}

   
                                                      
  
                                                 
                                                   
  
                              
                                                          
   
static guint read_cpu_physical_id(guint logical_id) {
    gchar *path = g_strdup_printf(
        "/sys/devices/system/cpu/cpu%u/topology/core_id", logical_id);
    gchar *content = NULL;
    guint phys = logical_id;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        g_strstrip(content);
        gchar *end = NULL;
        guint64 v = g_ascii_strtoull(content, &end, 10);
        if (end != content && *end == '\0') phys = (guint)v;
        g_free(content);
    }
    g_free(path);
    return phys;
}

   
                                                                 
  
                                                        
                                                   
                                            
  
                       
                                                                    
                                                                           
                                                                   
                                                                     
  
                            
                                                                   
                                                                            
                                                             
                                                   
  
                    
                                                           
                                              
  
                                                         
   
static void scan_and_register_host_topology(CpuAllocator *alloc) {
    g_message("[Init] Scanning Host Topology and Isolated CPUs...");

                              
    gchar *online_raw = NULL;
    gboolean online_read = g_file_get_contents(
        "/sys/devices/system/cpu/online", &online_raw, NULL, NULL);
    GArray *online = online_read ? cpu_allocator_parse_cpulist(online_raw) : NULL;
    g_free(online_raw);

    if (!online || online->len == 0) {
                                                       
        g_warning("[Init] Failed to read/parse /sys/devices/system/cpu/online — "
                  "registering core 0 (non-isolated) only; CPU pinning effectively "
                  "disabled (graceful fallback).");
        cpu_allocator_add_core(alloc, 0, 0, 0, FALSE);
        if (online) g_array_unref(online);
        return;
    }

                                                      
    gchar *isolated_raw = NULL;
    GArray *isolated = NULL;
    if (g_file_get_contents("/sys/devices/system/cpu/isolated",
                            &isolated_raw, NULL, NULL)) {
        isolated = cpu_allocator_parse_cpulist(isolated_raw);
    }
    g_free(isolated_raw);
    if (!isolated) {
                                                
        isolated = g_array_new(FALSE, FALSE, sizeof(guint));
    }

                                             
    guint isolated_count = 0;
    for (guint i = 0; i < online->len; i++) {
        guint cpu = g_array_index(online, guint, i);
        guint numa = read_cpu_numa_node(cpu);
        guint phys = read_cpu_physical_id(cpu);
        gboolean iso = cpu_allocator_cpulist_contains(isolated, cpu);
        if (iso) isolated_count++;
        cpu_allocator_add_core(alloc, cpu, phys, numa, iso);
    }

    g_message("[Init] Host Topology mapped: %u online core(s), %u isolated. %s",
              online->len, isolated_count,
              isolated_count == 0
                ? "No isolcpus set — VMs created without exclusive core pinning "
                  "(graceful fallback)."
                : "Isolated cores available for exclusive VM pinning.");

    g_array_unref(online);
    g_array_unref(isolated);
}

                                                                      
                               
  
                      
                                             
                                             
                                           
  
                                             
                                 
                              
                                                      
  
            
                                                          
                                            
                                  
                                                                         

#define SIG_PROBE_DOM "signal_probe"                          

   
                                                    
  
                                                   
                                                   
  
                                                               
                               
                                               
  
                                                                          
                                              
                                                               
  
                                                                    
                                                
   
static void
_on_vm_started_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-started RECEIVED — vm_name='%s'", vm_name);
}

   
                                                    
  
                                                     
  
                                                                      
                                
  
                                         
                              
                                  
   
static void
_on_vm_stopped_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-stopped RECEIVED — vm_name='%s'", vm_name);
}

   
                                                    
  
                                                
                                                   
  
                                                          
                                              
  
                   
                                                              
                                                           
  
           
                                     
                                 
                                         
                     
  
                                         
                                                                    
                                  
   
static void
_on_metrics_updated_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                          GHashTable          *cache,
                          gpointer             user_data __attribute__((unused)))
{
    guint n = cache ? g_hash_table_size(cache) : 0;

                                                   
    const gchar *first_uuid = NULL;
    if (cache && n > 0) {
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, cache);
        g_hash_table_iter_next(&it, &key, NULL);
        first_uuid = (const gchar *)key;
    }

    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-metrics-updated RECEIVED — "
                 "vm_count=%u first_uuid=%s",
                 n, first_uuid ? first_uuid : "(none)");
}

                                                                      
                    
  
             
                       
                            
                                                
                                           
                                                  
  
               
                                                        
                                          
                                                  
  
             
                                        
                                             
                                                  
                                                                         

   
                                                   
  
                                                 
                                                    
  
                                                
                                                 
                            
   
int main(int argc, char *argv[]) {
    const PcvBootstrapEditionInfo *edition_info = pcv_bootstrap_get_edition_info();

                                                                    
                      
      
                      
                                                                
                                                    
                                         
                                  
                                            
      
                
                                                                          
                                                            
                                                    
                                                                       
    if (edition_info) {
        g_message("[init] Edition bootstrap: %s (cluster=%s)",
                  edition_info->edition_name,
                  edition_info->cluster_enabled ? "enabled" : "disabled");
    }

                                                   
                                                        
    if (geteuid() != 0) {
        fprintf(stderr, "\n\x1b[31m[!] CRITICAL ERROR: INSUFFICIENT PRIVILEGES\x1b[0m\n");
        fprintf(stderr, "    The PureCVisor Daemon MUST be run as root.\n");
        fprintf(stderr, "    Please execute using sudo: \x1b[33msudo %s\x1b[0m\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }

                                                                    
                        
      
              
                                                  
                                                  
                                    
      
           
                                                                      
                                                              
                                                 
                                                                       
    g_setenv("LIBVIRT_LOG_OUTPUTS", "1:file:/dev/null", TRUE);
    g_setenv("LIBVIRT_LOG_FILTERS", "1:libvirt", TRUE);

    GError *error = NULL;

                                                                    
                           
      
                       
                                                          
                                      
      
                 
                                                      
                                                  
                                                                       
    purecvisor_logger_init();                               
    pcv_config_init();                                              

                                             
                                                   
    gboolean https_enabled = TRUE;
    if (!pcv_config_get_https_enabled(&https_enabled, &error) ||
        !pcv_config_validate_transport(&error)) {
        g_critical("Invalid transport configuration: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        pcv_config_shutdown();
        pcv_log_shutdown();
        return EXIT_FAILURE;
    }
    PcvRestTransportPlan rest_transport = pcv_rest_transport_plan(
        pcv_rest_tls_mode_from_config(https_enabled),
        pcv_config_get_string("server", "bind_plaintext", "loopback"));
    pcv_log_load_module_levels();                                 

      
                                      
      
                                              
                                                
                                              
       
    #if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
    #endif

      
                                 
      
                                                           
                                                      
                            
      
                                                  
                               
       
    gvir_init_object(&argc, &argv);

    g_message("Starting PureCVisor Engine...");

                                                           
      
           
                                                 
                                         
                                                                    
      
                        
                                   
                                              
                                                           
      
                      
                                         
                                                               
      
                             
                                    
                                                             
                                                                        
    gint64 init_total_start = g_get_monotonic_time();
    gint64 stage_start;
    gint   stage_num = 0;

#define STAGE_BEGIN(label) do { \
    stage_num++; \
    stage_start = g_get_monotonic_time(); \
    (void)0; } while(0)

#define STAGE_END(label) do { \
    gint64 _ms = (g_get_monotonic_time() - stage_start) / 1000; \
    g_message("[init] Stage %d (%s) completed in %ldms", stage_num, (label), (long)_ms); \
    } while(0)

                                                                    
                                    
      
                         
                                               
                                            
                                  
                                             
                                       
      
                                                      
                                                   
                                         
                                                                       

      
                                      
      
           
                                          
                                         
                                                             
      
              
                                                    
                                            
      
              
                                                                               
       
    STAGE_BEGIN("core-modules");
    init_pending_state_machine();

      
                           
      
                    
                                                          
                                        
                                                       
      
                
                                                      
                                                                 
      
                      
                                                        
                                
       
    virt_conn_pool_init((guint)pcv_config_get_pool_max_conn());

      
                        
      
                        
                                           
                                                             
                                   
      
                 
                                        
                                                                
                          
       
    cmap_init();

      
                            
      
                       
                                              
                                 
                                    
      
                   
                                                          
                                                  
       
    pcv_drain_init();

      
                     
      
                                    
                           
                                                     
       
    global_allocator = cpu_allocator_new();
    scan_and_register_host_topology(global_allocator);

                                                           
                                                                   
                                                                           
                                                      
      
                                                                     
                                                                      
                                                                     
                                                              
      
                                                                            
                                                                                 
                                                     
                                                          
                                                          
                                                              
    {
        virConnectPtr rec_conn = virt_conn_pool_acquire();
        if (rec_conn) {
            cpu_allocator_reconcile(global_allocator, rec_conn);
            virt_conn_pool_release(rec_conn);
        } else {
            g_warning("[cpu_allocator] 부팅 reconcile 스킵 — libvirt conn 취득 실패. 재시작 후 "
                      "실행 중 VM 의 핀 코어가 미등록되어 새 vm.start 가 오버커밋할 수 있다");
        }
    }

                                                          
                                                   
    STAGE_END("core-modules");

                                                                    
                                           
      
                  
                                                
                                                                             
                                                                   
      
                      
                                              
                                             
                                                                  
                                        
      
                              
                                               
                                                                
                                                                       
                                                                       
    STAGE_BEGIN("security");
                                                        
                                                   
    pcv_privdrop_apply_all();

    if (pcv_config_get_allow_core_dumps())
        pcv_privdrop_enable_coredumps();                                                                 
    else
        pcv_privdrop_disable_coredumps();

      
                                  
      
                      
                                                             
                                        
      
                                      
                                           
                                             
                                                
      
                               
                                                                    
                                                                      
                                                                 
       
    pcv_spawn_launcher_init();
    pcv_worker_pool_init();                                                      
    pcv_update_check_init();                                       

      
                                   
      
                                       
                                              
                                           
                                           
                                 
       
    pcv_network_rundir_init();

                                                        
                                                                       
                                                   
                                                                   
                                                  
    {
        GError *audit_error = NULL;
        if (!pcv_audit_init(pcv_config_get_string(
                "audit", "db_path", "/var/lib/purecvisor/pcv_audit.db"),
                &audit_error)) {
                                                                
                                                           
                                                     
            g_critical("[init] audit subsystem failed closed: %s",
                       audit_error ? audit_error->message : "unknown error");
            g_clear_error(&audit_error);
            return 1;
        }
    }

                                                           
                                                                          
                                                         
                                                         
                                                            
                                                           
                                                           
                                                       
                                                   
                                                    
                                                               
                                                             
                                                  
                    
    {
        GError *bpf_err = NULL;
        if (!pcv_bpf_init("/usr/lib/purecvisor/bpf", &bpf_err)) {
                                                         
                                                 
            g_critical("[init] pcv_bpf_init 치명: %s", bpf_err ? bpf_err->message : "?");
            g_clear_error(&bpf_err);
        } else if (!pcv_bpf_rehydrate("/usr/lib/purecvisor/bpf", &bpf_err)) {
            g_warning("[init] pcv_bpf_rehydrate 실패(degraded, 계속): %s",
                      bpf_err ? bpf_err->message : "?");
            g_clear_error(&bpf_err);
        }
                                                                         
                                                                      
                                                           
                                                                       
        if (!pcv_shared_bridge_bpf_prepare("/usr/lib/purecvisor/bpf", &bpf_err)) {
            g_warning("[init] shared bridge BPF prepare 실패(shared disabled): %s",
                      bpf_err ? bpf_err->message : "?");
            g_clear_error(&bpf_err);
        }
    }

                                                                  
                                                     
    pcv_security_group_restore();

                                                              
                                                                
                                                               
                                                       
    {
        GError *rehy_err = NULL;
        if (!pcv_tenant_overlay_rehydrate(&rehy_err)) {
            g_warning("오버레이 레지스트리 재수화 실패(degraded, 계속): %s",
                      rehy_err && rehy_err->message ? rehy_err->message : "unknown");
            g_clear_error(&rehy_err);
        }
    }

                                                              
                                                              
                                                        
    pcv_security_group_overlay_exclusion_audit();

                                                                      
                                                               
                                     
      
                   
                                             
                                                                      
                                                                 
                                                                               
                                                                 
                                                          
                                                   
                                                             
                                                     
                 
                                                   
                                                            
                                                             
                                   
                                                  
      
                                                                 
                                                               
                                                     
                                                              
                                             
                                                                          
                                                  
                                                     
                                                  
                                                       
                                                  
                                                           
    guint qos_reconcile_timer_id = 0;
    guint qos_metrics_timer_id = 0;
    {
        gint qos_uplink_mbps = pcv_config_get_int("qos", "uplink_mbps", 1000);
        if (qos_uplink_mbps <= 0) {
            g_warning("[qos] qos.uplink_mbps 설정값(%d)이 비합리(<=0) — 기본값 1000Mbit 로 폴백",
                      qos_uplink_mbps);
            qos_uplink_mbps = 1000;
        }

        if (!pcv_qos_ids_load(PCV_QOS_IDS_PATH))
            g_warning("[qos] qos_ids.json 로드 실패/오염 — 빈 상태로 시작(신규 minor 배정)");

                                                             
                                                           
                                                            
        if (!pcv_qos_tenant_sla_load(PCV_QOS_TENANT_SLA_PATH))
            g_warning("[qos] qos_tenants.json 로드 실패/오염 — 빈 상태로 시작(테넌트 SLA 미적용)");

        GError *qos_err = NULL;
        if (!pcv_qos_ensure_root((guint32)qos_uplink_mbps, &qos_err)) {
            g_warning("[qos] ensure_root 실패 — QoS 비활성(수동 조치 필요): %s",
                      qos_err ? qos_err->message : "unknown");
            g_clear_error(&qos_err);
        }

                                                                       
                                                             
                                              
                                                          
                                                          
                                                           
                                                                
                                                            
                                                 
                                                         
                                                
                                               
                                               
        pcv_qos_chaos_purge_all();

                                                           
                                                          
                                                          
                                               
                                                       
                                                      
                                                   
                           
        pcv_trace_purge_all();

        pcv_qos_backfill_existing();

        pcv_qos_set_expected_provider(pcv_vm_qos_expected_provider);
        GError *recon_err = NULL;
        if (!pcv_qos_reconcile(&recon_err)) {
            g_warning("[qos] 부팅 reconcile 일부 실패(주기 타이머가 재시도): %s",
                      recon_err ? recon_err->message : "unknown");
            g_clear_error(&recon_err);
        }

                                                                          
                                                             
                                                      
                                                             
                                                                            
        GError *save_err = NULL;
        if (!pcv_qos_ids_save(PCV_QOS_IDS_PATH, &save_err)) {
            g_warning("[qos] 부팅 reconcile 후 qos_ids.json 저장 실패(WARN — 다음 재시작 결정성에 "
                      "영향 가능): %s", save_err ? save_err->message : "unknown");
            g_clear_error(&save_err);
        }

        qos_reconcile_timer_id = pcv_qos_reconcile_timer_start();
                                                                  
                                                                            
                                                                 
                                                                  
        qos_metrics_timer_id = pcv_qos_metrics_timer_start();
    }

                                                                               
                                                                      
                                                      
                                                                     
                                                                 
                                                                     
                                                             
                                                                  
                                                         
                                                 
                                                      
                                                     
                                        
                                                             
                                                   
                                                       
                                                            
                                                        
                                                    
    if (!pcv_security_store_ensure_open()) {
        g_warning("[init] security_store ensure_open 실패 — BPF 소비 스레드는 "
                  "기동하되 store 가 재개될 때까지 LSM 이벤트 제출은 드롭됩니다");
    }
    pcv_bpf_consumer_start();

                                                           
                                                                 
                                                                      
                                                               
                                                         
                                                           
                                                          
                                                   
    pcv_suricata_eve_tail_start();

      
                                                   
      
                                                          
                                             
      
                           
                                                          
                                             
                                                   
                                                                        
                
      
                                             
                                                    
                                                           
                                                          
                                                   
                                                      
                                                     
                                               
                                                            
                    
      
                                  
                                                       
                                             
                                                    
                                                   
                                                      
                       
      
                                 
                                                           
                                                
                                                         
                                             
                                                                     
                                                         
                    
      
                                    
                                                               
                                          
                                               
                                                      
                                                 
                      
       
    init_virt_events_daemon();

                                                                         
                                                                
                                                      
                                                          
                                                             
                                                         
                                                               
                                                      
                                                                 
                                                     
                                                         
                                                         
                                                      
                                          
    {
        virConnectPtr rc = virt_conn_pool_acquire();
        if (rc) {
            virDomainPtr *doms = NULL;
            int ndom = virConnectListAllDomains(rc, &doms,
                                                VIR_CONNECT_LIST_DOMAINS_ACTIVE);
            if (ndom >= 0) {
                GHashTable *running = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                            g_free, NULL);
                for (int i = 0; i < ndom; i++) {
                    const char *nm = virDomainGetName(doms[i]);
                    if (nm) g_hash_table_add(running, g_strdup(nm));
                    virDomainFree(doms[i]);
                }
                free(doms);

                guint reaped = 0;
                GPtrArray *members = pcv_tenant_overlay_list_member_vms();
                for (guint i = 0; i < members->len; i++) {
                    const gchar *vm = g_ptr_array_index(members, i);
                    if (g_hash_table_contains(running, vm)) continue;
                    g_message("[ovl5] 부트 reconcile: 유령 오버레이 멤버 '%s'"
                              "(running 아님) — 회수", vm);
                    pcv_tenant_overlay_on_vm_gone(vm);
                    reaped++;
                }
                g_ptr_array_unref(members);
                g_hash_table_unref(running);
                if (reaped)
                    g_message("[ovl5] 부트 reconcile: 유령 멤버 %u개 회수", reaped);
            } else {
                g_warning("[ovl5] 부트 reconcile 스킵 — ACTIVE 도메인 열거 실패");
            }
            virt_conn_pool_release(rc);
        } else {
            g_warning("[ovl5] 부트 reconcile 스킵 — libvirt conn 취득 실패. 데몬 다운 중 "
                      "소멸한 VM 의 오버레이 잔재가 다음 재시작까지 잔존할 수 있다");
        }

        guint sweep_fail = 0;
        guint swept = pcv_tenant_overlay_sweep_orphan_endpoints(&sweep_fail);
        if (swept || sweep_fail)
            g_message("[ovl5] 고아 endpoint 스윕: 회수 %u, 실패 %u", swept, sweep_fail);
    }
    STAGE_END("security");

                                                                    
                                     
      
                 
                                                    
                                  
                                                                       

      
                                 
      
                                             
                                                                   
                                                    
                                            
                                                                  
                                                     
                                           
      
                    
                                                     
                                                   
                                    
                                      
       
    STAGE_BEGIN("libvirt-dispatcher");
    gboolean libvirt_degraded = FALSE;
    GVirConnection *conn = gvir_connection_new(pcv_config_get_libvirt_uri());
                                                          
                                                                 
    if (!gvir_connection_open(conn, NULL, &error)) {
        g_warning("libvirt connection failed: %s — entering DEGRADED mode "
                  "(VM operations unavailable, REST/cluster queries still active)",
                  error->message);
        g_error_free(error);
        error = NULL;
        libvirt_degraded = TRUE;
                                            
                                              
    }

      
                              
      
                 
                                                  
                                                       
                                                       
      
                     
                                                     
                                            
                                                          
       
    PureCVisorDispatcher *dispatcher = purecvisor_dispatcher_new();
    purecvisor_dispatcher_set_connection(dispatcher, conn);

      
                               
      
                             
                                       
                                          
                                           
      
                 
                                            
                                            
                                                            
                                        
       
    PureCVisorVmManager *_mgr =
        purecvisor_dispatcher_get_vm_manager(dispatcher);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(_on_vm_started_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STOPPED,
                     G_CALLBACK(_on_vm_stopped_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_CALLBACK(_on_metrics_updated_probe), NULL);

      
                                   
      
              
                                                                 
                                                         
                                                                    
                                                 
      
                                
                                                   
                                                     
                                                
       
    init_telemetry_daemon(_mgr);
    STAGE_END("libvirt-dispatcher");

                                                                    
                        
      
                                  
                                           
                                               
                                       
      
             
                                                                                
                                             
      
              
                                                                       
                                                               
                                                                       
    STAGE_BEGIN("uds-server");
    UdsServer *server = uds_server_new(pcv_config_get_socket_path());
    uds_server_set_dispatcher(server, dispatcher);

                                                       
                                                    
    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS server: %s", error->message);
        g_error_free(error);
        return 1;                                      
    }
    STAGE_END("uds-server");

                                                                    
                                
      
                                  
                                                                   
                                                       
                                                        
                                                                  
      
                         
                                                                    
                                                   
                                             
      
                   
                                             
                                      
                                                          
                                                                       
    STAGE_BEGIN("rest-grpc");
    pcv_rest_transport_initialize(&rest_transport, pcv_tls_init_from_config);
    {
                                                                  
        gchar *jwt_secret = pcv_config_get_secret("auth", "jwt_secret", NULL);
        if (jwt_secret && *jwt_secret) {
            pcv_jwt_init(jwt_secret);
            pcv_secure_free_str(&jwt_secret);                                 
        } else {
            pcv_secure_free_str(&jwt_secret);                                 
            pcv_jwt_init(pcv_config_get_jwt_secret());                   
        }
    }

    PcvRestServer *rest_server =
        pcv_rest_server_new(dispatcher, 0, rest_transport);
    if (!pcv_rest_server_start(rest_server, &error)) {
        g_critical("Failed to start REST server: %s", error->message);
        g_error_free(error);
                                                  
        g_object_unref(rest_server);
        rest_server = NULL;
        g_warning("REST API unavailable — continuing with UDS only");
    }

                                                                    
                                                     
      
                                               
                                                
                                                                       
    pcv_grpc_server_start();
    STAGE_END("rest-grpc");

                                                                    
                        
      
                         
                                
                                              
                                            
                                                  
      
                               
                                         
                                       
      
                
                                                            
                                               
                                 
                                      
                                                 
                                                                      
                                                                       
    STAGE_BEGIN("cluster");
    pcv_bootstrap_init_cluster_manager();

                                                                    
                                      
      
                   
                                                        
                                                      
                                     
      
                
                                           
                                                         
                                                      
                                                          
                                          
      
               
                                                                  
                                         
                                                    
      
                             
                                                          
                                           
                                                                       
    STAGE_END("cluster");

    STAGE_BEGIN("network-storage");
    pcv_overlay_init(pcv_config_get_string("overlay", "tunnel_ip", ""));
                                                            
                                                                   
                                                          
                                                  
    pcv_overlay_restore();
    pcv_iscsi_init();
    pcv_ovn_init();

                                                             
                                                                
                                                    
                                                                
    {
        GError *vpc_error = NULL;
        if (!pcv_vpc_init(NULL, &vpc_error)) {
            g_critical("[vpc] startup reconcile failed closed: %s",
                       vpc_error ? vpc_error->message : "unknown");
            g_clear_error(&vpc_error);
            return EXIT_FAILURE;
        }
        if (!pcv_vpc_reconcile(&vpc_error)) {
            if (g_error_matches(vpc_error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE)) {
                                                                             
                                                                  
                g_warning("[vpc] startup reconcile completed with quarantine: %s",
                          vpc_error->message);
                g_clear_error(&vpc_error);
            } else {
                g_critical("[vpc] startup reconcile failed closed: %s",
                           vpc_error ? vpc_error->message : "unknown");
                g_clear_error(&vpc_error);
                return EXIT_FAILURE;
            }
        }
    }

                                                                        
    pcv_dpdk_init();                                                    
    pcv_sriov_init();                                                        

      
                                     
      
                    
                                              
                                                                 
                                  
                                                 
      
                         
                                                     
                                                  
                                             
      
              
                                                            
                                                       
                                                            
      
                             
                                            
                                                               
                                            
       
#if PCV_USE_URING
    {
        GError *uring_err = NULL;
        PcvUringCtx *uring = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &uring_err);
        if (uring) {
            g_message("[main] io_uring initialized (queue_depth=%u, eventfd=%d)",
                      PCV_URING_DEFAULT_QUEUE_DEPTH, uring->event_fd);
                                                               
        } else {
                                                               
            g_warning("[main] io_uring init failed: %s — using GLib I/O fallback",
                      uring_err ? uring_err->message : "unknown");
            if (uring_err) g_error_free(uring_err);
        }
    }
#endif

    STAGE_END("network-storage");

                                                                    
                                             
      
                   
                                             
                                      
                                 
                                                                       

      
                              
      
                                                   
                                                        
                                
       
    STAGE_BEGIN("observability");
    pcv_prom_init();

      
                           
      
                                                       
                                              
                                             
      
                                                         
                                                           
       

                                                        
    pcv_job_queue_init();

      
                       
      
                                      
                                             
                                           
       
    pcv_hot_reload_init(pcv_bootstrap_get_daemon_binary_path(), -1);

                                        
    pcv_storage_tier_init();

      
                         
      
                                                              
                                                           
                                                         
       
    pcv_gpu_init();

      
                                     
      
                                                    
                                       
                                        
       
    pcv_plugin_manager_init("/etc/purecvisor/plugins.d");

      
                             
      
                                               
                                         
                                           
                                                   
                                                         
       

    STAGE_END("observability");

    STAGE_BEGIN("extensions");
                                                                    
    pcv_nfv_init();

                                            
    pcv_bootstrap_init_federation();

    STAGE_END("extensions");

                                                                    
                                               
      
                             
                                  
                                                                 
                                                
                                                     
                                                             
                                                            
                                                                       

      
                                
      
                        
                                                                                 
                                                                                         
                                              
                                 
                                                       
                                            
                               
                                                                   
                       
                                                            
      
                       
                                              
                                               
                                               
       
    STAGE_BEGIN("monitoring");
    pcv_ebpf_telemetry_init();

                                                            
                                                                      
                                                            
                                                          
                                                   
                                                  
                                                        
    pcv_suricata_health_start();

                                                                 
                                                   
      
                                                                     
                                                                  
                                                              
                                                                      
                                                               
                                                        
                                                          
                  
      
                                                                     
                                                                
    pcv_suricata_ips_boot_flush_stale();

                                                                    
                                                             
                                                                 
    if (pcv_config_get_ips_enabled()) {
        guint    ips_qn = (guint)pcv_config_get_ips_queue_num();
        gboolean ips_fo = pcv_config_get_ips_fail_open();
        GError  *ips_err = NULL;

                                                
          
                                                                                 
                                                         
                                                           
                                                          
          
                                                      
                                                             
                                  
                                                                              
                                                                      
                                                                        
                                                                             
                                                         
                                        
                                                                  
                                         
                                                                 
                                       
          
                                                                      
                                                                
                                                                 
                                                             
          
                                                     
                                                     
                                                 
                                                                            
                          
        if (!g_file_test(PCV_IPS_RULES_PATH, G_FILE_TEST_EXISTS)) {
            PCV_LOG_INFO("suricata-ips",
                         "D13 Phase B Stage 2: 파생 룰셋 부재 — 최초 1회 생성 (%s)",
                         PCV_IPS_RULES_PATH);
            GHashTable *boot_sids = pcv_suricata_policy_drop_sids_snapshot();
            GError     *rules_err = NULL;
            if (!pcv_suricata_ips_rules_apply(boot_sids, "system", &rules_err)) {
                PCV_LOG_WARN("suricata-ips",
                             "D13 Phase B Stage 2: 파생 룰셋 최초 생성 실패: %s "
                             "(IPS 는 룰 없이 기동될 수 있음)",
                             rules_err ? rules_err->message : "unknown");
                g_clear_error(&rules_err);
            }
            g_hash_table_destroy(boot_sids);
        } else if (pcv_suricata_ips_rules_stale()) {
                                                               
                                                                      
                                         
              
                                                             
                                                                  
                                                                
                                                                 
                                                                     
                                             
                                                            
                                     
            PCV_LOG_WARN("suricata-ips",
                         "D13 Phase B Stage 2: 파생 룰셋(%s)이 원본/정책보다 낡았다 — "
                         "데몬 정지 중 외부 변경으로 추정. 부팅 예산 때문에 자동 재생성은 "
                         "하지 않는다. `suricata.ips.drop.add`/`remove` 또는 "
                         "`suricata.rules.update` 를 1회 호출해 재생성할 것",
                         PCV_IPS_RULES_PATH);
        }

        if (pcv_suricata_ips_enable(ips_qn, ips_fo, &ips_err)) {
            PCV_LOG_INFO("suricata-ips", "D13 Phase B: inline IPS enabled at boot "
                         "(queue %u, %s)", ips_qn, ips_fo ? "fail-open" : "fail-closed");
        } else {
            PCV_LOG_WARN("suricata-ips", "D13 Phase B: inline IPS enable failed at boot: "
                         "%s (continuing degraded)", ips_err ? ips_err->message : "unknown");
            g_clear_error(&ips_err);
        }
    } else {
                                                                                
                                                           
                                                         
        pcv_suricata_ips_boot_stop_orphan_unit();
    }

      
                                                 
      
              
                                                    
                                               
                                                         
                                          
                                                     
                                          
                                                                
                                                 
      
                                          
                                          
                                                                      
                                                            
                                    
      
                     
                                           
                                                 
       
    {
                                                                
                                                
        extern void pcv_zfs_pool_lock_init(void);
        pcv_zfs_pool_lock_init();

        extern void pcv_anomaly_init(void);
        extern void pcv_predict_init(void);
        extern void pcv_healing_init(void);
        extern void pcv_agent_init(void);
        pcv_anomaly_init();
        pcv_predict_init();
        pcv_healing_init();
        pcv_agent_init();

                                                   
        extern void pcv_agent_configure(int provider, const gchar *model,
                                         const gchar *api_key, const gchar *endpoint);

          
                                                                  
          
                            
                                      
                                                      
                                           
           
        static const struct { int id; const gchar *prefix; } _ai_provs[] = {
            { 0, "claude" }, { 1, "openai" }, { 2, "gemini" }, { 3, "ollama" }
        };

          
                                                       
          
                       
                                                                                  
                                                                                  
          
                             
                                     
                                                     
                                          
           
                                                                     
                                                       
        for (gsize i = 0; i < G_N_ELEMENTS(_ai_provs); i++) {
            gchar key_k[64], model_k[64], ep_k[64];
            g_snprintf(key_k,   sizeof(key_k),   "%s_api_key",  _ai_provs[i].prefix);
            g_snprintf(model_k, sizeof(model_k),  "%s_model",    _ai_provs[i].prefix);
            g_snprintf(ep_k,    sizeof(ep_k),     "%s_endpoint", _ai_provs[i].prefix);
            gchar *api_key        = pcv_config_get_secret("ai", key_k,   "");
            const gchar *model    = pcv_config_get_string("ai", model_k,  NULL);
            const gchar *endpoint = pcv_config_get_string("ai", ep_k,     NULL);
                                                            
            if (api_key && *api_key)
                pcv_agent_configure(_ai_provs[i].id, model, api_key, endpoint);
            pcv_secure_free_str(&api_key);
        }
    }

      
                                                                     
      
              
                                              
                                                        
                                                         
                                                            
       
    pcv_alert_engine_init();

      
                                                         
      
                                                                
                                                        
                                                       
                                                
                                   
                                                                 
                                                     
                                                               
                                                                 
                                                      
                                                 
      
                                                    
                                                             
                     
       
    {
        GError *wp_err = NULL;
        if (pcv_webpush_init("/var/lib/purecvisor/pcv_webpush.db",
                             "/var/lib/purecvisor/webpush_vapid.pem", &wp_err)) {
                                                                        
                                                            
                                     
            const gchar *wp_sev = pcv_config_get_webpush_min_severity();
            pcv_webpush_set_policy(pcv_config_get_webpush_enabled(),
                                   g_strcmp0(wp_sev, "crit") == 0,
                                   pcv_config_get_webpush_contact());
            PCV_LOG_INFO("webpush", "SP2b: Web Push 배선 완료 (enabled=%s, min_severity=%s)",
                         pcv_config_get_webpush_enabled() ? "true" : "false", wp_sev);
        } else {
            PCV_LOG_WARN("webpush", "SP2b: Web Push 초기화 실패: %s (continuing degraded)",
                         wp_err ? wp_err->message : "unknown");
            g_clear_error(&wp_err);
        }
    }

      
                                                     
      
                                                       
                                           
                           
       
    pcv_process_monitor_init();
    STAGE_END("monitoring");

    STAGE_BEGIN("auth-templates");
      
                                      
      
                                
                                               
                                                           
                                                
                                    
       
    pcv_rbac_init("/var/lib/purecvisor/rbac.db");

      
                                         
                                                       
                                                                              
       
    pcv_vm_template_init();

      
                                      
      
                                             
                                
                                              
       
    pcv_backup_scheduler_init();
    pcv_security_group_resync_timer_init();                                            
                                                                    
                                                                   
                                                                          
                          
    pcv_qos_reconcile_timer_init();
    pcv_overlay_reconcile_timer_init();
    STAGE_END("auth-templates");

    STAGE_BEGIN("scheduler-proxy");
    pcv_bootstrap_init_scheduler_proxy();

    STAGE_END("scheduler-proxy");

                                                                    
                                    
      
                    
                                                           
                                           
                                       
      
               
                                                   
                                                
                                                                       
    STAGE_BEGIN("overlay-provision");
    pcv_bootstrap_init_runtime_network();

                                        
                                                            
                                                        
                                          
                                             
    pcv_qos_restore();

    STAGE_END("overlay-provision");

                                                              
    {
        gint64 total_ms = (g_get_monotonic_time() - init_total_start) / 1000;
        g_message("[init] All %d stages completed in %ldms", stage_num, (long)total_ms);
    }

                                                                    
                                  
      
                                          
                                          
                                                                       
    {
        gint health_errors = 0;

                                 
        if (!libvirt_degraded) {
            virConnectPtr test_conn = virt_conn_pool_acquire();
            if (!test_conn) {
                g_warning("[init] HEALTH: libvirt connection pool unavailable");
                health_errors++;
            } else {
                virt_conn_pool_release(test_conn);
            }
        }

                            
        gint lock_count = pcv_vm_state_get_lock_count();
        if (lock_count < 0) {
            g_warning("[init] HEALTH: vm_state DB unavailable");
            health_errors++;
        }

                             
        if (!g_file_test(pcv_config_get_socket_path(), G_FILE_TEST_EXISTS)) {
            g_warning("[init] HEALTH: UDS socket not found at %s",
                      pcv_config_get_socket_path());
            health_errors++;
        }

        if (health_errors > 0)
            g_warning("[init] Degraded startup: %d service(s) unavailable", health_errors);
        else
            g_message("[init] Health self-check passed");
    }

                                                                    
                         
      
                      
                                            
                                              
                                           
                                                    
                                      
                                         
                    
      
                                                 
      
                          
                              
                                                     
                           
                                
                             
                                    
                                                           
                                                                       

      
                   
                                                  
                                                               
       
    loop = g_main_loop_new(NULL, FALSE);

                                                            
    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);
                                                                           
    g_unix_signal_add(SIGHUP, on_sighup_received, NULL);

      
                                               
      
                                 
                                                       
                                    
                                                                    
                                    
                                     
      
                                               
                                         
       
                                                         
                                                         
    pcv_drain_notify_ready();

                                                   
                                       
    gint rest_port = pcv_config_get_rest_port();

                                                
    g_message("═══════════════════════════════════════════════════════");
    g_message("  PureCVisor Engine v%s — All systems operational", PCV_PRODUCT_VERSION);
    g_message("═══════════════════════════════════════════════════════");
    g_message("  UDS  : /var/run/purecvisor/daemon.sock (io_uring)");
    g_message("  REST : http://%s:%d/api/v1/",
              rest_transport.plaintext_host, rest_port);
    g_message("  Web  : http://%s:%d/ui/",
              rest_transport.plaintext_host, rest_port);
    g_message("  WS   : ws://%s:%d/api/v1/ws/events",
              rest_transport.plaintext_host, rest_port);
    g_message("  RPC  : 130 methods registered");
    g_message("  REST : 88 endpoints active");
    if (libvirt_degraded)
        g_message("  MODE : *** DEGRADED (libvirt unavailable) ***");
    g_message("═══════════════════════════════════════════════════════");
    g_message("Daemon is running. Waiting for requests...");

                                                                 
      
                                                           
                                               
                               
      
                                                 
                                                     
                                                          
                
       
    {
        guint64 watchdog_usec = pcv_drain_get_watchdog_usec();
        if (watchdog_usec > 0) {
            guint64 interval_us = watchdog_usec / 2;
            if (interval_us < 5000000) interval_us = 5000000;              
            g_atomic_int_set(&g_watchdog_stop, 0);
            g_watchdog_thread = g_thread_new("pcv-watchdog",
                _watchdog_thread_func, GSIZE_TO_POINTER((gsize)interval_us));
            g_message("[main] systemd watchdog enabled via dedicated thread "
                      "(interval=%.1fs, timeout=%luus)",
                      interval_us / 1000000.0,
                      (unsigned long)watchdog_usec);
        }
    }

                                                                     
                                                      
                                                                 
                                                          
                                           
    pcv_bpf_seal();
    g_message("[init] BPF 로드 창 봉인(seal) — 런타임 로드 금지");

                                                      
                                                  
                                    
    g_main_loop_run(loop);

                                                                    
                                           
      
                                                 
                                                
      
                 
                                                    
      
                
                                    
                                     
                                            
                                  
      
                    
                                                             
                                              
                                              
                                                                       
    g_message("Cleaning up resources before exit...");

                                                              
                                                                    
                                                 
    if (g_watchdog_thread) {
        g_atomic_int_set(&g_watchdog_stop, 1);
        g_thread_join(g_watchdog_thread);                      
        g_watchdog_thread = NULL;
        g_message("[main] watchdog thread stopped");
    }

                                                     

                    
    pcv_grpc_server_stop();

                                                  
    if (rest_server) {
        pcv_rest_server_stop(rest_server);
        g_object_unref(rest_server);                                  
    }

                                                    
    pcv_jwt_shutdown();

      
                                
      
                           
                                   
                                                        
                                               
      
              
                                                                   
                                                          
                       
       
    g_object_unref(server);                                    
    g_object_unref(dispatcher);                       
    g_object_unref(conn);                                                       
    g_main_loop_unref(loop);                                    

                                                 
    pcv_bootstrap_shutdown_cluster_stack();                   
    pcv_backup_scheduler_shutdown();                      
    pcv_security_group_resync_timer_shutdown();                             
    if (qos_reconcile_timer_id > 0)
        g_source_remove(qos_reconcile_timer_id);                                        
    if (qos_metrics_timer_id > 0)
        g_source_remove(qos_metrics_timer_id);                                        
                                                        
                                                  
                                                                  
                                                               
                                            
                                                           
    pcv_qos_chaos_clear();
                                                                
                                                                           
    pcv_qos_reconcile_timer_shutdown();                                                               
    pcv_overlay_reconcile_timer_shutdown();                                       
    pcv_vm_template_shutdown();                   
    pcv_rbac_shutdown();                              
                                                                   
                                                          
                                                                    
    extern void pcv_healing_shutdown(void);
    pcv_healing_shutdown();
                                                                    
                                                                  
                                                                     
                                           
                                                                
                                                      
                                                            
    pcv_suricata_ips_shutdown_signal();
    pcv_process_monitor_shutdown();                                 
    pcv_alert_engine_shutdown();                                 
                                                                
                                                       
                                                                 
                                                                          
    pcv_webpush_shutdown();
    pcv_ebpf_telemetry_shutdown();                             
                                                                        
                                                                
                                                                 
                                                                  
                                              
                                                                      
    pcv_suricata_ips_disable(NULL);
    pcv_suricata_health_stop();                                                                                                                                       
    pcv_bpf_consumer_stop();                                                         
    pcv_suricata_eve_tail_stop();                                                                                                                                                            
    pcv_bpf_shutdown();                                                                 
    pcv_iscsi_shutdown();                            
    pcv_vpc_shutdown();                                                                      
    pcv_ovn_shutdown();                                                         
    pcv_overlay_shutdown();                         

                                                  
    pcv_drain_shutdown();                                         
    shutdown_pending_state_machine();                                            
    cmap_shutdown();                                         
    virt_conn_pool_shutdown();                                    

      
                                                  
      
                 
                                                          
                                             
                                        
                                                             
                                                                          
       
    pcv_job_queue_shutdown();                         
    pcv_config_shutdown();                                      
    pcv_worker_pool_shutdown();                        
    pcv_spawn_launcher_shutdown();                                
    pcv_log_shutdown();                                

    g_message("PureCVisor Engine exited cleanly.");
    return 0;
}
