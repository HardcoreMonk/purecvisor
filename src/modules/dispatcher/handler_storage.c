   
                          
                                            
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                      
                                                            
                                                    
                 
  
                                                                    
                                    
                                                                  
                                                              
                                                         
                   
                                                                       
  
            
                                                            
                                                                                                                             
                                                                                                                     
  
                      
                                                                                         
                                                                                  
                                                                                  
                                                                      
  
          
                                                                                          
                                                                      
                                                          
  
         
                                                      
                                      
                                                 
                                                             
  
          
                                    
                          
   
#include "handler_storage.h"
#include "poll_async.h"
#include "rpc_utils.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_zfs_exec.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "api/drain.h"
#include "../audit/pcv_audit.h"
#include "modules/storage/zfs_driver.h"
#include "modules/resource/resource_gate.h"                             
#include <glib.h>
#include <string.h>

   
                               
                                                              
                                                                   
                                                            
                         
  
                                               
                                                            
  
          
                                                       
                                                  
                  
  
                                                                               
                                                              
  
                                                                               
  
                                                                    
   
static JsonArray* execute_zfs_command_to_json(PcvPollGuard *guard, const gchar *command, const gchar **keys,
                                              int num_keys) {
    gchar *std_out = NULL;
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    JsonArray *json_arr = json_array_new();

      
                                                    
                                                                     
                         
       
    gchar **parsed_argv = NULL;
    if (!g_shell_parse_argv(command, NULL, &parsed_argv, &error)) {
        g_printerr("[ZFS] Failed to parse command: %s\n", error->message);
        g_error_free(error);
        return json_arr;
    }
    if (!pcv_poll_guard_spawn(guard, (const gchar * const *)parsed_argv, &std_out, &std_err, &error)) {
        g_printerr("[ZFS] Command failed: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_strfreev(parsed_argv);
        return json_arr;
    }
    g_strfreev(parsed_argv);
                                                                              
    exit_status = 0;                                                          

                                                                
                                                     
    if (exit_status == 0 && std_out != NULL) {
        gchar **lines = g_strsplit(std_out, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            if (strlen(lines[i]) == 0) continue;                       

                                           
            gchar **tokens = g_strsplit(lines[i], "\t", -1);
            JsonObject *row_obj = json_object_new();

            for (int j = 0; j < num_keys && tokens[j] != NULL; j++) {
                gchar *clean_val = g_strstrip(g_strdup(tokens[j]));                
                json_object_set_string_member(row_obj, keys[j], clean_val);
                g_free(clean_val);
            }
            json_array_add_object_element(json_arr, row_obj);
            g_strfreev(tokens);
        }
        g_strfreev(lines);
    }

    g_free(std_out);
    g_free(std_err);
    return json_arr;
}

                                                                                               
                                                                  
                                                      
static JsonNode *
pcv_storage_pool_list_compute(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    (void)params; (void)error;
                                                          
    const gchar *cmd = "zpool list -H -o name,size,alloc,free,health";
    const gchar *keys[] = {"name", "size", "alloc", "free", "health"};
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, execute_zfs_command_to_json(guard, cmd, keys, 5));
    return result_node;
}

                                                                    
                           
                                                                    
                                                                                               
                                                                
                                                            
void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    pcv_poll_async_submit("storage.pool.list", params, rpc_id, server, connection, pcv_storage_pool_list_compute);
}

                                                                                                       
                                                          
static JsonNode *
pcv_storage_zvol_list_compute(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    (void)params; (void)error;
                                                                                             
    const gchar *cmd = "zfs list -t volume -H -o name,volsize,used,refer,compressratio,dedup,written";
    const gchar *keys[] = {"name", "volsize", "used", "refer", "compression_ratio", "dedup", "written"};
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, execute_zfs_command_to_json(guard, cmd, keys, 7));
    return result_node;
}

                                                                    
                       
                                                                    
                                                                                                    
                                                              
void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    pcv_poll_async_submit("storage.zvol.list", params, rpc_id, server, connection, pcv_storage_zvol_list_compute);
}

                                                                                    
                                                               
