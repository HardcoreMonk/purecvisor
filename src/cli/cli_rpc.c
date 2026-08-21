   
                  
                                                           
  
                           
                                                   
                                                    
                                        
  
                                                      
                                                        
                                                        
  
                                                     
                                                        
                                                     
                              
                                                    
                                             
  
                                                                               
           
                                                                               
                                                               
                                              
  
                                                                     
                                                            
                                                            
                                                        
                                                                             
                                                            
                                                            
                                                              
  
                                                                               
              
                                                                               
  
                                  
                                         
                                                          
                                         
                                  
                                                  
                                                 
                                                      
  
                                 
                                              
                                       
                                                                    
  
                                            
                                         
                                                 
                              
                                              
                                                                               
   
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
    g_socket_set_timeout(sock, 10);                                                     

                                   
                                                       
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
                                                                        
                                                          
                                                   
                                                                 
        gchar *masked = g_strdup(payload);                                  
        static const char *sensitive_keys[] = {
            "password", "secret", "token", "api_key", "apikey",
            "jwt_secret", "refresh_token", "access_token",
            "totp_secret", "recovery_codes", NULL                        
        };
        for (int i = 0; sensitive_keys[i]; i++) {
                                                                      
            gchar *needle = g_strdup_printf("\"%s\":\"", sensitive_keys[i]);
            gchar *found = strstr(masked, needle);
            if (found) {
                gchar *val_start = found + strlen(needle);                  
                gchar *val_end = strchr(val_start, '"');                      
                if (val_end) {
                    gsize val_len = (gsize)(val_end - val_start);                
                                                          
                    memset(val_start, '*', val_len > 4 ? 4 : val_len);
                                                                
                                                                
                    if (val_len > 4) memmove(val_start + 4, val_end, strlen(val_end) + 1);
                }
            }
            g_free(needle);                              
        }
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

    if (g_ctx.verbose)
        g_printerr("%s[← %s]%s\n", ce(CYBER_DIM), result, ce(CYBER_RESET));                    

                                            
                                                             
                                                 
                                                                 
                                     
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
