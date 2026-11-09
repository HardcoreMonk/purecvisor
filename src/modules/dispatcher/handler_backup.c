   
                         
                                                               
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                   
                                                   
                                                 
                                                             
  
                                                       
                                                       
                                               
                                                             
                                                                
                                                         
                              
                                                       
                                                                  
                                       
                                                                            
                                                                                 
                                                                        
                                                           
                                                              
                                                                      
  
            
                                                                        
                                                                                                     
  
                      
                                                                          
                                                                           
                                         
                                                                         
                                                                          
                              
                                                                            
                              
                                                                     
                                               
  
                             
                                                                         
                                                 
                                               
                                                             
                                                           
  
         
                                                          
                                             
                                                    
                                                             
                                                                     
  
          
                                                        
                                                        
                          
   

  
                     
  
                                                                        
                                                              
                                                         
                                                                                
                                                            
                                                                        
                                                            
                                                                                  
                                                                                               
                                                                                
   
#include "api/drain.h"
#include "handler_backup.h"
#include "rpc_utils.h"
#include "../backup/backup_scheduler.h"
#include "../backup/replication_hostkey.h"                               
#include "../audit/pcv_audit.h"
#include "../../api/uds_server.h"
#include "../../api/ws_server.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_job_queue.h"
#include "../../utils/pcv_validate.h"
#include "../core/vm_state.h"                                                          
#include "handler_vm_start.h"                                          
#include "rpc_completion.h"                                      

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "api/snapshot_verify_probe.h"
#include "utils/pcv_spawn.h"

  
                                            
                                                      
                                                       
   
#define BACKUP_HANDLER_LOG "backup_handler"

                                                              
                    
                                                                 

   
                            
                                                        
                                                   
                                                                              
                 
                       
                           
  
                                                               
                                                               
                                                  
  
           
                                                      
                                                
                                                               
  
                                
                                   
   
void handle_backup_policy_set(JsonObject       *params,
                               const gchar      *rpc_id,
                               UdsServer        *server,
                               GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    gint interval_hours  = 0;
    gint retention_count = 0;

                                                                 
                                                           
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (params && json_object_has_member(params, "interval_hours"))
        interval_hours = (gint)json_object_get_int_member(params, "interval_hours");

    if (params && json_object_has_member(params, "retention_count"))
        retention_count = (gint)json_object_get_int_member(params, "retention_count");

                                                                      
                                                          
                                                         
                                                                  
    if (!vm_name || interval_hours < 1 || interval_hours > 8760
        || retention_count < 1 || retention_count > 365) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid params: vm_name required, interval_hours 1~8760, retention_count 1~365");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
                                             
                                                         
                                                                   
    if (g_strcmp0(vm_name, "*") != 0 && !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_set(vm_name, interval_hours,
                                         retention_count, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to set policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

                                                                      
                                                                    
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);                                  
}

                                                              
                     
                                                                 

   
                             
                                                 
                             
                 
                       
                           
  
                  
                                                                                    
                                                                       
                                                    
   
void handle_backup_policy_list(JsonObject       *params __attribute__((unused)),
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
                                                         
                                                               
    GPtrArray *policies = pcv_backup_policy_list();

                                              
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(policies, i);
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", p->vm_name);
        json_object_set_int_member(obj, "interval_hours", p->interval_hours);
        json_object_set_int_member(obj, "retention_count", p->retention_count);
        json_object_set_boolean_member(obj, "enabled", p->enabled);
        json_array_add_object_element(arr, obj);
    }
    g_ptr_array_unref(policies);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);                         
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                              
                       
                                                                 

   
                               
                                                      
                                         
                 
                       
                           
  
                                                     
                                                                    
                                                           
   
