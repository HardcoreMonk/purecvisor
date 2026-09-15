   
                        
                                                          
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                        
                                                          
                                                       
                                                    
                        
  
                                                              
                                                              
                                                           
                                                    
                                                             
                                                
                                                                    
                                     



                                                                        
                                                          
  
            
                                                                                    
                                                                                                     
                                                                                                          
  
                           
                                                                                        
                                                                                    
                                                                       
                                                                       
                                                                         
                                                                  
                                                                                
  
                         
                                                                    
                                                                              
                                                         
                                                                      
                                                                         
                                                                                             
                                                                             
  



  
         
                                 
                                                     
                                     
                                            
                                                                      
  
          
                                           
                                             
                                           
                                                  
                                                    
                                                                  
                                                                  
                                                                 
                                                                  
                                                                    
  
                    
                             
                                              
                                                   
                                             
                                           
                                                    
  
              
                                      
                                                                  
                                   
   

  
                     
  
                                              
                                                                    
                                                                  
                                                                  
                                                                          
                                                
                                                              
                                                          
   
#include "api/drain.h"
#include "handler_accel.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/sriov_manager.h"
#include "modules/accel/gpu_manager.h"
#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/rpc_utils.h"
#include "purecvisor/pcv_validate.h"
#include "api/ws_server.h"
#include "utils/pcv_job_queue.h"

