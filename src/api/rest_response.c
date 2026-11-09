   
                        
                                                
  
                           
                                                                                                                                           
                                                                                                         
                                                                                                           
                                               
  
                                               
   

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

                                                                  
                              
                                                                
                                        
                      
                                                                     
   
                                                      
  
                                               
                                              
                        
  
                                                         
                                                                      
                                                      
                                                         
  
                                              
                                                   
  
         
                                                                     
                                                        
                            
                                                
                                                   
                                            
                         
  
                   
                                                               
                                    
                                                 
  
                                      
                                              
                                             
   
gchar *
pcv_rest_rpc_over_uds_timeout(const gchar *rpc_json, gint timeout_sec)
{
    const gchar *sock_path = pcv_config_get_socket_path();

                                                                   
      
                        
                                                           
                                      
                                                              
      
                  
                                                           
                                           
                                                                      
       
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return g_strdup_printf(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"socket() failed: %s\"}}", strerror(errno));
    }

                                                 
      
                      
                                                  
                                                       
      
             
                                  
                                       
                                                                    
                                                                     
       
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                              
      
             
                                                     
                                                                
                                           
                                                 
       
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        PCV_LOG_WARN("rest", "connect(%s) failed: %s", sock_path, strerror(errno));
        gchar *msg = g_strdup(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"Daemon temporarily unavailable\"}}");
        close(fd);
        return msg;
    }

                           
      
                          
                                                   
                                                  
      
                                                          
                                                    
       
    {
        gsize json_len = strlen(rpc_json);
        struct iovec iov[2] = {
            { .iov_base = (void *)rpc_json, .iov_len = json_len },
            { .iov_base = (void *)"\n",     .iov_len = 1        }
        };
        gssize total_len = (gssize)(json_len + 1);
        gssize sent = writev(fd, iov, 2);
        if (sent != total_len) {
            close(fd);
            return g_strdup("{\"error\":{\"code\":\"WRITE_FAILED\","
                             "\"message\":\"UDS write failed\"}}");
        }
    }

      
                                                           
                                                      
                                                            
       
    shutdown(fd, SHUT_WR);

                         
      
                   
                                         
                                                          
                                                             
      
                     
                                  
                                                   
       
                                                  
                                                  
                                           
                                          
    GByteArray *dyn_buf = g_byte_array_new();
    gchar tmp[8192];
    for (;;) {
        gssize n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {                                        
                                                   
                                                                    
                                                          
                                                    
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_byte_array_free(dyn_buf, TRUE);
                close(fd);
                return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"error\":{"
                    "\"code\":%d,"
                    "\"message\":\"RPC timeout — daemon did not respond within %ds\"}}",
                    PURE_RPC_ERR_TIMEOUT, timeout_sec);
            }
            break;
        }
        if (n == 0) break;
        g_byte_array_append(dyn_buf, (guint8 *)tmp, (guint)n);
    }
    close(fd);
    gssize total = (gssize)dyn_buf->len;

    if (total <= 0) {
        g_byte_array_free(dyn_buf, TRUE);
        return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"error\":{"
            "\"code\":%d,"
            "\"message\":\"No response from daemon\"}}", PURE_RPC_ERR_ZFS_OPERATION);
    }

                         
    g_byte_array_append(dyn_buf, (guint8 *)"\0", 1);
    if (total > 0 && dyn_buf->data[total - 1] == '\n')
        dyn_buf->data[total - 1] = '\0';

    gchar *line = g_strdup((gchar *)dyn_buf->data);
    g_byte_array_free(dyn_buf, TRUE);

    if (!line) {
        return g_strdup("{\"error\":{\"code\":\"READ_FAILED\","
                         "\"message\":\"No response from daemon\"}}");
    }

    return line;                              
}

                        
                                               
                                              
gchar *
pcv_rest_rpc_over_uds(const gchar *rpc_json)
{
    return pcv_rest_rpc_over_uds_timeout(rpc_json, REST_RPC_TIMEOUT_SEC);
}


                                                                  
             
                                                                     
   
                     
  
                                              
                                                    
                                                 
  
                                                              
                                                                  
                                           
  
                               
                                                    
                                                       
                                                  
  
                                               
                                                       
                               
   
void
pcv_rest_send_json(SoupServerMessage *msg, guint status, const gchar *body)
{
    soup_server_message_set_status(msg, status, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Content-Type",
                                  "application/json; charset=utf-8");
    soup_message_headers_replace(hdrs, "X-Content-Type-Options", "nosniff");

                                                                       
                                                        
                                                        
                                                       
    {
        const gchar *req_method = soup_server_message_get_method(msg);
        gboolean is_get = (g_strcmp0(req_method, "GET") == 0);

        if (is_get && status == 200 && body) {
            gsize body_len = strlen(body);
            gchar *etag = pcv_compute_etag(body, body_len);
            soup_message_headers_replace(hdrs, "ETag", etag);
            soup_message_headers_replace(hdrs, "Cache-Control",
                                          "private, max-age=5, must-revalidate");
                                                         
            SoupMessageHeaders *req_hdrs =
                soup_server_message_get_request_headers(msg);
            const gchar *if_none_match =
                soup_message_headers_get_one(req_hdrs, "If-None-Match");
            if (if_none_match && g_strcmp0(if_none_match, etag) == 0) {
                soup_server_message_set_status(msg, 304, NULL);
                g_free(etag);
                return;                                    
            }
            g_free(etag);
        } else {
            soup_message_headers_replace(hdrs, "Cache-Control", "no-store");
        }
    }

                                                   
                                                          
                                                           
                                             
    soup_message_headers_replace(hdrs, "Connection", "close");
                                                       
                                                     
                                                               
    const PcvRestClientIdentity *identity =
        pcv_rest_client_identity_get(msg);
    if (pcv_rest_hsts_enabled ||
        pcv_rest_client_identity_is_external_https(identity)) {
        soup_message_headers_replace(hdrs, "Strict-Transport-Security",
                                      "max-age=31536000; includeSubDomains");
    }
    soup_message_headers_replace(hdrs, "Content-Security-Policy", PCV_CSP_POLICY);
    soup_message_headers_replace(hdrs, "X-Frame-Options", "SAMEORIGIN");
    soup_message_headers_replace(hdrs, "X-XSS-Protection", "1; mode=block");
    soup_message_headers_replace(hdrs, "Referrer-Policy",
                                  "strict-origin-when-cross-origin");
    soup_message_headers_replace(hdrs, "Permissions-Policy",
                                  "camera=(), microphone=(), geolocation=()");
                                                          
    soup_message_headers_replace(hdrs, "Vary", "Accept-Encoding");

    gsize body_len = strlen(body);

                                                  
      
                        
                                                             
                                                
      
             
                                                             
                                                                         
                              
                                                    
                                                        
      
                   
                                                       
                                         
       
    SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
    const gchar *accept_enc = soup_message_headers_get_one(req_hdrs,
                                                            "Accept-Encoding");
    if (accept_enc && strstr(accept_enc, "gzip") != nullptr && body_len > 256) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
                                                          
        int zinit_ret = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                     15 + 16, 8, Z_DEFAULT_STRATEGY);
        if (zinit_ret != Z_OK) {
                                                               
            g_warning("rest_server: deflateInit2 failed (ret=%d), sending uncompressed",
                      zinit_ret);
        }
        if (zinit_ret == Z_OK) {
            gsize max_compressed = deflateBound(&zs, (uLong)body_len);
            guchar *compressed = g_malloc(max_compressed);
            zs.next_in  = (Bytef *)body;
            zs.avail_in = (uInt)body_len;
            zs.next_out  = compressed;
            zs.avail_out = (uInt)max_compressed;

            int zret = deflate(&zs, Z_FINISH);
            if (zret == Z_STREAM_END) {
                gsize compressed_len = zs.total_out;
                deflateEnd(&zs);
                                                                     
                if (compressed_len < body_len) {
                    soup_message_headers_replace(hdrs, "Content-Encoding",
                                                  "gzip");
                    soup_server_message_set_response(msg, "application/json",
                                                      SOUP_MEMORY_COPY,
                                                      (const gchar *)compressed,
                                                      compressed_len);
                    g_free(compressed);
                    return;
                }
            } else {
                deflateEnd(&zs);
            }
            g_free(compressed);
        }
    }

                               
    soup_server_message_set_response(msg, "application/json",
                                      SOUP_MEMORY_COPY,
                                      body, body_len);
}

   
                   
  
                                              
                             
                                                                     
                                                            
  
                                                            
                                         
  
                                  
                           
                                                           
                                   
   