void handle_backup_policy_delete(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_delete(vm_name, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to delete policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                              
                 
                                                                 

   
                         
                                                       
                                         
                 
                       
                           
  
                   
                                                         
                                                                      
                                                               
   
void handle_backup_history(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GPtrArray *snaps = pcv_backup_history(vm_name);

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < snaps->len; i++) {
        json_array_add_string_element(arr,
            (const gchar *)g_ptr_array_index(snaps, i));
    }
    g_ptr_array_unref(snaps);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                              
                                          
  
                             
                                                                        
                                                                         
                                                              
                                                                    
  
                        
                                          
                                    
                                                                 

   
                   
                              
  
                                                          
                                                      
                                                     
  
                  
                                                                     
                                                                 
                                                 
  
         
                                                                           
                                                            
                                                                         
   
typedef struct {
    gchar *vm_name;                                   
    gchar *snapshot_name;                              
    gchar *job_id;                                                           
    PcvJobRef *job_ref;                                     
    gboolean  was_running;                               
    gboolean  rollback_ok;                
    gchar    *rollback_error;                       
    PcvVmLock *lock;                                                                              
    gchar    *actor;                                                                        
} RestoreTaskData;

                                                           
                                          
                                                      
static void _restore_task_data_free(gpointer p)
{
    RestoreTaskData *d = (RestoreTaskData *)p;
    if (!d) return;                                         
    g_free(d->vm_name);
    g_free(d->snapshot_name);
    pcv_job_unref(d->job_ref);
    g_free(d->job_id);
    g_free(d->rollback_error);
    g_free(d->actor);
    g_free(d);
}

   
                   
                                                      
  
                                                           
  
                                   
                   
                                    
  
                                                                
   
static void _restore_finish(RestoreTaskData *d, gboolean ok, const gchar *err_msg, const gchar *actor)
{
                                                                   
    gchar *target = g_strdup_printf("%s@%s", d->vm_name, d->snapshot_name);
    if (!ok) {
        const gchar *msg = err_msg ? err_msg : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async restore failed: %s@%s — %s",
                     d->vm_name, d->snapshot_name, msg);
        pcv_audit_log(actor, "backup.restore", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        JsonObject *failed = json_object_new();
        json_object_set_string_member(failed, "error", msg);
        JsonNode *failed_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(failed_node, failed);
        gchar *failed_json = json_to_string(failed_node, FALSE);
        json_node_free(failed_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_FAILED, failed_json, msg);
        g_free(failed_json);
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async restore complete: %s@%s",
                     d->vm_name, d->snapshot_name);
        pcv_audit_log(actor, "backup.restore", target, "ok", 0, 0, "local");
        JsonObject *completed = json_object_new();
        json_object_set_string_member(completed, "vm_name", d->vm_name);
        json_object_set_string_member(completed, "snapshot_name", d->snapshot_name);
        JsonNode *completed_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(completed_node, completed);
        gchar *completed_json = json_to_string(completed_node, FALSE);
        json_node_free(completed_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_COMPLETED, completed_json, NULL);
        g_free(completed_json);
    }
    g_free(target);
}

   
                   
                            
  
                                                  
                                                            
                                                        
                                         
  
                          
                                                                                     
  
                                                    
                                                     
  
                              
                                                          
                                                                
                                                                  
                                             
  
         
                                                                                      
                                                                                  
                                                          
                                                           
                                                     
   
static void _restore_worker(GTask        *task,
                              gpointer      source __attribute__((unused)),
                              gpointer      task_data,
                              GCancellable *cancel __attribute__((unused)))
{
    RestoreTaskData *d = (RestoreTaskData *)task_data;
    GError *err = NULL;

    pcv_job_progress(d->job_ref, 10, "ZFS snapshot restore running");

                                                                         
    d->rollback_ok = pcv_backup_restore(d->vm_name, d->snapshot_name, &d->was_running, &err);
    if (!d->rollback_ok && err)
        d->rollback_error = g_strdup(err->message);
    g_clear_error(&err);
    g_task_return_boolean(task, TRUE);
}

   
                         
                                            
  
                                                     
                            
  
                                              
                             
                                 
                                                                      
  
                                                                   
                                                                                      
   
static void _restore_restart_done(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data)
{
    (void)vm_id;
    RestoreTaskData *d = user_data;
    if (!d->rollback_ok) {
        _restore_finish(d, FALSE, d->rollback_error, d->actor);                                    
    } else if (ok) {
        _restore_finish(d, TRUE, NULL, d->actor);
    } else {
        gchar *msg = g_strdup_printf("Rollback completed but VM '%s' did not restart: %s",
                                     d->vm_name, error ? error->message : "unknown");
        _restore_finish(d, FALSE, msg, d->actor);
        g_free(msg);
    }
    _restore_task_data_free(d);
}

   
                      
                                                                     
  
                                                        
                                                    
  
                                                                    
                                                           
                                                            
                                        
   
static void _restore_task_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source; (void)user_data;
    RestoreTaskData *d = g_task_get_task_data(G_TASK(res));
    g_clear_pointer(&d->lock, pcv_vm_lock_release);
    if (!d->was_running) {
        _restore_finish(d, d->rollback_ok, d->rollback_error, pcv_drain_task_actor(res));
        return;                       
    }
                                                             
    RestoreTaskData *own = g_new0(RestoreTaskData, 1);
    *own = *d;
    memset(d, 0, sizeof(*d));
    own->actor = g_strdup(pcv_drain_task_actor(res));                                              
    PcvVmStartRequest req = { .vm_id = own->vm_name, .numa_node = 0, .vcpu_count = 0,
                              .bridge_name = NULL, .origin = "backup.restore", .actor = own->actor };
    gchar *err = NULL;
    if (pcv_vm_start_submit(&req, _restore_restart_done, own, &err) != PCV_VM_START_SUBMITTED) {
        pcv_vm_start_report_rejected(own->vm_name, "backup.restore", err, own->actor);
        GError *e = g_error_new(G_IO_ERROR, G_IO_ERROR_BUSY, "%s", err ? err : "start rejected");
        _restore_restart_done(own->vm_name, FALSE, e, own);
        g_error_free(e);
    }
    g_free(err);
}

   
                     
                                                                         
  
                                                    
                                                 
  
                                                           
                                                                           
   