typedef struct {
    const gchar *method;                                                                 
    gchar *zvol_path;                          
    gchar *size;                                        
    gchar *audit_target;                        
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    gboolean ok;                        
    gint err_code;                                                        
    gchar *err_msg;
} ZvolOpCtx;

                                                                            
static void
_zvol_op_ctx_free(gpointer p)
{
    ZvolOpCtx *c = p;
    g_free(c->zvol_path); g_free(c->size); g_free(c->audit_target);
    g_free(c->rpc_id); g_free(c->err_msg);
    if (c->server) g_object_unref(c->server);
    if (c->connection) g_object_unref(c->connection);
    g_free(c);
}

                                                                        
                                                    
static gboolean
_zvol_delete_is_volume(const gchar *zvol_path)
{
                                                                            
    const gchar *type_argv[] = {"zfs", "list", "-H", "-o", "type", zvol_path, NULL};
    gchar *type_out = NULL;
    gboolean ok = pcv_spawn_sync(type_argv, &type_out, NULL, NULL);
                                                           
    gboolean is_vol = ok && type_out && g_strcmp0(g_strstrip(type_out), "volume") == 0;
    g_free(type_out);
    return is_vol;
}

                                                              
                                                                    
                                                                  
static void
_zvol_op_worker(GTask *task, gpointer src G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    ZvolOpCtx *c = task_data;
    GError *error = NULL;
    gchar *std_err = NULL;

    if (c->size) {
        const gchar *options[] = {"-V", c->size, NULL};
        c->ok = pcv_zfs_create_locked_short(options, c->zvol_path, &std_err, &error);
    } else if (_zvol_delete_is_volume(c->zvol_path)) {
        c->ok = pcv_zfs_destroy_locked_short("-r", c->zvol_path, &std_err, &error);
    } else {
        c->err_msg = g_strdup("Refused: target is not a zvol (volume) — parent datasets cannot be recursively destroyed");
    }
    if (!c->ok && !c->err_msg)
        c->err_msg = g_strdup(error ? error->message
                                    : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error"));
    c->err_code = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ? PURE_RPC_ERR_BUSY : PURE_RPC_ERR_ZFS_OPERATION;
    if (c->ok)
        PCV_LOG_INFO("storage", "%s done: %s", c->method, c->zvol_path);
                                                                                               
    pcv_audit_log(pcv_drain_task_actor(task), c->method, c->audit_target,
                  c->ok ? "ok" : "fail", c->ok ? 0 : c->err_code, 0, "local");
    g_free(std_err);
    g_clear_error(&error);
    g_task_return_boolean(task, TRUE);                            
}

                                          
                                        
static void
_zvol_op_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res G_GNUC_UNUSED, gpointer user_data)
{
    ZvolOpCtx *c = user_data;
    gchar *resp;
    if (c->ok) {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        resp = pure_rpc_build_success_response(c->rpc_id, res_node);
    } else {
        resp = pure_rpc_build_error_response(c->rpc_id, c->err_code, c->err_msg);
    }
    pure_uds_server_send_response(c->server, c->connection, resp);
    g_free(resp);
                                               
}

                                                                        
                                            
static void
_zvol_op_submit(const gchar *method, const gchar *requested, const gchar *zvol_path, const gchar *size,
                const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    ZvolOpCtx *c = g_new0(ZvolOpCtx, 1);
    c->method = method;
    c->zvol_path = g_strdup(zvol_path);
    c->size = g_strdup(size);
    c->audit_target = g_strdup(requested);
    c->rpc_id = g_strdup(rpc_id);
    c->server = g_object_ref(server);
    c->connection = g_object_ref(connection);
    GTask *task = pcv_drain_task_new(NULL, NULL, _zvol_op_done, c);
    g_task_set_task_data(task, c, _zvol_op_ctx_free);
    g_task_run_in_thread(task, _zvol_op_worker);
    g_object_unref(task);
}

                                                                    
                               
  
                                                         
                                        
  
                               
                                          
                                           
                                     
  
                                                        
                                            
  
                                           
                                                    
                                                           
                                                                       
