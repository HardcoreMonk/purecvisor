   
                      
                                                                
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                        
                                                         
                                                 
                                                             
                                                    
                                                        
                                              
  
          
                                                               
                                                   
                                       
  
            
                   
                                                         



                                                                

                                                                                                
  
                             


                                                               
                                                                
                                                                                
                                                   
                                                                                    
                                                                          
  


                                             

                                                               
                                       
  
         






                                                     
   

#include "api/drain.h"



static GThread *g_producer_thread;
static gint g_producer_stop;
static void
_producer_wait(guint milliseconds)
{
    for (guint elapsed = 0; elapsed < milliseconds &&
         !g_atomic_int_get(&g_producer_stop); elapsed += 50)
        g_usleep(MIN(50U, milliseconds - elapsed) * 1000U);
}

#include <glib.h>
#include <libvirt/libvirt.h>
#include <string.h>
#include <time.h>

                                  
#include "modules/core/cpu_allocator.h"
#include "modules/ai/self_healing.h"                                       
#include "../network/security_group.h"                             
#include "../network/tenant_overlay.h"                                     
#include "../network/pcv_qos.h"                                                                            
#include "../../utils/pcv_worker_pool.h"
#include "modules/daemons/pcv_undefine_debounce.h"                                         
#include "../../utils/pcv_log.h"                                             
#include "pcv_vm_death_class.h"                                 
#include "modules/daemons/pcv_virt_listener_guard.h"
                                                

                                                               
                          
  
                                                     
                                                     
                                                    
                                        
  
                                
                                             
  
        
                                               
                         
                                                                              
  
                                            
                                                       
                                                                  
#define REBOOT_LOOP_WINDOW_SEC  600
#define REBOOT_LOOP_THRESHOLD   5
#define REBOOT_LOOP_RING        5

                                                       
                                                                    
typedef struct {
    gint64 stop_us[REBOOT_LOOP_RING];
    gint   pos;
    gint   count;
    gint64 last_alert_us;                    
} VmRebootTracker;

static GHashTable *g_reboot_trackers = NULL;                                       
static GMutex      g_reboot_mu;
static gboolean    g_reboot_init = FALSE;

                                                                   
                                                         
static void
_reboot_tracker_init_once(void)
{
    if (g_reboot_init) return;
    g_mutex_init(&g_reboot_mu);
    g_reboot_trackers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    g_reboot_init = TRUE;
}

   
                  
                                                        
                                            
                     
                        
  
                                            
                                                                       
   
static void
_track_vm_stop(const gchar *uuid, const gchar *vm_name)
{
    if (!uuid) return;
    _reboot_tracker_init_once();

    g_mutex_lock(&g_reboot_mu);
    VmRebootTracker *t = g_hash_table_lookup(g_reboot_trackers, uuid);
    if (!t) {
        t = g_new0(VmRebootTracker, 1);
        g_hash_table_insert(g_reboot_trackers, g_strdup(uuid), t);
    }

    gint64 now = g_get_monotonic_time();
    gint64 window_us = (gint64)REBOOT_LOOP_WINDOW_SEC * G_USEC_PER_SEC;

                        
                                                       
                                                        
                                                                
    t->stop_us[t->pos] = now;
    t->pos = (t->pos + 1) % REBOOT_LOOP_RING;
    if (t->count < REBOOT_LOOP_RING) t->count++;

                           
    gint recent = 0;
    for (gint i = 0; i < t->count; i++) {
        if (t->stop_us[i] > 0 && (now - t->stop_us[i]) <= window_us) recent++;
    }

                           
    gboolean alert = (recent >= REBOOT_LOOP_THRESHOLD) &&
                     (now - t->last_alert_us > 300 * G_USEC_PER_SEC);
    if (alert) t->last_alert_us = now;
    g_mutex_unlock(&g_reboot_mu);

    if (alert) {
        g_warning("🔁 [vm-reboot-loop] VM %s (%s) stopped %d times within %ds — possible boot failure or OOM",
                  vm_name ? vm_name : "(unknown)", uuid, recent, REBOOT_LOOP_WINDOW_SEC);
                                                                 
                                                                          
        pcv_healing_on_anomaly("vm-reboot-loop", (gdouble)recent, 99.0, 0.0, NULL);
    }
}

void init_virt_events_daemon(void);

                                                            
                                  
                                                               

   
                                                                        
                                                              
  
                                                        
                                                  
                                  
  
                                                                              
                                                                           
                                                                  
                                                       
                                                                             
                               
   
                                                                    
                                                                  
                                                                   
                  
                                                                
                                            
typedef struct { gchar *uuid; gchar *name; gboolean anomaly; } VmDeathPayload;