static GTask *_restore_task_new(RestoreTaskData *d)
{
    GTask *task = pcv_drain_task_new(NULL, NULL, _restore_task_done, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_restore_task_data_free);
    return task;
}

   
                         
                                                    
                                                          
                                                          
                 
                       
                           
  
                                  
                                                                 
                                                                          
                                                      
                                                          
                                                
  
                                     
                                                          
                                               
   
void handle_backup_restore(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name       = NULL;
    const gchar *snapshot_name = NULL;

    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");
    if (params && json_object_has_member(params, "snapshot_name"))
        snapshot_name = json_object_get_string_member(params, "snapshot_name");

    if (!vm_name || !snapshot_name) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: vm_name, snapshot_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
                                                 
    if (!pcv_validate_vm_name(vm_name) || !pcv_validate_vm_name(snapshot_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name or snapshot_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                                                                    
                                            
    gchar *lock_err = NULL;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_name, VM_OP_SNAPSHOT, &lock_err);
    if (!op_lock) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                                                    lock_err ? lock_err : "VM busy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp); g_free(lock_err);
        return;
    }

      
                                              
      
             
                                               
                                                     
                                                                 
      
                    
                                                
                                               
                                           
                                                     
      
                       
                                         
                                                                      
       
    gchar *target = g_strdup_printf("%s@%s", vm_name, snapshot_name);
    PcvJobRef *job_ref = pcv_job_admit("backup.restore", target, NULL, "backup.restore");
    g_free(target);
    if (!job_ref) {
        pcv_vm_lock_release(op_lock);                                   
        gchar *job_error = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Failed to create backup restore job");
        pure_uds_server_send_response(server, connection, job_error);
        g_free(job_error);
        return;
    }

    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "vm_name", vm_name);
    json_object_set_string_member(accepted, "snapshot_name", snapshot_name);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

      
                    
      
                      
                                                                 
                                                              
                                     
                                            
                                                       
                                     
      
                                              
                                             
       
    RestoreTaskData *d = g_new0(RestoreTaskData, 1);
    d->vm_name       = g_strdup(vm_name);
    d->snapshot_name = g_strdup(snapshot_name);
    d->job_id        = job_id;                          
    d->job_ref = job_ref;             
    d->lock    = op_lock;                             

      
                         
      
                            
                                                                                   
                                                             
                                                         
      
                                               
                                               
                                        
      
                            
                                
                                                    
                                  
       
    GTask *task = _restore_task_new(d);
    g_task_run_in_thread(task, _restore_worker);
    g_object_unref(task);
}

                                                              
                                                     
  
                                                               
                                            
                                                
                                              
                                                                 

   
                       
                                                      
                                       
                                              
   