void
pcv_rest_send_error(SoupServerMessage *msg, guint status,
       const gchar *code, const gchar *detail)
{
    gchar *body = g_strdup_printf(
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code, detail);
    pcv_rest_send_json(msg, status, body);
    g_free(body);
}

                                                                  
                      
                                                                     
   
                      
  
                                                       
                                                             
                                                   
  
                                                      
  
         
                                                      
                                                                        
  
                                  
                                              
                                              
                                               
                                               
                                               
                                   
  
                             
                                            
   

                                                      

   
                                                       
  
                                                 
                                
                                                               
                                                    
  
                         
                                      
  
                                     
                           
                          
                          
                                             
   
static void
_add_pagination_headers(SoupServerMessage *msg, gint total, gint offset,
                        gint limit, const gchar *base_path)
{
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);

    gchar *total_str = g_strdup_printf("%d", total);
    soup_message_headers_replace(hdrs, "X-Total-Count", total_str);
    g_free(total_str);

                                               
    GString *links = g_string_new("");
    if (offset + limit < total) {
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"next\"",
                               base_path, offset + limit, limit);
    }
    if (offset > 0) {
        if (links->len > 0) g_string_append(links, ", ");
        gint prev_offset = offset - limit;
        if (prev_offset < 0) prev_offset = 0;
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"prev\"",
                               base_path, prev_offset, limit);
    }
    if (links->len > 0)
        soup_message_headers_replace(hdrs, "Link", links->str);
    g_string_free(links, TRUE);
}

   
                                                                       
  
                                                  
                                           
  
                                               
                                               
                                                    
               
  
                                                                        
                                                               
                                                
                                          
                                                      
                                                  
                                                                   
                                           
                                    
                                                    
   