void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                                                                 
    const gchar *zvol_path = json_object_get_string_member_with_default(params, "zvol_path", NULL);
    if (!zvol_path)
        zvol_path = json_object_get_string_member_with_default(params, "name", NULL);
                                                                  
    const gchar *size = json_object_get_string_member_with_default(params, "size", NULL);
    gchar *size_buf = NULL;
    if (!size && json_object_has_member(params, "size_gb")) {
        gint64 gb = json_object_get_int_member(params, "size_gb");
        size_buf = g_strdup_printf("%ldG", (long)gb);
        size = size_buf;
    }

    if (!zvol_path || !zvol_path[0] || !size) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path'/'name' or 'size'/'size_gb'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); g_free(size_buf); return;
    }

                                                          
                                                        
    for (const gchar *p = zvol_path; *p; p++) {
                                                                       
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != '/') {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid zvol name — allowed: [a-zA-Z0-9_.-/]");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(size_buf); return;
        }
    }

                                                   
                                                  
    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/'))
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);

    _zvol_op_submit("storage.zvol.create", zvol_path, full_path ? full_path : zvol_path, size,
                    rpc_id, server, connection);

    g_free(full_path);
    g_free(size_buf);
}

                                                                    
                    
  
                                                     
                                              
  
                                                    
  
                      
                                                    
                                                       
  
                         
                               
                                                                       
void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
                                                                 
    const gchar *zvol_path = json_object_get_string_member_with_default(params, "zvol_path", NULL);
    if (!zvol_path)
        zvol_path = json_object_get_string_member_with_default(params, "name", NULL);

    if (!zvol_path || !zvol_path[0]) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path' or 'name'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); return;
    }

                                                      
    const gchar *requested = zvol_path;
    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/')) {
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);
        zvol_path = full_path;
    }

                                                                               
                                                   
                                                                       
                                                                                
                                                             
    gboolean bad = (zvol_path[0] == '/') || strstr(zvol_path, "..") || strstr(zvol_path, "//");
    for (const gchar *c = zvol_path; !bad && *c; c++) {
        if (!g_ascii_isalnum(*c) && *c != '_' && *c != '-' && *c != '.' && *c != '/')
            bad = TRUE;
    }
    if (bad) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid zvol_path");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        g_free(full_path);
        return;
    }

    _zvol_op_submit("storage.zvol.delete", requested, zvol_path, NULL, rpc_id, server, connection);

    g_free(full_path);
}

                                                                                         
                                                                  
static gboolean
_pool_vdev_type_allowed(const gchar *vdev_type)
{
    static const gchar *const allowed[] = { "", "mirror", "raidz", "raidz1", "raidz2", "raidz3", NULL };
    return vdev_type && g_strv_contains(allowed, vdev_type);
}

                                                                    
                                               
  
                                                        
  
                                                                                     
                      
                                                                                                             
                                   
  
                                                       
                                                                       