static gboolean handle_vm_death_in_main_thread(gpointer user_data) {
    VmDeathPayload *p = user_data;

                                                                   
                                                                   
    if (p->anomaly) {
        g_warning("🚨 [Self-Healing] VM '%s' (uuid=%s) 크래시/실패 감지 — 자원 회수",
                  (p->name && *p->name) ? p->name : "?", p->uuid ? p->uuid : "?");
    } else {
        g_message("[lifecycle] VM '%s' (uuid=%s) 정상/의도적 종료 — 자원 회수",
                  (p->name && *p->name) ? p->name : "?", p->uuid ? p->uuid : "?");
    }

                                               
                                                                        
                                                      
    if (global_allocator != NULL && p->name && *p->name) {
        cpu_allocator_free_vm_cores(global_allocator, p->name);
    }

                                                                       
                                                                         
                                                                
                                                                    
    if (p->anomaly) {
        pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, p->uuid);
    } else {
        g_debug("[Self-Healing] VM '%s' 의도적/정상 종료 — heal 스킵(자원만 회수)",
                (p->name && *p->name) ? p->name : "?");
    }

                                                           
                                   

                                                                 
    g_free(p->uuid);
    g_free(p->name);
    g_free(p);

    return G_SOURCE_REMOVE;                  
}


                                                            
                                
                                                               

                                                            
                                                                  
                                                     
                                                      
static void
_sg_event_sync_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    const gchar *vm = task_data;
    if (vm) pcv_security_group_sync_vm(vm);
    g_task_return_boolean(task, TRUE);
}

                                                          
                                                                  
                                                        
                                                      
                                          
static void
_schedule_sg_sync(const char *vm_name)
{
    if (!vm_name || !pcv_security_group_vm_is_bound(vm_name))
        return;
    GTask *sgt = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(sgt, g_strdup(vm_name), g_free);
    pcv_worker_pool_push(sgt, _sg_event_sync_worker);
    g_object_unref(sgt);                                                            
}

                                                                 
                                                                 
                                                       
                                              
static void
_sg_event_unbind_all_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    const gchar *vm = task_data;
    if (vm) pcv_security_group_unbind_vm_all(vm);
    g_task_return_boolean(task, TRUE);
}

                                                                          
                                                                
                                   
                                                       
                                                    
static void
_schedule_sg_unbind_all(const char *vm_name)
{
    if (!vm_name || !pcv_security_group_vm_is_bound(vm_name))
        return;
    GTask *sgt = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(sgt, g_strdup(vm_name), g_free);
    pcv_worker_pool_push(sgt, _sg_event_unbind_all_worker);
    g_object_unref(sgt);                                              
}

                                                                           
                                                                     
                                                             
                                        
                                                   
                                             
static void
_overlay_event_cleanup_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    const gchar *vm = task_data;
    if (vm) pcv_tenant_overlay_on_vm_gone(vm);
    g_task_return_boolean(task, TRUE);
}

                                                                   
                                                               
                                                                     
                          
                                                         
                                 
static void
_schedule_overlay_cleanup(const char *vm_name)
{
    if (!vm_name || !pcv_tenant_overlay_vm_in_any_tenant(vm_name))
        return;
    GTask *ovt = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(ovt, g_strdup(vm_name), g_free);
    pcv_worker_pool_push(ovt, _overlay_event_cleanup_worker);
    g_object_unref(ovt);                                                            
}

                                                                     
                                                            
                                               
                                                            
                                                      
                                          
typedef struct {
    gchar *vm;
    gchar *tenant;
    gchar *iface;
} QosCleanupCtx;

                                         
                                                    
  
                                                              
                                                
                                                                
                                                 
                                  
static void
_qos_cleanup_ctx_free(gpointer p)
{
    QosCleanupCtx *ctx = p;
    if (!ctx) return;
    g_free(ctx->vm);
    g_free(ctx->tenant);
    g_free(ctx->iface);
    g_free(ctx);
}

                                                        
                                                                   
                                                         
                                                 
                                                     
                                                             