void
pcv_rest_send_rpc_result(SoupServerMessage *msg, const gchar *rpc_resp)
{
    if (!rpc_resp) {
        pcv_rest_send_error(msg, 500, "NO_RESPONSE", "No response from daemon");
        return;
    }

                                                           
                                                    
                                    
    JsonParser *p   = json_parser_new();
    GError     *err = nullptr;

                                                                  
    if (!json_parser_load_from_data(p, rpc_resp, -1, &err)) {
        pcv_rest_send_error(msg, 500, "PARSE_ERROR",
               err ? err->message : "Invalid JSON from daemon");
        if (err) g_error_free(err);
        g_object_unref(p);
        return;
    }

    JsonObject    *obj = json_node_get_object(json_parser_get_root(p));
    gchar         *body_str;
    guint          status;

                           
      
                                                                  
                                                       
      
                                                                                            
                                                               
      
                     
                                                         
                                                                    
       
    if (json_object_has_member(obj, "result")) {
                                                     
        JsonNode *result = json_object_get_member(obj, "result");
        gchar *r = json_to_string(result, FALSE);
        body_str = g_strdup_printf("{\"data\":%s}", r);
        g_free(r);
        status = 200;

                                         
        {
            GUri *uri = soup_server_message_get_uri(msg);
            const gchar *req_path = uri ? g_uri_get_path(uri) : NULL;
            const gchar *req_query = uri ? g_uri_get_query(uri) : NULL;
            if (req_path && JSON_NODE_HOLDS_ARRAY(result)) {
                gint total = (gint)json_array_get_length(json_node_get_array(result));
                gint offset = 0, limit = total;
                if (req_query) {
                                                 
                    gchar **pairs = g_strsplit(req_query, "&", -1);
                    for (gchar **p = pairs; *p; p++) {
                        if (g_str_has_prefix(*p, "offset="))
                            offset = (gint)g_ascii_strtoll(*p + 7, NULL, 10);
                        else if (g_str_has_prefix(*p, "limit="))
                            limit = (gint)g_ascii_strtoll(*p + 6, NULL, 10);
                    }
                    g_strfreev(pairs);
                }
                if (limit > 0)
                    _add_pagination_headers(msg, total, offset, limit, req_path);
            }
        }
    } else if (json_object_has_member(obj, "error")) {
                                                      
        JsonObject *e    = json_object_get_object_member(obj, "error");
        gint64      code = json_object_get_int_member_with_default(e, "code", PURE_RPC_ERR_ZFS_OPERATION);
        JsonNode   *enode = json_object_get_member(obj, "error");
        gchar *r = json_to_string(enode, FALSE);
        body_str = g_strdup_printf("{\"error\":%s}", r);
        g_free(r);
        status = pcv_rest_http_status_for_rpc_error(code);
    } else {
        body_str = g_strdup(rpc_resp);
        status   = 200;
    }

    pcv_rest_send_json(msg, status, body_str);
    g_free(body_str);
    g_object_unref(p);
}
