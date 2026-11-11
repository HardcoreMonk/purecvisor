   
                  
                                                           
  
                           
                                                   
                                                    
                                        
  
                                                      
                                                        
                                                        
  
                                                     
                                                        
                                                     
                              
                                                    
                                             
  
                                                                               
           
                                                                               
                                                               
                                              
  
                                                                     
                                                            
                                                            
                                                        
                                                                             
                                                            
                                                            
                                                              
  
                                                                               
              
                                                                               
  
                                  
                                         
                                                          
                                         
                                  
                                                  
                                                 
                                                      
  
                                 
                                              
                                       
                                                                    
  
                                            
                                         
                                                 
                              
                                              
                                                                               
   
#include "cli_rpc.h"
#include "modules/dispatcher/rpc_utils.h"                                 

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

                                                               
                             
                                     
                                                                          
                                                           
PcvCtx g_ctx = {
    .fmt         = FMT_TABLE,                            
    .interactive = false,                                    
    .batch       = false,
    .no_color    = false,                                     
    .verbose     = false,
    .socket_path = DAEMON_SOCK_PATH,                         
    .command_status = PCV_CLI_EXIT_OK,
};

                                                                 
                                         
                                                                 
                           
                                                                          
                                                    
                                                       
const char *cc(const char *code) {
                                                     
                                                      
    if (g_ctx.no_color || !isatty(STDOUT_FILENO)) return "";
    return code;                                   
}
                                                            
const char *ce(const char *code) {
                                                          
    if (g_ctx.no_color || !isatty(STDERR_FILENO)) return "";
    return code;
}

                                                                
                                                          
                                                                
                                                               