static void
_qos_event_cleanup_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    QosCleanupCtx *ctx = task_data;
    if (ctx) {
        GError *err = NULL;
        if (!pcv_qos_remove_vm(ctx->iface, ctx->tenant, ctx->vm, &err)) {
            g_warning("[qos] on_vm_gone 정리 실패 tenant=%s vm=%s: %s "
                      "(reconcile 이 고아 수거)", ctx->tenant, ctx->vm,
                      err ? err->message : "unknown");
            g_clear_error(&err);
        }
    }
    g_task_return_boolean(task, TRUE);
}

                                                                
                                                                     
                                                      
                                                      
                                                           
                                                    
                                    
                                                     
                                                   
                                              
static void
_schedule_qos_cleanup(const char *vm_name)
{
    if (!vm_name) return;

    gchar *tenant = NULL, *iface = NULL;
    if (!pcv_qos_lookup_applied(vm_name, &tenant, &iface)) {
        g_message("[qos] VM '%s' gone — apply 추적 없음(데몬 재시작 등), "
                  "reconcile 백스톱에 맡김", vm_name);
        return;
    }

    QosCleanupCtx *ctx = g_new0(QosCleanupCtx, 1);
    ctx->vm     = g_strdup(vm_name);
    ctx->tenant = tenant;               
    ctx->iface  = iface;                

    GTask *qt = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(qt, ctx, _qos_cleanup_ctx_free);
    pcv_worker_pool_push(qt, _qos_event_cleanup_worker);
    g_object_unref(qt);                                                            
}

                                                               
                                  
  
                                                            
                                                  
                                                  
                                                   
                                                        
                                                  
  
                                                                     
                                                  
                                                       
                                                     
                                 
  
                                                                 
                                                  
                                                           
                        
  
                                                                          
                                                                       
            
                                                                  

                                                 
#define PCV_UNDEFINE_DEBOUNCE_MS 2000                                          
static PcvUndefineDebounce *g_undefine_debounce = NULL;

                                                                    
static PcvUndefineDebounce *_undefine_debounce(void) {
    if (!g_undefine_debounce) g_undefine_debounce = pcv_undefine_debounce_new();
    return g_undefine_debounce;
}

                                                                      
                                                                             
                                                         
                                                    
static void _ovl4_reclaim(const gchar *uuid, const gchar *vm_name) {
                                                                             
                                                                          
                                                          
                                                               
    if (global_allocator && vm_name && *vm_name)
        cpu_allocator_free_vm_cores(global_allocator, vm_name);
    if (vm_name) {
        _schedule_overlay_cleanup(vm_name);
        _schedule_qos_cleanup(vm_name);
        _schedule_sg_unbind_all(vm_name);                                            
    }
    PCV_LOG_INFO("virt_events",
        "[OVL-4] undefine 확정(창 내 DEFINED 없음) — 자원 회수 vm='%s' uuid='%s'",
        vm_name ? vm_name : "?", uuid ? uuid : "?");
}

                                          
                                                            
typedef struct { gchar *uuid; gchar *name; } Ovl4UndefPayload;

                                                                         
static void _ovl4_payload_free(gpointer data) {
    Ovl4UndefPayload *p = data;
    if (!p) return;
    g_free(p->uuid); g_free(p->name); g_free(p);
}

                                                                     
                                                                  
                                                           
                                                          
                                                           
                              
                                                      
                                                     
static gboolean _ovl4_domain_still_active(const char *uuid) {
    if (!uuid || !*uuid) return FALSE;
    virConnectPtr c = virConnectOpenReadOnly("qemu:///system");
    if (!c) return FALSE;                                    
    gboolean active = FALSE;
    virDomainPtr d = virDomainLookupByUUIDString(c, uuid);
    if (d) {
        active = (virDomainIsActive(d) == 1);
        virDomainFree(d);
    }
    virConnectClose(c);
    return active;
}

                                                              
                                                    
                                               
                                                
                                    
                                                      
                                           
static gboolean _ovl4_reclaim_main(gpointer user_data) {
    Ovl4UndefPayload *p = user_data;
    _ovl4_reclaim(p->uuid, p->name);
    _ovl4_payload_free(p);
    return G_SOURCE_REMOVE;
}

                                                               
                                                   
                                                         
                                                    
                                                          
                                                              
                                                      
                                 
