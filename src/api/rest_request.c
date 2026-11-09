   
                       
                                        
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                                
   

#include "api/drain.h"
#include "api/rest_shutdown.h"
#include "api/rest_server.h"
#include "api/rest_middleware.h"
#include "api/rest_auth.h"
#include "api/rest_client_identity.h"
#include "utils/pcv_crypto.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_totp.h"
#include "utils/pcv_log.h"
#include "modules/daemons/prometheus_exporter.h"
#include "api/ws_server.h"
#include "utils/pcv_config.h"
#include "utils/pcv_tls.h"
#include "utils/pcv_bpf.h"
#include "utils/pcv_secure.h"
#include "modules/dispatcher/rpc_utils.h"
#if PCV_CLUSTER_ENABLED
#include "../modules/cluster/cluster_manager.h"
#endif
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "modules/network/ovn_manager.h"
#include "modules/network/dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"
#include "api/rest_internal.h"
#include "api/rest_response.h"
#include "api/rest_request.h"

gchar *
pcv_rest_build_rpc(const gchar *method, JsonObject *params)
{
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);

    json_builder_set_member_name(jb, "jsonrpc");
    json_builder_add_string_value(jb, "2.0");
    json_builder_set_member_name(jb, "method");
    json_builder_add_string_value(jb, method);
    json_builder_set_member_name(jb, "params");

    if (params) {
                                               
                                            
                                                       
                                                                
        JsonNode *params_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(params_node, params);
        json_builder_add_value(jb, params_node);
    } else {
                                                          
        json_builder_begin_object(jb);
        json_builder_end_object(jb);
    }

    json_builder_set_member_name(jb, "id");
    json_builder_add_int_value(jb, 1);
    json_builder_end_object(jb);

                                     
                                            
                                       
                                                 
                                                        
      
                                            
    JsonNode *root = json_builder_get_root(jb);
    gchar *rpc = json_to_string(root, FALSE);

    json_node_free(root);
    g_object_unref(jb);
    return rpc;                          
}

   
                          
  
                                                  
                       
                                                                          
  
                                              
                                                     
                                                                                
  
                         
                                               
                                           
   
gchar *
pcv_rest_build_rpc_name(const gchar *method, const gchar *name)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    gchar *rpc = pcv_rest_build_rpc(method, p);
    json_object_unref(p);
    return rpc;
}

                                      
  
                                                               
                                                                     
                                                                
                                   
JsonObject *
pcv_rest_vpc_params(JsonObject *body,
                 GHashTable *query,
                 const gchar *id_key,
                 const gchar *id_value)
{
    JsonObject *params = body ? json_object_ref(body) : json_object_new();
    if (query) {
        const gchar *tenant = g_hash_table_lookup(query, "tenant");
        if (tenant && *tenant)
            json_object_set_string_member(params, "tenant", tenant);
    }
    if (id_key && id_value)
        json_object_set_string_member(params, id_key, id_value);
    return params;
}