typedef struct {
    gchar *vm_name;                               
    gchar *job_id;                                
    PcvJobRef *job_ref;                                     
} IncrementalTaskData;

                                   
                                                  
static void _incremental_task_data_free(gpointer p)
{
    IncrementalTaskData *d = (IncrementalTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    pcv_job_unref(d->job_ref);
    g_free(d->job_id);
    g_free(d);
}

   
                       
                                                      
  
                                                      
                                                            
  
                                                                   
                                                    
                                                                                         
                                                    
  
                                                                         
                                                      
   
static void _incremental_worker(GTask        *task,
                                 gpointer      source __attribute__((unused)),
                                 gpointer      task_data,
                                 GCancellable *cancel __attribute__((unused)))
{
    IncrementalTaskData *d = (IncrementalTaskData *)task_data;
    GError *err = NULL;

    pcv_job_progress(d->job_ref, 10, "ZFS incremental backup running");

                                                              
                                                             
    JsonObject *result = pcv_backup_incremental(d->vm_name, &err);
    gboolean ok = (result != NULL);                                         
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async incremental failed: %s — %s", d->vm_name, err_msg);
        pcv_audit_log(pcv_drain_task_actor(task), "backup.incremental", d->vm_name, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        JsonObject *failed = json_object_new();
        json_object_set_string_member(failed, "error", err_msg);
        JsonNode *failed_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(failed_node, failed);
        gchar *failed_json = json_to_string(failed_node, FALSE);
        json_node_free(failed_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_FAILED, failed_json, err_msg);
        g_free(failed_json);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Incremental backup failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async incremental complete: %s", d->vm_name);
        pcv_audit_log(pcv_drain_task_actor(task), "backup.incremental", d->vm_name, "ok", 0, 0, "local");
        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(result_node, result);
        gchar *result_json = json_to_string(result_node, FALSE);
        json_node_free(result_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_COMPLETED, result_json, NULL);
        g_free(result_json);
        json_object_unref(result);
        g_task_return_boolean(task, TRUE);
    }
}

   
                             
                                                     
                                  
                                                  
  
                                                          
                                                                  
  
                                                     
                                                 
   
void handle_backup_incremental(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
    const gchar *name = NULL;
    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");

    if (!name || *name == '\0') {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    PcvJobRef *job_ref = pcv_job_admit("backup.incremental", name, NULL, "backup.incremental");
    if (!job_ref) {
        gchar *job_error = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Failed to create incremental backup job");
        pure_uds_server_send_response(server, connection, job_error);
        g_free(job_error);
        return;
    }

                                                                      
    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "vm_name", name);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                                                    
    IncrementalTaskData *d = g_new0(IncrementalTaskData, 1);
    d->vm_name = g_strdup(name);
    d->job_id = job_id;                          
    d->job_ref = job_ref;             

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_incremental_task_data_free);
    g_task_run_in_thread(task, _incremental_worker);
    g_object_unref(task);
}

                                                              
                                  
                                                                 

   
                        
                                                    
                                                            
                                                                                
  
                                  
                                                                  
                                                  
   
void handle_backup_verify(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection)
{
    const gchar *name     = NULL;
    const gchar *snapshot = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "snapshot"))
        snapshot = json_object_get_string_member(params, "snapshot");

    if (!name || !snapshot) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, snapshot");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    JsonObject *result = pcv_backup_verify(name, snapshot, &err);
    if (!result) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Verify failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                              
                                                 
                                                                 

                                        
                                                               