static void _ovl4_expire_liveness_worker(GTask *task, gpointer src,
                                         gpointer task_data, GCancellable *c) {
    (void)src; (void)c;
    Ovl4UndefPayload *p = task_data;
    if (_ovl4_domain_still_active(p->uuid)) {
        PCV_LOG_INFO("virt_events",
            "[OVL-4] undefine 확정이나 도메인 여전히 active(transient-undefine) — "
            "회수 스킵 vm='%s' uuid='%s'",
            p->name ? p->name : "?", p->uuid ? p->uuid : "?");
    } else {
                                                                  
                                                             
        Ovl4UndefPayload *q = g_new0(Ovl4UndefPayload, 1);
        q->uuid = g_strdup(p->uuid);
        q->name = g_strdup(p->name);
        pcv_drain_invoke(NULL, _ovl4_reclaim_main, q);
    }
    g_task_return_boolean(task, TRUE);
}

                                                        
                                                          
                                                                 
                                                        
                                                   
static gboolean _ovl4_undefine_expire(gpointer user_data) {
    Ovl4UndefPayload *p = user_data;
    gchar *name = pcv_undefine_debounce_take_expired(_undefine_debounce(), p->uuid);
    if (name) {
        Ovl4UndefPayload *w = g_new0(Ovl4UndefPayload, 1);
        w->uuid = g_strdup(p->uuid);
        w->name = name;                                     
        GTask *t = pcv_drain_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(t, w, _ovl4_payload_free);
        pcv_worker_pool_push(t, _ovl4_expire_liveness_worker);
        g_object_unref(t);                                          
    }
    return G_SOURCE_REMOVE;
}

                                                             
                                                       
                            
static gboolean _ovl4_on_undefined_main(gpointer user_data) {
    Ovl4UndefPayload *p = user_data;                                               
    guint src = pcv_drain_timeout(PCV_UNDEFINE_DEBOUNCE_MS,
                                   _ovl4_undefine_expire, p, _ovl4_payload_free);
    guint old = pcv_undefine_debounce_note_undefined(_undefine_debounce(), p->uuid, p->name, src);
    if (old) g_source_remove(old);                                                             
    return G_SOURCE_REMOVE;
}

                                                     
                                                       
                                           
static gboolean _ovl4_on_defined_main(gpointer user_data) {
    gchar *uuid = user_data;
    guint tok = pcv_undefine_debounce_note_defined(_undefine_debounce(), uuid);
    if (tok) g_source_remove(tok);                                                      
    g_free(uuid);
    return G_SOURCE_REMOVE;
}

   
                                                    
                                                                   
  
                                                       
                                                         
                                                         
                                                       
   
static int domain_lifecycle_cb(virConnectPtr conn, virDomainPtr dom,
                               int event, int detail, void *opaque)
{
    (void)conn; (void)opaque;                                                 

    char uuid[VIR_UUID_STRING_BUFLEN] = "";
    const char *vm_name = virDomainGetName(dom);
    const gboolean uuid_valid = virDomainGetUUIDString(dom, uuid) == 0;

                                                     
                                                     
                                                               
    if (event == VIR_DOMAIN_EVENT_STARTED ||
        event == VIR_DOMAIN_EVENT_STOPPED ||
        event == VIR_DOMAIN_EVENT_SHUTDOWN ||
        event == VIR_DOMAIN_EVENT_CRASHED)
        _schedule_sg_sync(vm_name);

                                                                          
                                                           
                                                                
    if (event == VIR_DOMAIN_EVENT_UNDEFINED) {
        if (!uuid_valid) return 0;
        Ovl4UndefPayload *p = g_new0(Ovl4UndefPayload, 1);
        p->uuid = g_strdup(uuid);
        p->name = g_strdup(vm_name ? vm_name : "");
        pcv_drain_invoke(NULL, _ovl4_on_undefined_main, p);
        return 0;
    }
                                                                            
    if (event == VIR_DOMAIN_EVENT_DEFINED) {
        if (!uuid_valid) return 0;
        pcv_drain_invoke(NULL, _ovl4_on_defined_main, g_strdup(uuid));
        return 0;
    }

                                                                        
    if (event == VIR_DOMAIN_EVENT_STARTED) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-started RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)",
              uuid_valid ? uuid : "(unavailable)");
        return 0;
    }

                                                                        
    if (event == VIR_DOMAIN_EVENT_STOPPED || event == VIR_DOMAIN_EVENT_SHUTDOWN) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-stopped RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)",
              uuid_valid ? uuid : "(unavailable)");
                                                                                   
                                                                          
                                                    
        if (uuid_valid && pcv_vm_death_is_anomaly(event, detail))
            _track_vm_stop(uuid, vm_name);
    }

                                                               
    if (event == VIR_DOMAIN_EVENT_STOPPED || event == VIR_DOMAIN_EVENT_CRASHED) {

                                                                 
                                                                          
                                          
        _schedule_overlay_cleanup(vm_name);

                                                                   
                                                 
                                                                       
        _schedule_qos_cleanup(vm_name);

        if (uuid_valid) {

                                                 
                                                                       
                                                                              
                                                 
                                                                               
                                                                       
            VmDeathPayload *p = g_new0(VmDeathPayload, 1);
            p->uuid = g_strdup(uuid);
            p->name = g_strdup(vm_name ? vm_name : "");
            p->anomaly = pcv_vm_death_is_anomaly(event, detail);                                    

                                                                
            pcv_drain_invoke(NULL, handle_vm_death_in_main_thread, p);
        }
    }
    
    return 0;
}

                                                         
                                                         
                                                                    
                                                                    
                                                     
                           