void handle_storage_pool_create_request(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *vdev_type = pcv_rpc_params_get_string(params, "vdev_type");                              

    if (!name || !json_object_has_member(params, "disks")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name' or 'disks'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    if (json_object_has_member(params, "vdev_type") && !_pool_vdev_type_allowed(vdev_type)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "'vdev_type' must be one of \"\" (stripe), mirror, raidz, raidz1, raidz2, raidz3");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                                                                                
    JsonNode *disks_node = json_object_get_member(params, "disks");
    if (!JSON_NODE_HOLDS_ARRAY(disks_node)) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "'disks' must be an array of device paths");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    JsonArray *disks_arr = json_node_get_array(disks_node);
    guint n_disks = json_array_get_length(disks_arr);
    if (n_disks == 0) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "'disks' array is empty");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                                                 
                                          
    const gchar **disk_list = g_new0(const gchar *, n_disks);
    for (guint i = 0; i < n_disks; i++) {
                                                                           
                                                             
        JsonNode *disk = json_array_get_element(disks_arr, i);
        disk_list[i] = (JSON_NODE_HOLDS_VALUE(disk) && json_node_get_value_type(disk) == G_TYPE_STRING)
            ? json_node_get_string(disk) : NULL;
        if (!disk_list[i] || !disk_list[i][0]) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "'disks' must be an array of device paths");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp);
            g_free(disk_list);
            return;
        }
    }

    GError *error = NULL;
                                                         
    const gchar *compression = NULL;
    if (json_object_has_member(params, "compression"))
        compression = json_object_get_string_member(params, "compression");
    gboolean ok = purecvisor_zfs_create_pool(name, vdev_type, disk_list, (gint)n_disks, compression, &error);
    g_free(disk_list);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "created", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                    
                                                
  
                                                     
  
                                      
  
                                     
          
                                                                       
void handle_storage_pool_destroy_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_destroy_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool destroy failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "destroyed", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                    
                                               
  
                                                   
                                        
  
                                       
  
                                           
                                               
                                                                       
void handle_storage_pool_scrub_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_scrub_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool scrub failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "scrub_started", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                      
#include "../../utils/pcv_config.h"
#include <sys/stat.h>

   
                                                                       
                                                                  
                                                                                                              
                                                                 
   
void handle_iso_list(JsonObject *params, const gchar *rpc_id,
                      UdsServer *server, GSocketConnection *connection)
{
    PcvResCaller caller = { 0 };
    pcv_res_caller_init(&caller, params, connection);                                         
    const gchar *iso_dirs_str = pcv_config_get_iso_dirs();
    gchar **dirs = g_strsplit(iso_dirs_str, ",", -1);                                  
    JsonArray *arr = json_array_new();

    for (gint i = 0; dirs[i] != NULL; i++) {
        gchar *iso_dir = g_strstrip(dirs[i]);                            
        if (iso_dir[0] == '\0') continue;                              
        if (strstr(iso_dir, "..")) continue;                                  

        GDir *dir = g_dir_open(iso_dir, 0, NULL);
        if (!dir) continue;

        const gchar *name;
                                                                     
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_suffix(name, ".iso") || g_str_has_suffix(name, ".ISO") ||
                g_str_has_suffix(name, ".img") || g_str_has_suffix(name, ".IMG")) {
                gchar *full = g_strdup_printf("%s/%s", iso_dir, name);
                if (!pcv_res_visible(&caller, PCV_RES_ISO, full)) { g_free(full); continue; }
                struct stat st;
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_object_set_string_member(obj, "path", full);
                json_object_set_string_member(obj, "dir", iso_dir);
                if (stat(full, &st) == 0)
                    json_object_set_int_member(obj, "size_mb", (gint64)(st.st_size / (1024*1024)));
                json_array_add_object_element(arr, obj);
                g_free(full);
            }
        }
        g_dir_close(dir);
    }
    g_strfreev(dirs);
    pcv_res_caller_clear(&caller);

    JsonNode *iso_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(iso_node, arr);
    gchar *iso_resp = pure_rpc_build_success_response(rpc_id, iso_node);
    pure_uds_server_send_response(server, connection, iso_resp);
    g_free(iso_resp);
}



                                                                  
                                                         
                                 
                       
                                                              
                                                  
                                    
                                                                        
void handle_storage_pool_health(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : "pcvpool";
    ZfsPoolHealth zh;
    if (pcv_zfs_pool_health(pool, &zh)) {
        JsonObject *result = pcv_zfs_pool_health_to_json(&zh);
        json_object_set_string_member(result, "pool", pool);
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, result);
        gchar *resp = pure_rpc_build_success_response(rpc_id, n);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to query pool health");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

                                                                        
                                                  
                                         
                      
                                 
                                                                        
void handle_storage_pool_forecast(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : "pcvpool";
    JsonObject *result = pcv_zfs_pool_forecast(pool);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}