typedef struct {
    gchar *vm_name;
    gchar *target_node;
    gchar *ssh_user;
    gchar *job_id;
    PcvJobRef *job_ref;                                     
} ReplicateTaskData;

                                              
static void _replicate_task_data_free(gpointer p)
{
    ReplicateTaskData *d = (ReplicateTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d->target_node);
    g_free(d->ssh_user);
    pcv_job_unref(d->job_ref);
    g_free(d->job_id);
    g_free(d);
}

   
                     
                                            
                                                     
                                                    
                                              
   
static void _replicate_worker(GTask        *task,
                               gpointer      source __attribute__((unused)),
                               gpointer      task_data,
                               GCancellable *cancel __attribute__((unused)))
{
    ReplicateTaskData *d = (ReplicateTaskData *)task_data;
    GError *err = NULL;

    pcv_job_progress(d->job_ref, 10, "ZFS replication running");

    gboolean ok = pcv_backup_replicate(d->vm_name, d->target_node,
                                        d->ssh_user, &err);
    gchar *target = g_strdup_printf("%s:%s", d->vm_name, d->target_node);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        gint code = pcv_repl_hostkey_rpc_code(err);                                                   
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async replication failed: %s → %s — %s",
                     d->vm_name, d->target_node, err_msg);
        pcv_audit_log(pcv_drain_task_actor(task), "backup.replicate", target, "fail", code, 0, "local");
        JsonObject *failed = json_object_new();
        json_object_set_string_member(failed, "error", err_msg);
        json_object_set_int_member(failed, "error_code", code);
        JsonNode *failed_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(failed_node, failed);
        gchar *failed_json = json_to_string(failed_node, FALSE);
        json_node_free(failed_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_FAILED, failed_json, err_msg);
        g_free(failed_json);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Backup replication failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async replication complete: %s → %s",
                     d->vm_name, d->target_node);
        pcv_audit_log(pcv_drain_task_actor(task), "backup.replicate", target, "ok", 0, 0, "local");
        JsonObject *completed = json_object_new();
        json_object_set_string_member(completed, "vm_name", d->vm_name);
        json_object_set_string_member(completed, "target_node", d->target_node);
        JsonNode *completed_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(completed_node, completed);
        gchar *completed_json = json_to_string(completed_node, FALSE);
        json_node_free(completed_node);
        pcv_ws_finish_job(d->job_ref, PCV_JOB_COMPLETED, completed_json, NULL);
        g_free(completed_json);
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

   
                           
                                                       
                                   
                                                                            
  
                                                                 
                                                   
                                 
                                                     
   
void handle_backup_replicate(JsonObject       *params,
                              const gchar      *rpc_id,
                              UdsServer        *server,
                              GSocketConnection *connection)
{
    const gchar *name        = NULL;
    const gchar *target_node = NULL;
    const gchar *ssh_user    = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "target_node"))
        target_node = json_object_get_string_member(params, "target_node");
    if (params && json_object_has_member(params, "ssh_user"))
        ssh_user = json_object_get_string_member(params, "ssh_user");

    if (!name || !target_node) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_remote_host(target_node)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (ssh_user && *ssh_user && !pcv_validate_ssh_user(ssh_user)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: ssh_user");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                                                                     
                                                                       
    GError *hk_err = NULL;
    if (!pcv_repl_hostkey_require(target_node, &hk_err)) {
        gint hk_code = pcv_repl_hostkey_rpc_code(hk_err);
        gchar *resp = pure_rpc_build_error_response(rpc_id, hk_code, hk_err->message);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        gchar *hk_target = g_strdup_printf("%s:%s", name, target_node);
        pcv_audit_log(pcv_rpc_completion_actor(), "backup.replicate", hk_target, "fail", hk_code, 0, "local");
        g_free(hk_target);
        g_error_free(hk_err);
        return;
    }

    gchar *target = g_strdup_printf("%s:%s", name, target_node);
    PcvJobRef *job_ref = pcv_job_admit("backup.replicate", target, NULL, "backup.replicate");
    g_free(target);
    if (!job_ref) {
        gchar *job_error = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Failed to create backup replication job");
        pure_uds_server_send_response(server, connection, job_error);
        g_free(job_error);
        return;
    }

                                              
    gchar *job_id = g_strdup(pcv_job_id(job_ref));
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "vm_name", name);
    json_object_set_string_member(accepted, "target_node", target_node);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

                                                               
    ReplicateTaskData *d = g_new0(ReplicateTaskData, 1);
    d->vm_name     = g_strdup(name);
    d->target_node = g_strdup(target_node);
                                                                  
    d->ssh_user    = g_strdup(ssh_user ? ssh_user : "");
    d->job_id      = job_id;                          
    d->job_ref = job_ref;             

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_replicate_task_data_free);
    g_task_run_in_thread(task, _replicate_worker);
    g_object_unref(task);
}

                                                              
                                                           
                                                                 

   
                                
                                                     
                                               
                                                                              
  
                                       
                        
   
                                                               