static void
_domain_device_cb(virConnectPtr conn, virDomainPtr dom,
                  const char *devAlias, void *opaque)
{
    (void)conn; (void)devAlias; (void)opaque;
                                                                               
    _schedule_sg_sync(virDomainGetName(dom));
}

                                                           
                                                            
                                                       
                                       
static void
_register_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    *added_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_ADDED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    *removed_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    if (*added_id < 0 || *removed_id < 0)
        g_warning("⚠️ [Events] device 콜백 등록 실패 [ADDED=%s REMOVED=%s] — NIC 핫플러그 "
                  "즉시성 저하 (I2-R1 주기 resync 로 fallback)",
                  *added_id   < 0 ? "FAIL" : "ok",
                  *removed_id < 0 ? "FAIL" : "ok");
    else
        g_message("🛡️ [Events] device add/remove 리스너 등록 "
                  "(NIC 핫플러그 즉시 SG 재동기화)");
}

                                                                 
                                                       
static void
_deregister_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    if (*added_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *added_id);
        *added_id = -1;
    }
    if (*removed_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *removed_id);
        *removed_id = -1;
    }
}

#define PCV_VIRT_LISTENER_RECONNECT_DELAY_SEC 5U






  
                                                  

   
typedef struct {
    virConnectPtr connection;
    gint lifecycle_callback_id;
    gint device_added_callback_id;
    gint device_removed_callback_id;
} PcvVirtEventListener;

static void
_event_listener_reset(PcvVirtEventListener *listener)
{
    listener->connection = NULL;
    listener->lifecycle_callback_id = -1;
    listener->device_added_callback_id = -1;
    listener->device_removed_callback_id = -1;
}


static void
_event_listener_close(PcvVirtEventListener *listener)
{
    if (!listener)
        return;
    if (!listener->connection) {
        _event_listener_reset(listener);
        return;
    }

    if (listener->lifecycle_callback_id >= 0) {
        virConnectDomainEventDeregisterAny(listener->connection,
                                           listener->lifecycle_callback_id);
        listener->lifecycle_callback_id = -1;
    }
    _deregister_device_callbacks(listener->connection,
                                 &listener->device_added_callback_id,
                                 &listener->device_removed_callback_id);
    virConnectClose(listener->connection);
    _event_listener_reset(listener);
}






                                                             


static gboolean
_event_listener_open(PcvVirtEventListener *listener)
{
    g_return_val_if_fail(listener != NULL, FALSE);
    _event_listener_reset(listener);

    listener->connection = virConnectOpen("qemu:///system");
    if (!listener->connection)
        return FALSE;

    gint alive_result = virConnectIsAlive(listener->connection);
    unsigned long libvirt_version = 0UL;
    gint rpc_probe_result = alive_result == 1
        ? virConnectGetLibVersion(listener->connection, &libvirt_version)
        : -1;
    if (pcv_virt_listener_connection_lost(TRUE, alive_result,
                                          rpc_probe_result)) {
        g_warning("⚠️ [Events] newly opened libvirt connection is unusable "
                  "(is_alive=%d rpc_probe=%d)", alive_result,
                  rpc_probe_result);
        _event_listener_close(listener);
        return FALSE;
    }


    gint keepalive_result = virConnectSetKeepAlive(listener->connection, 5, 3);
    if (keepalive_result < 0)
        g_warning("⚠️ [Events] libvirt keepalive setup failed; periodic health probe remains active");
    else if (keepalive_result == 1)
        g_message("[Events] libvirt peer does not support keepalive; periodic health probe is authoritative");

    listener->lifecycle_callback_id = virConnectDomainEventRegisterAny(
        listener->connection,
        NULL,
        VIR_DOMAIN_EVENT_ID_LIFECYCLE,
        VIR_DOMAIN_EVENT_CALLBACK(domain_lifecycle_cb),
        NULL,
        NULL);
    if (listener->lifecycle_callback_id < 0) {
        g_warning("⚠️ [Events] failed to register libvirt lifecycle callback");
        _event_listener_close(listener);
        return FALSE;
    }

    _register_device_callbacks(listener->connection,
                               &listener->device_added_callback_id,
                               &listener->device_removed_callback_id);
    return TRUE;
}
