static gboolean
_strict_nonnegative_decimal(const gchar *value, gint64 *parsed)
{
    if (!value || !*value || strlen(value) > 20) return FALSE;
    for (const gchar *p = value; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    errno = 0;
    gchar *end = NULL;
    gint64 result = g_ascii_strtoll(value, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return FALSE;
    *parsed = result;
    return TRUE;
}

                                                                                        
                                               
static gboolean
_audit_query_keys_valid(const gchar *raw_query)
{
    static const gchar *const keys[] = { "from_ts", "to_ts", "username", "method", "limit", "offset" };
    gboolean seen[G_N_ELEMENTS(keys)] = { FALSE };
    gboolean valid = TRUE;
    gchar **pairs = g_strsplit(raw_query, "&", -1);
    for (gchar **pair = pairs; valid && *pair; pair++) {
        const gchar *equals = strchr(*pair, '=');
        guint i = 0;
        for (; equals && i < G_N_ELEMENTS(keys); i++)
            if (strlen(keys[i]) == (gsize)(equals - *pair) && g_str_has_prefix(*pair, keys[i])) break;
        if (!equals || i == G_N_ELEMENTS(keys) || seen[i]) valid = FALSE;
        else seen[i] = TRUE;
    }
    g_strfreev(pairs);
    return valid;
}

                                                                           
static gboolean
_audit_ts_valid(const gchar *value)
{
    static const gchar shape[] = "dddd-dd-dd dd:dd:dd";                                                
    gint y, mo, d, h, mi, s;
    if (strlen(value) != strlen(shape)) return FALSE;
    for (gsize i = 0; shape[i]; i++)
        if (shape[i] == 'd' ? !g_ascii_isdigit(value[i]) : value[i] != shape[i]) return FALSE;
    if (sscanf(value, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return FALSE;
    GDateTime *dt = g_date_time_new_utc(y, mo, d, h, mi, (gdouble)s);
    if (!dt) return FALSE;
    g_date_time_unref(dt);
    return TRUE;
}

                                                                                          
                                                                                         
static gboolean
_audit_query_convert(GHashTable *query, JsonObject *params)
{
    static const gchar *const text_keys[] = { "from_ts", "to_ts", "username", "method" };
    for (guint i = 0; i < G_N_ELEMENTS(text_keys); i++) {
        const gchar *value = g_hash_table_lookup(query, text_keys[i]);
        if (!value) continue;
        if (!*value || !g_utf8_validate(value, -1, NULL) || (i < 2 && !_audit_ts_valid(value))) return FALSE;
        json_object_set_string_member(params, text_keys[i], value);
    }
    static const gchar *const int_keys[] = { "limit", "offset" };
    for (guint i = 0; i < G_N_ELEMENTS(int_keys); i++) {
        const gchar *value = g_hash_table_lookup(query, int_keys[i]);
        gint64 parsed = 0;
        if (!value) continue;
        if (!_strict_nonnegative_decimal(value, &parsed) || parsed > G_MAXINT / 2 || (i == 0 && parsed < 1)) return FALSE;
        json_object_set_int_member(params, int_keys[i], parsed);
    }
    return TRUE;
}

                                                                                                           
                                                                                          
                                                                              
                                                                           
JsonObject *
pcv_rest_audit_search_params(SoupServerMessage *message, GHashTable *query, JsonObject *body)
{
    GUri *uri = soup_server_message_get_uri(message);
    const gchar *raw_query = uri ? g_uri_get_query(uri) : NULL;
    JsonObject *params = body ? json_object_ref(body) : json_object_new();
    if (!raw_query || !*raw_query)
        return params;
    if (!query || !_audit_query_keys_valid(raw_query) || !_audit_query_convert(query, params)) {
        json_object_unref(params);
        params = json_object_new();
        json_object_set_boolean_member(params, "invalid_query", TRUE);
    }
    return params;
}

                                                                               
                                                                    
                                                                  
   
                                                                      
  
                                                
                                                
                                                       
  
                                                                                             
                                                                                       
                                                                   
                                                            
                                             
                                                         
                                                        
   
gchar *
pcv_rest_rpc_attach_auth_context(const gchar *rpc_json,
                         const gchar *subject,
                         PcvRole      role)
{
                
                                                 
                                                            
                                                      
                                        
      
               
                                                     
                                                           
                                                   
    if (!rpc_json)
        return NULL;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
                                                                                                                                                      
    if (!json_parser_load_from_data(parser, rpc_json, -1, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *params = NULL;
    JsonNode *params_node = json_object_get_member(root_obj, "params");
    if (params_node && JSON_NODE_HOLDS_OBJECT(params_node)) {
        params = json_node_get_object(params_node);
    } else {
        params = json_object_new();
        json_object_set_object_member(root_obj, "params", params);
    }

    json_object_remove_member(params, "_pcv_caller_sub");
    json_object_remove_member(params, "_pcv_caller_role");
    json_object_remove_member(params, "_pcv_caller_tenant");                                             
    if (subject && *subject)
        json_object_set_string_member(params, "_pcv_caller_sub", subject);
    json_object_set_int_member(params, "_pcv_caller_role", (gint)role);

    gchar *rpc_with_context = json_to_string(root, FALSE);
    g_object_unref(parser);
    return rpc_with_context ? rpc_with_context : g_strdup(rpc_json);
}

                                                                  
           
                                                                     
   
                     
  
                                                    
                                                      
                               
                                                                
                                                                      
  
                                                
                                       
                                    
  
          
                                                        
                                                             
  
                         
                                                
   
JsonObject *
pcv_rest_parse_body(SoupServerMessage *msg)
{
                                                      
                                                               
    SoupMessageBody *body = soup_server_message_get_request_body(msg);
    if (!body || body->length == 0)
        return json_object_new();

    const gchar *data = body->data;
    gsize        len  = (gsize)body->length;
    if (len > REST_MAX_BODY) return NULL;

                                                                
                                                              
                                                         
                                                                  
                                                            
    JsonParser *p = nullptr;
    GError     *e = nullptr;
    if (!pcv_rpc_parse_guarded(data, (gssize)len, &p, &e)) {
        if (e) g_error_free(e);
        return NULL;
    }

                             
      
                                                               
                                                               
      
                                               
                                             
      
                                                
                                                         
                                                
      
                                                  
    JsonNode   *root = json_parser_get_root(p);
    JsonObject *obj  = JSON_NODE_HOLDS_OBJECT(root)
                       ? json_object_ref(json_node_get_object(root))
                       : json_object_new();
    g_object_unref(p);                                        
    return obj;
}

                                                                  
                 
                                                                                   
                                                                     
   
                      
  
                                                                   
                                                      
                                                            
  
                                                  
                                                       
  
                                                                  
                                               
  
                                                       
                                                 
   
gchar **
pcv_rest_split_path(const gchar *path)
{
    const gchar *p = path;
    if (g_str_has_prefix(p, REST_API_PREFIX))
        p += strlen(REST_API_PREFIX);
    if (*p == '/') p++;
    return g_strsplit(p, "/", -1);
}

                                                       
                                                             
                                                  
                                            
                                                  
                                                    
                                                  
                                                         
gboolean
pcv_rest_body_has_secret(const gchar *body, gsize len)
{
    if (!body || len == 0)
        return FALSE;
    static const gchar *const secret_keys[] = {
        "password", "passwd", "secret", "api_key", "apikey", "token",
        "chap_password", "jwt_secret", "private_key", "credential",
        "totp", "recovery_code",
        "webhook",                                                                         
        "user_data",                                                                           
        nullptr
    };
    gsize scan_len = MIN(len, (gsize)4096);                               
    gchar *lower = g_ascii_strdown(body, (gssize)scan_len);
    gboolean found = FALSE;
    for (gsize i = 0; secret_keys[i]; i++) {
        if (strstr(lower, secret_keys[i])) { found = TRUE; break; }
    }
    g_free(lower);
    return found;
}