void handle_snapshot_schedule_set(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    handle_backup_policy_set(params, rpc_id, server, connection);
}

                                                              
                                                            
                                                                 

   
                                 
                                                 
                   
  
                                        
                                                                      
   
                                                                
void handle_snapshot_schedule_list(JsonObject       *params,
                                   const gchar      *rpc_id,
                                   UdsServer        *server,
                                   GSocketConnection *connection)
{
    handle_backup_policy_list(params, rpc_id, server, connection);
}

                                                              
                                                              
                                                                 

   
                                   
                                                 
                                         
  
                                          
                                            
   
                                                                  
void handle_snapshot_schedule_delete(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection)
{
    handle_backup_policy_delete(params, rpc_id, server, connection);
}


                                                                   
                                                                
                                          
                                                              

                                                               
                                                          
                                                    
                                
typedef struct {
    UdsServer         *server;                          
    GSocketConnection *connection;                                   
    gchar             *rpc_id;
    gchar             *snap;                                                
} SnapshotVerifyCtx;

                                                                    
                                                                
static void _snapshot_verify_ctx_free(gpointer data)
{
    if (!data) return;
    SnapshotVerifyCtx *ctx = data;
    g_free(ctx->rpc_id);
    g_free(ctx->snap);
    if (ctx->server)     g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                                      
                                                         
                                                                      
                                                 
                                                            
static void _snapshot_verify_worker(GTask *task, gpointer src,
                                    gpointer data, GCancellable *cancellable)
{
    (void)src; (void)cancellable;
    SnapshotVerifyCtx *ctx = data;
    gboolean exists = pcv_snapshot_verify_probe(ctx->snap);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "snapshot", ctx->snap);
    json_object_set_boolean_member(res, "exists", exists);
                                                                
                                                                       
                                                                
                                                                      
    const gchar *integrity;
    if (!exists) {
        integrity = "missing";
    } else {
        const gchar *prop_argv[] = {
            "zfs", "get", "-H", "-o", "value", "written", ctx->snap, NULL
        };
                                                                  
        gboolean prop_ok = pcv_spawn_sync(prop_argv, NULL, NULL, NULL);
        integrity = prop_ok ? "verified" : "degraded";
    }
    json_object_set_string_member(res, "integrity", integrity);
    g_task_return_pointer(task, res, (GDestroyNotify)json_object_unref);
}

                                                                  
                                                           
                                                      
                                                           
static void _snapshot_verify_done(GObject *src, GAsyncResult *result,
                                  gpointer data)
{
    (void)src;
    SnapshotVerifyCtx *ctx = data;
    JsonObject *res = g_task_propagate_pointer(G_TASK(result), NULL);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res ? res : json_object_new());                    
    gchar *r = pure_rpc_build_success_response(ctx->rpc_id, n);                  
    pure_uds_server_send_response(ctx->server, ctx->connection, r);
    g_free(r);
                                                        
                                                         
                                                                         
                                                               
                                  
    pcv_audit_log(pcv_drain_task_actor(result), "backup.snapshot.verify", ctx->snap,
                  res ? "ok" : "fail", res ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
                                                                             
}

                                                             
                                                         
                                                               
                                                                        
void handle_snapshot_verify(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *snap = params ? json_object_get_string_member_with_default(params, "snapshot", NULL) : NULL;
    if (!snap || !*snap) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "snapshot name required");
        pure_uds_server_send_response(server, connection, r); g_free(r);
                                                          
                                                      
        pcv_audit_log(pcv_rpc_completion_actor(), "backup.snapshot.verify", "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }
                                                        
                                               
    SnapshotVerifyCtx *ctx = g_new0(SnapshotVerifyCtx, 1);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->rpc_id     = g_strdup(rpc_id);
    ctx->snap       = g_strdup(snap);
    GTask *task = pcv_drain_task_new(NULL, NULL, _snapshot_verify_done, ctx);
    g_task_set_task_data(task, ctx, _snapshot_verify_ctx_free);
    g_task_run_in_thread(task, _snapshot_verify_worker);
    g_object_unref(task);
}