static gpointer libvirt_listener_watchdog_thread(gpointer data) {
    (void)data;


    if (virInitialize() < 0 || virEventRegisterDefaultImpl() < 0) {
        g_critical("🚨 [Events] Failed to initialize Libvirt default event loop. "
                   "Lifecycle monitoring disabled.");
        return NULL;
    }

    PcvVirtEventListener listener;
    _event_listener_reset(&listener);
    gboolean connected_once = FALSE;
    guint reconnect_attempt = 0U;
    gint64 disconnected_at_us = 0;

    while (!g_atomic_int_get(&g_producer_stop)) {
        if (!listener.connection) {
            if (connected_once || reconnect_attempt > 0U)
                _producer_wait(PCV_VIRT_LISTENER_RECONNECT_DELAY_SEC * 1000U);
            if (g_atomic_int_get(&g_producer_stop)) break;
            if (reconnect_attempt < G_MAXUINT)
                reconnect_attempt++;

            if (!_event_listener_open(&listener)) {
                if (reconnect_attempt == 1U || reconnect_attempt % 6U == 0U)
                    g_warning("⚠️ [Events] listener connect attempt %u failed; "
                              "retrying in %us", reconnect_attempt,
                              PCV_VIRT_LISTENER_RECONNECT_DELAY_SEC);
                continue;
            }

            if (connected_once) {
                gint64 downtime_sec = disconnected_at_us > 0
                    ? (g_get_monotonic_time() - disconnected_at_us) / G_USEC_PER_SEC
                    : 0;
                g_message("🛡️ [Events] Reconnected to libvirt after %" G_GINT64_FORMAT
                          "s (attempt=%u callback_id=%d)",
                          downtime_sec, reconnect_attempt,
                          listener.lifecycle_callback_id);
            } else {
                g_message("🛡️ [Events] Libvirt Lifecycle Listener & Self-Healing "
                          "Daemon Started (callback_id=%d).",
                          listener.lifecycle_callback_id);
            }
            connected_once = TRUE;
            reconnect_attempt = 0U;
        }

        if (!pcv_virt_listener_guard_wait(
                PCV_VIRT_LISTENER_HEALTHCHECK_INTERVAL_MS)) {
            g_critical("🚨 [Events] listener watchdog interval is invalid; "
                       "lifecycle monitoring disabled");
            _event_listener_close(&listener);
            return NULL;
        }
        if (g_atomic_int_get(&g_producer_stop)) break;

        gint alive_result = virConnectIsAlive(listener.connection);
        unsigned long libvirt_version = 0UL;
        gint rpc_probe_result = alive_result == 1
            ? virConnectGetLibVersion(listener.connection, &libvirt_version)
            : -1;
        if (!pcv_virt_listener_connection_lost(listener.connection != NULL,
                                               alive_result,
                                               rpc_probe_result))
            continue;

        g_warning("⚠️ [Events] libvirt listener lost "
                  "(is_alive=%d rpc_probe=%d); "
                  "reconnecting",
                  alive_result, rpc_probe_result);
        disconnected_at_us = g_get_monotonic_time();
        _event_listener_close(&listener);
        reconnect_attempt = 0U;
    }


    _event_listener_close(&listener);
    return NULL;
}


                                                            
                      
                                                               

   
                                             
  
                                                   
                                                
   
void init_virt_events_daemon(void) {
    GError *error = NULL;
    g_atomic_int_set(&g_producer_stop, FALSE);
    g_producer_thread = g_thread_try_new("libvirt-events",
                                       libvirt_listener_watchdog_thread,
                                       NULL, &error);
    
    if (!g_producer_thread) {
        g_critical("Failed to create Libvirt events daemon thread: %s", error->message);
        g_error_free(error);
    }
}



void
pcv_virt_events_shutdown(void)
{
    g_atomic_int_set(&g_producer_stop, TRUE);
    if (g_producer_thread) {
        g_thread_join(g_producer_thread);
        g_producer_thread = NULL;
    }
}