static void
_handle_gpu_assignment(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c,
                       gboolean attach)
{
    if (!json_object_has_member(p, "vm_name") ||
        !json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(
            id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name, pci_addr");
        pure_uds_server_send_response(s, c, r);
        g_free(r);
        return;
    }
    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pci = json_object_get_string_member(p, "pci_addr");
    if (!pcv_validate_vm_name(vm) || !pcv_validate_pci_addr(pci)) {
        gchar *r = pure_rpc_build_error_response(
            id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name or canonical pci_addr");
        pure_uds_server_send_response(s, c, r);
        g_free(r);
        return;
    }

    GError *error = NULL;
    gboolean ok = attach ? pcv_gpu_attach(vm, pci, &error)
                         : pcv_gpu_detach(vm, pci, &error);
    if (!ok) {
        gchar *r = pure_rpc_build_error_response(
            id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "GPU assignment operation failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r);
        g_clear_error(&error);
        return;
    }

    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "status", attach ? "attached" : "detached");
    json_object_set_string_member(result, "vm_name", vm);
    json_object_set_string_member(result, "pci_addr", pci);
    json_object_set_string_member(result, "assignment_mode", "persistent-vfio");
    json_object_set_boolean_member(result, "managed", FALSE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *r = pure_rpc_build_success_response(id, node);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

void handle_device_gpu_attach(JsonObject *p, const gchar *id,
                              UdsServer *s, GSocketConnection *c)
{
    _handle_gpu_assignment(p, id, s, c, TRUE);
}

void handle_device_gpu_detach(JsonObject *p, const gchar *id,
                              UdsServer *s, GSocketConnection *c)
{
    _handle_gpu_assignment(p, id, s, c, FALSE);
}

                                                                 
               
  
                                                                  
                                      
  
                   
                                               
                                                    
                                                             
                                                                    

   
                      
                        
                      
                  
                  
  
                                                                        
                                                       
                                                           
                                                      
   
void handle_dpdk_status(JsonObject *p __attribute__((unused)), const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                             
              
                      
                  
                  
  
                                              
                                    
                                          
                                                            
   
void handle_dpdk_hugepage_info(JsonObject *p __attribute__((unused)), const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_hugepage_info();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                    
                                                                      
                      
                  
                  
  
                             
                                                 
  
                                         
                                    
                          
  
                                                
                                                          
                                                                        
                                                               
   
void handle_dpdk_bind(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

      
                                                       
                               
                                                          
       
    const gchar *drv = json_object_has_member(p, "driver")
        ? json_object_get_string_member(p, "driver") : NULL;

                                                          
                                                       
                                                 
    gchar *guard_reason = NULL;
                                                                 
                                                        
    if (pcv_dpdk_nic_is_protected(pci, &guard_reason)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS,
            guard_reason ? guard_reason : "refusing to bind NIC in use by host");
        pure_uds_server_send_response(s, c, r);
        g_free(r); g_free(guard_reason);
        return;
    }
    g_free(guard_reason);

      
                                                                    
                                                        
                                                         
       
    GError *err = NULL;
    if (!pcv_dpdk_bind(pci, drv, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk bind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "bound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                      
                        
                      
                  
                  
  
                                     
                                                          
  
                                                  
                                                         
   
void handle_dpdk_unbind(JsonObject *p, const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    GError *err = NULL;
    if (!pcv_dpdk_unbind(pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "dpdk unbind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "unbound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                    
              
                      
                  
                  
  
                                      
                                                             
                                            
   
void handle_dpdk_list(JsonObject *p __attribute__((unused)), const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    JsonArray *arr = pcv_dpdk_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

typedef struct {
    gboolean create;
    gchar *name;
    gchar *port;
    guint mtu;
    gchar *job_id;
    gchar *actor;
    gint64 started_us;
} DpdkBridgeTask;

static void
_dpdk_bridge_task_free(DpdkBridgeTask *data)
{
    if (!data)
        return;
    g_free(data->name);
    g_free(data->port);
    g_free(data->job_id);
    g_free(data->actor);
    g_free(data);
}

static gchar *
_dpdk_bridge_result_json(const DpdkBridgeTask *data, gboolean ok,
                         const GError *error)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "status",
                                  ok ? (data->create ? "created" : "deleted")
                                     : "failed");
    json_object_set_string_member(object, "name", data->name);
    if (data->create) {
        json_object_set_int_member(object, "mtu", data->mtu);
        json_object_set_boolean_member(object, "physical_port_attached",
                                       data->port && *data->port);
    }
    if (!ok)
        json_object_set_string_member(object, "error",
                                      error ? error->message : "DPDK bridge operation failed");

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);
    gchar *json = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    json_node_free(node);
    return json;
}





static void
_dpdk_bridge_worker(GTask *task, gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
    DpdkBridgeTask *data = task_data;
    GError *error = NULL;
    const gchar *method = data->create
        ? "dpdk.bridge.create" : "dpdk.bridge.delete";
    pcv_job_update_status(data->job_id, PCV_JOB_RUNNING, 10,
                          data->create ? "DPDK bridge reconcile running"
                                       : "DPDK bridge delete running");
    gboolean ok = data->create
        ? pcv_dpdk_bridge_create(data->name, data->port, data->mtu, &error)
        : pcv_dpdk_bridge_delete(data->name, &error);

    gint64 duration_ms = (g_get_monotonic_time() - data->started_us) / 1000;
    gchar *result_json = _dpdk_bridge_result_json(data, ok, error);
    pcv_job_set_result(data->job_id,
                       ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED,
                       result_json);
    pcv_audit_log(data->actor, method, data->name, ok ? "ok" : "fail",
                  ok ? 0 : PURE_RPC_ERR_INTERNAL_ERROR,
                  duration_ms, "local");
    pcv_ws_broadcast_job_complete_mt(data->job_id, method,
                                     ok ? "completed" : "failed",
                                     ok ? NULL : (error ? error->message : "unknown"));
    g_free(result_json);

    if (ok) {
        g_task_return_boolean(task, TRUE);
    } else if (error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "DPDK bridge operation failed");
    }
}

static void
_dpdk_bridge_schedule(gboolean create, const gchar *name, const gchar *port,
                      guint mtu, JsonObject *params, const gchar *rpc_id,
                      UdsServer *server, GSocketConnection *connection)
{
    DpdkBridgeTask *data = g_new0(DpdkBridgeTask, 1);
    data->create = create;
    data->name = g_strdup(name);
    data->port = port && *port ? g_strdup(port) : NULL;
    data->mtu = mtu;
    if (params && json_object_has_member(params, "_pcv_caller_sub")) {
        JsonNode *actor = json_object_get_member(params, "_pcv_caller_sub");
        if (actor && JSON_NODE_HOLDS_VALUE(actor) &&
            json_node_get_value_type(actor) == G_TYPE_STRING)
            data->actor = g_strdup(json_node_get_string(actor));
    }
    data->started_us = g_get_monotonic_time();

    const gchar *method = create ? "dpdk.bridge.create" : "dpdk.bridge.delete";
    data->job_id = pcv_job_create(method, name, NULL);
    if (!data->job_id) {
        gchar *response = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "DPDK bridge Job creation failed");
        pure_uds_server_send_response(server, connection, response);
        g_free(response);
        _dpdk_bridge_task_free(data);
        return;
    }

    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", data->job_id);
    json_object_set_string_member(accepted, "method", method);
    json_object_set_string_member(accepted, "name", data->name);
    if (create)
        json_object_set_int_member(accepted, "mtu", data->mtu);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, accepted);
    gchar *response = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, response);
    g_free(response);

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, data, (GDestroyNotify)_dpdk_bridge_task_free);
    g_task_run_in_thread(task, _dpdk_bridge_worker);
    g_object_unref(task);
}

   
                             

                      
                  
                  
  
                          
                                                                        

  
                                               
                                      
                                                    
  


                                                  

   
void handle_dpdk_bridge_create(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

                                                   
                                                                
    if (!pcv_validate_bridge_name(name)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

      


       
    const gchar *port = json_object_has_member(p, "dpdk_port")
        ? json_object_get_string_member(p, "dpdk_port") : NULL;

                                                             
                                                  
    if (port && *port && !pcv_validate_pci_addr(port)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: dpdk_port");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    guint mtu = PCV_DPDK_MTU_DEFAULT;
    if (json_object_has_member(p, "mtu")) {
        JsonNode *mtu_node = json_object_get_member(p, "mtu");
        GType mtu_type = mtu_node && JSON_NODE_HOLDS_VALUE(mtu_node)
            ? json_node_get_value_type(mtu_node) : G_TYPE_INVALID;
        if (mtu_type != G_TYPE_INT64 && mtu_type != G_TYPE_INT &&
            mtu_type != G_TYPE_LONG) {
            gchar *r = pure_rpc_build_error_response(
                id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: mtu must be an integer");
            pure_uds_server_send_response(s, c, r); g_free(r); return;
        }
        gint64 requested_mtu = json_node_get_int(mtu_node);
        if (requested_mtu < PCV_DPDK_MTU_MIN || requested_mtu > PCV_DPDK_MTU_MAX) {
            gchar *r = pure_rpc_build_error_response(
                id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: mtu must be between 68 and 9216");
            pure_uds_server_send_response(s, c, r); g_free(r); return;
        }
        mtu = (guint)requested_mtu;
    }

    _dpdk_bridge_schedule(TRUE, name, port, mtu, p, id, s, c);
}

   
                             
                    
                      
                  
                  
  
                                          
                            
  
                                
                                                       
   
void handle_dpdk_bridge_delete(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

    _dpdk_bridge_schedule(FALSE, name, NULL, PCV_DPDK_MTU_DEFAULT,
                          p, id, s, c);
}

                                                                 
             
  
                                                                              
                                                                  
  
                                          
                                        
  
              
                                                         
                                                
                                                                  
                                           
                                        
                                                                    

   
                       
              
                      
                  
                  
  
                                                          
                                                                    
                                                         
   
void handle_sriov_status(JsonObject *p __attribute__((unused)), const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_sriov_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                       
                                                     
                      
                  
                  
  
                                   
                                                              
  
                                                  
                                                     
  
                                                         
                                                          
                                                           
   
void handle_sriov_enable(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

      
                                                  
                                                  
                                                    
      
                                                                  
                                     
                                      
       
    gint num = json_object_has_member(p, "num_vfs")
        ? (gint)json_object_get_int_member(p, "num_vfs") : 1;

      
                           
                                                                             
                                                
                                             
       
    GError *err = NULL;
    if (!pcv_sriov_enable(pf, num, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov enable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "enabled");
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "num_vfs", num);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                        
                  
                      
                  
                  
  
                                        
  
                                      
                                                 
  
                                             
                                                            
   
void handle_sriov_disable(JsonObject *p, const gchar *id,
                           UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

                                  
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_disable(pf, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov disable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "disabled");
    json_object_set_string_member(res, "pf", pf);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                     
                                            
                      
                  
                  
  
                         
                                          
                                                
                                                          
   
void handle_sriov_list(JsonObject *p, const gchar *id,
                        UdsServer *s, GSocketConnection *c)
{
    const gchar *pf = json_object_has_member(p, "pf")
        ? json_object_get_string_member(p, "pf") : NULL;

    JsonArray *arr = pcv_sriov_list(pf);
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                    
                                                                         
                      
                  
                  
  
                 
                                                                      
                                                                   
                                                   
  
                                    
  
                                   
                                                    
                                                                              
   
void handle_sriov_set(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf") || !json_object_has_member(p, "vf_index")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: pf, vf_index");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *pf = json_object_get_string_member(p, "pf");
    gint vf_idx = (gint)json_object_get_int_member(p, "vf_index");

                                  
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

      
                                        
      
                                            
                                                    
      
                                                    
                                                   
      
                                          
                                             
                                                         
      
                                                     
                                                             
                                                           
                                                          
       
    const gchar *mac = json_object_has_member(p, "mac")
        ? json_object_get_string_member(p, "mac") : NULL;
    gint vlan = json_object_has_member(p, "vlan")
        ? (gint)json_object_get_int_member(p, "vlan") : -1;
    gint spoof = -1;
    if (json_object_has_member(p, "spoofchk")) {
        JsonNode *node = json_object_get_member(p, "spoofchk");
        GType value_type = json_node_get_value_type(node);
        if (value_type == G_TYPE_BOOLEAN) {
            spoof = json_node_get_boolean(node) ? 1 : 0;
        } else if (value_type == G_TYPE_INT64) {
            gint64 value = json_node_get_int(node);
            spoof = (value == 0 || value == 1) ? (gint)value : -2;
        } else if (value_type == G_TYPE_STRING) {

            const gchar *value = json_node_get_string(node);
            if (g_strcmp0(value, "on") == 0)
                spoof = 1;
            else if (g_strcmp0(value, "off") == 0)
                spoof = 0;
            else
                spoof = -2;
        } else {
            spoof = -2;
        }
    }

    if (mac && !pcv_validate_mac(mac)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: mac");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (spoof == -2) {
        gchar *r = pure_rpc_build_error_response(
            id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid: spoofchk (expected on/off, boolean, or 0/1)");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_set(pf, vf_idx, mac, vlan, spoof, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov set failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "configured");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                       
                                                                           
                      
                  
                  
  
                                  
  
           
                                                   
                                                                 
                                                        
                                
  
                                           
                                                  
  
                                                                            
                                                          
                                                                      
   
void handle_sriov_attach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name, pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pf = json_object_get_string_member(p, "pf");

                                  
    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

      
                                                          
                                                       
       
    gint vf_idx = json_object_has_member(p, "vf_index")
        ? (gint)json_object_get_int_member(p, "vf_index") : 0;

      
                                     
      
                                                        
                                                                      
                                                                              
                                                                           
                                                                
      
                                                        
                                                       
                                                        
                                                           
       
    GError *err = NULL;
    if (!pcv_sriov_attach_vm(vm, pf, vf_idx, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov attach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "attached");
    json_object_set_string_member(res, "vm_name", vm);
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "vf_index", vf_idx);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

   
                       
                                       
                      
                  
                  
  
                           
                                                 
  


  
                                 
                                                
                                                             
   
void handle_sriov_detach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_name, pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

                                  
    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_pci_addr(pci)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

      
                                                                                 
      
                                        
                                           
                                                               
                                                                   
                                           
       
    GError *err = NULL;
    if (!pcv_sriov_detach_vm(vm, pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "sriov detach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "detached");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}