void pcv_cli_command_begin(void) {
    g_ctx.command_status = PCV_CLI_EXIT_USAGE;
}

                                                                         
void pcv_cli_command_mark_success(void) {
    if (g_ctx.command_status != PCV_CLI_EXIT_RUNTIME)
        g_ctx.command_status = PCV_CLI_EXIT_OK;
}

                                                               
void pcv_cli_command_mark_runtime_failure(void) {
    g_ctx.command_status = PCV_CLI_EXIT_RUNTIME;
}

                                                                
PcvCliExitStatus pcv_cli_command_status(void) {
    return g_ctx.command_status;
}

   
                                                                 
  
                                                          
                                                      
                                                   
   
static gboolean _classify_rpc_response(const gchar *response) {
    GError *parse_error = NULL;
    JsonParser *parser = json_parser_new();
    gboolean method_not_found = FALSE;

    if (!json_parser_load_from_data(parser, response, -1, &parse_error)) {
        pcv_cli_command_mark_runtime_failure();
        g_clear_error(&parse_error);
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        pcv_cli_command_mark_runtime_failure();
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *root = json_node_get_object(root_node);
    if (json_object_has_member(root, "error")) {
        JsonNode *error_node = json_object_get_member(root, "error");
        pcv_cli_command_mark_runtime_failure();
                                                                   
                                                            
        if (error_node && JSON_NODE_HOLDS_OBJECT(error_node)) {
            JsonObject *error_obj = json_node_get_object(error_node);
            if (json_object_get_int_member_with_default(error_obj, "code", 0) ==
                    PURE_RPC_ERR_METHOD_NOT_FOUND)
                method_not_found = TRUE;
        }
    } else if (!json_object_has_member(root, "result")) {
                                                                      
        pcv_cli_command_mark_runtime_failure();
    }

    g_object_unref(parser);
    return method_not_found;
}

                                                                      
                                          
static const char *const VERBOSE_SECRET_WORDS[] = {
    "password", "passphrase", "secret", "token", "api_key", "apikey", "recovery_codes", "webhook_url", "webhook_crit_url",
};

                                                                                                 
static gboolean
_verbose_key_is_secret(const gchar *key, gboolean is_request)
{
    if (is_request && g_strcmp0(key, "value") == 0) return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(VERBOSE_SECRET_WORDS); i++)
        if (strstr(key, VERBOSE_SECRET_WORDS[i])) return TRUE;
    return FALSE;
}

                                                                                             
static void
_verbose_mask_node(JsonNode *node, gboolean is_request)
{
    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *arr = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(arr); i++)
            _verbose_mask_node(json_array_get_element(arr, i), is_request);
    } else if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *obj = json_node_get_object(node);
        GList *members = json_object_get_members(obj);
        for (GList *m = members; m; m = m->next) {
            JsonNode *child = json_object_get_member(obj, m->data);
            if (!_verbose_key_is_secret(m->data, is_request)) { _verbose_mask_node(child, is_request); continue; }
            gboolean text = JSON_NODE_HOLDS_VALUE(child) && json_node_get_value_type(child) == G_TYPE_STRING;
            if (JSON_NODE_HOLDS_ARRAY(child) || JSON_NODE_HOLDS_OBJECT(child) || (text && *json_node_get_string(child)))
                json_object_set_string_member(obj, m->data, "****");
        }
        g_list_free(members);
    }
}

                                                                                       
static gchar *
_verbose_masked_payload(const gchar *payload, gboolean is_request)
{
                                                                                       
    JsonNode *copy = json_from_string(payload, NULL);
    if (!copy) return g_strdup(is_request ? "(unprintable request)" : "(unprintable response)");
    _verbose_mask_node(copy, is_request);
    gchar *masked = json_to_string(copy, FALSE);
    json_node_unref(copy);
    return masked;
}

   
                                                               
  
                                     
       
                                                      
                                                         
                                                                
                                
                                                     
                                                 
                                       
  
       
                                             
                                         
                                                           
                                               
                                       
  
                                             
  
                                              
                                          
                              
                                        
   
gchar *purectl_send_request(const gchar *method,
                            JsonObject  *params_obj,
                            GError     **error) {
                                                              
                                                                                 
    pcv_cli_command_mark_success();

                        
                                                        
    GSocketClient    *client = g_socket_client_new();
    GSocketAddress   *addr   = g_unix_socket_address_new(g_ctx.socket_path);
    GSocketConnection *conn  = g_socket_client_connect(
            client, G_SOCKET_CONNECTABLE(addr), NULL, error);

                                                             
    g_object_unref(client);
    g_object_unref(addr);
    if (!conn) {
          
                                                     
                                                    
                                              
           
        if (params_obj) json_object_unref(params_obj);
        pcv_cli_command_mark_runtime_failure();
        return NULL;
    }

    GSocket *sock = g_socket_connection_get_socket(conn);
                                                                                            
                                                                               
                                                                                                                                                                                                                      
                                                                                                              
                                
    guint sock_timeout = g_strcmp0(method, "vm.inspect") == 0 ? 35
                       : (g_strcmp0(method, "backup.replication.hostkey.scan") == 0 ||
                          g_strcmp0(method, "backup.replication.hostkey.trust") == 0) ? 40
                       : g_strcmp0(method, "webhook.dlq.retry") == 0 ? 70
                       : g_strcmp0(method, "container.exec") == 0 ? 65
                       : g_strcmp0(method, "vm.guest.exec") == 0 ? 20
                       : g_strcmp0(method, "container.snapshot.rollback") == 0 ? 75 : 10;
    g_socket_set_timeout(sock, sock_timeout);

                                   
                                                       
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");                  
    json_object_set_string_member(root_obj, "method",  method);
                                                 
                                                             
                                                  
    json_object_set_object_member(root_obj, "params",
            params_obj ? params_obj : json_object_new());
    json_object_set_int_member(root_obj, "id", 1);                                

    JsonNode      *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);                                   
    gchar *payload = json_to_string(root_node, FALSE);                             
    json_node_free(root_node);                                                  

    if (g_ctx.verbose) {
                                                                               
                                                                                      
        gchar *masked = _verbose_masked_payload(payload, TRUE);
        g_printerr("%s[→ %s]%s\n", ce(CYBER_DIM), masked, ce(CYBER_RESET));                   
        g_free(masked);
    }

                      
                                                             
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gsize bytes_written;
                                                    
                                                        
    if (!g_output_stream_write_all(out, payload, strlen(payload),
                                   &bytes_written, NULL, error)) {
        g_free(payload);
        g_object_unref(conn);
        pcv_cli_command_mark_runtime_failure();
        return NULL;
    }
    g_free(payload);                                   

                                                                      
                                                          
    if (!g_socket_shutdown(sock, FALSE, TRUE, error)) {
        g_object_unref(conn);
        pcv_cli_command_mark_runtime_failure();
        return NULL;
    }

                       
                                                       
    GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GByteArray   *buf = g_byte_array_new();                                 
    gchar         tmp[8192];                                        
    gssize        n;
                                                          
    while ((n = g_input_stream_read(in, tmp, sizeof(tmp), NULL, error)) > 0)
        g_byte_array_append(buf, (guint8 *)tmp, (guint)n);                    

    g_object_unref(conn);                                     

    if (n < 0) {
                                                           
        g_byte_array_free(buf, TRUE);
        pcv_cli_command_mark_runtime_failure();
        return NULL;
    }

    if (buf->len == 0) {                                          
        g_byte_array_free(buf, TRUE);
        pcv_cli_command_mark_runtime_failure();
        return NULL;
    }
    g_byte_array_append(buf, (guint8 *)"\0", 1);                                
    gchar *result = g_strdup((gchar *)buf->data);                              
    g_byte_array_free(buf, TRUE);                                                 

    if (g_ctx.verbose) {
        gchar *masked = _verbose_masked_payload(result, FALSE);                                         
        g_printerr("%s[← %s]%s\n", ce(CYBER_DIM), masked, ce(CYBER_RESET));                    
        g_free(masked);
    }

                                            
                                                             
                                                 
                                                                 
                                     
    if (_classify_rpc_response(result)) {                                               
        g_printerr("\n%s[!] This command is not included in Single Edge.%s\n"
                   "%s    Current daemon does not support '%s' RPC method.%s\n"
                   "%s    Use the appropriate edition repository for this feature.%s\n\n",
            ce(CYBER_YELLOW), ce(CYBER_RESET),
            ce(CYBER_DIM), method, ce(CYBER_RESET),
            ce(CYBER_DIM), ce(CYBER_RESET));
        g_free(result);                                          
        return NULL;
    }

    return result;                                          
}
