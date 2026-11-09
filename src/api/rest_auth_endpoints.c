   
                              
                                                                                    
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                         
  
                                                 
   

#include "api/drain.h"
#include "api/rest_shutdown.h"
#include "api/rest_server.h"
#include "api/rest_middleware.h"
#include "api/rest_auth.h"
#include "api/rest_client_identity.h"
#include "api/rest_login.h"
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
#include "api/rest_async.h"
#include "api/rest_transport.h"
#include "api/rest_request_auth.h"

                                                                                                               
                                                         
                                                                                            
                                                                                 
static gboolean
_subject_gate(SoupServerMessage *msg, const gchar *sub, const gchar *verified_token, gboolean *out_sent)
{
    *out_sent = FALSE;
    PcvSubjectCheck who = pcv_rbac_subject_check_token(sub, verified_token, NULL);
    if (who == PCV_SUBJECT_UNAVAILABLE) {
        pcv_rest_send_error(msg, 503, "SERVICE_UNAVAILABLE", "Authentication store unavailable");
        *out_sent = TRUE;
    }
    return who == PCV_SUBJECT_OK;
}

                                                                                                                                                                         
                                                    
PcvRestStep
pcv_rest_auth_token(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                             
      
                                                                             
                                                                               
                                                       
      
                                                                           
                                                                                 
      
             
                                        
                                                                       
                                                 
                                                                               
                                   
      
                                                 
                                                         
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/token") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = pcv_rest_parse_body(msg);
        const gchar *username_in = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password_in = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";
        gchar *username = g_strdup(username_in ? username_in : "");
        gchar *password = g_strdup(password_in ? password_in : "");

                                                     
        const gchar *client_ip = request_client_ip;
        {
            gint ip_lockout = pcv_rbac_get_ip_remaining_lockout(client_ip);
            if (ip_lockout > 0) {
                if (body) json_object_unref(body);
                g_free(username);
                pcv_secure_free_str(&password);
                SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", ip_lockout);
                soup_message_headers_replace(rh, "Retry-After", retry_buf);
                pcv_rest_send_error(msg, 429, "TOO_MANY_REQUESTS",
                       "Too many failed login attempts from this IP");
                return PCV_REST_STEP_CLEANUP;
            }
        }

                                                                                                     
                                                 
        pcv_rest_login_start(msg, rq->self->rest_ctx, username, password, client_ip);
        if (body) json_object_unref(body);
        g_free(username);
        pcv_secure_free_str(&password);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                     
                                                    
PcvRestStep
pcv_rest_auth_refresh(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
      
                                                                       
      
                                             
                                                                
                                                                                      
      
                                                 
                                              
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/refresh") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = pcv_rest_parse_body(msg);
        const gchar *ref_tok  = body
            ? json_object_get_string_member_with_default(body, "refresh_token", "")
            : "";

        if (!ref_tok || !*ref_tok) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "refresh_token is required");
            return PCV_REST_STEP_CLEANUP;
        }

        GError *err = nullptr;
        gchar *new_refresh = nullptr;
        gchar *new_access = pcv_rbac_refresh_token(ref_tok, &new_refresh, &err);

        if (!new_access) {
            if (body) json_object_unref(body);
            g_free(new_refresh);
            if (err && err->domain == G_IO_ERROR && err->code == G_IO_ERROR_FAILED)                                 
                pcv_rest_send_error(msg, 503, "SERVICE_UNAVAILABLE", "Authentication store unavailable");
            else
                pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
                       err ? err->message : "Invalid refresh token");
            if (err) g_error_free(err);
            return PCV_REST_STEP_CLEANUP;
        }

        gchar *resp = nullptr;
        if (new_refresh) {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900,"
                "\"refresh_expires_in\":604800}",
                new_access, new_refresh);
        } else {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900}",
                new_access);
        }
        pcv_rest_send_json(msg, 200, resp);
        g_free(resp);
        g_free(new_access);
        g_free(new_refresh);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Token refreshed via /auth/refresh");
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                               
                                                                                 
                                                         
static void
_logout_blacklist_access(const gchar *auth_hdr)
{
    const gchar *tk = auth_hdr;
    if (g_str_has_prefix(tk, "Bearer ")) tk += 7;
    if (g_str_has_prefix(tk, "bearer ")) tk += 7;
                                         
    gchar **parts = g_strsplit(tk, ".", 3);
    if (parts && parts[0] && parts[1]) {
        gsize plen = 0;
                             
        gchar *p1 = g_strdup(parts[1]);
        gsize p1len = strlen(p1);
                                              
        for (gchar *c = p1; *c; c++) {
            if (*c == '-') *c = '+';
            else if (*c == '_') *c = '/';
        }
                       
        gsize pad = (4 - (p1len % 4)) % 4;
        gchar *padded = g_strconcat(p1, pad >= 1 ? "=" : "",
                                    pad >= 2 ? "=" : "",
                                    pad >= 3 ? "=" : "", NULL);
        guchar *raw = g_base64_decode(padded, &plen);
        g_free(padded); g_free(p1);
        if (raw && plen > 0) {
            gchar *json_str = g_strndup((const gchar *)raw, plen);
            JsonParser *parser = json_parser_new();
                                                                                                          
            if (json_parser_load_from_data(parser, json_str, -1, NULL)) {
                JsonObject *o = json_node_get_object(json_parser_get_root(parser));
                const gchar *jti = json_object_get_string_member_with_default(o, "jti", NULL);
                gint64 exp = json_object_get_int_member_with_default(o, "exp", 0);
                if (jti && exp > 0) {
                    pcv_jwt_blacklist_add(jti, exp);
                }
            }
            g_object_unref(parser);
            g_free(json_str);
        }
        g_free(raw);
    }
    g_strfreev(parts);
}

                                                                                       
                                                                                          
static gboolean
_logout_body_refresh(JsonObject *body, const gchar **out)
{
    *out = nullptr;
    if (!body)
        return FALSE;
    JsonNode *node = json_object_get_member(body, "refresh_token");
    if (!node)
        return TRUE;
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const gchar *s = json_node_get_string(node);
    if (!s || !*s)
        return FALSE;
    *out = s;
    return TRUE;
}

                                                                                                         
                                                                              
                                                                             
PcvRestStep
pcv_rest_auth_logout(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                                                    
      
                                                                                          
                                                                                      
                                                                                                 
                                                                             
                                                                                              
       
    const gboolean all = g_strcmp0(path, REST_API_PREFIX "/auth/logout/all") == 0;
    if ((all || g_strcmp0(path, REST_API_PREFIX "/auth/logout") == 0)
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject *body = pcv_rest_parse_body(msg);
        const gchar *refresh = nullptr;
        if (!_logout_body_refresh(body, &refresh)) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "refresh_token must be a non-empty string");
            return PCV_REST_STEP_CLEANUP;
        }

        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        gchar *sub = nullptr;
        if (auth_hdr) {
            GError *verr = nullptr;
            sub = pcv_jwt_verify(auth_hdr, &verr);
            if (verr) g_error_free(verr);
            gboolean sent = FALSE;
            if (sub && !_subject_gate(msg, sub, auth_hdr, &sent)) {
                g_clear_pointer(&sub, g_free);                                 
                if (sent) {
                    if (body) json_object_unref(body);
                    return PCV_REST_STEP_CLEANUP;
                }
            }
        }

                                                           
                                                                            
        gchar *owner = nullptr;
        gint revoked = 0;
        if (refresh && !(all && sub))
            revoked = pcv_rbac_revoke_session_by_token(refresh, sub, &owner);
        const gchar *who = sub ? sub : owner;

        gboolean store_ok = revoked >= 0;
        if (store_ok && who && all) {
            GError *err = nullptr;
            store_ok = pcv_rbac_revoke_session(who, &err);
            if (err) g_error_free(err);
        }
        if (sub)
            _logout_blacklist_access(auth_hdr);

        if (!store_ok) {
            pcv_rest_send_error(msg, 500, "SERVER_ERROR", "Session store unavailable");
        } else if (!who) {
            pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Valid token required for logout");
        } else {
            const gchar *resp = all
                ? "{\"status\":\"ok\",\"scope\":\"all\",\"message\":\"All sessions revoked\"}"
                : !refresh
                ? "{\"status\":\"ok\",\"scope\":\"access\",\"message\":\"Access token revoked\"}"
                : revoked == 1
                ? "{\"status\":\"ok\",\"scope\":\"session\",\"session_revoked\":true,\"message\":\"Session revoked\"}"
                : "{\"status\":\"ok\",\"scope\":\"session\",\"session_revoked\":false,\"message\":\"No active session matched\"}";
            pcv_rest_send_json(msg, 200, resp);
            pcv_audit_log(who, all ? "auth.logout.all" : "auth.logout", who, "ok", 0, 0, request_client_ip);
            PCV_LOG_INFO(REST_LOG_DOM, "Logout (%s) for '%s'",
                         all ? "all" : refresh ? "session" : "access", who);
        }
        g_free(owner);
        g_free(sub);
        if (body) json_object_unref(body);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                            
                                                    
PcvRestStep
pcv_rest_auth_password(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                        
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/password") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        if (!auth_hdr || !g_str_has_prefix(auth_hdr, "Bearer ")) {
            pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Bearer token required");
            return PCV_REST_STEP_CLEANUP;
        }
        GError *verr = nullptr;
        gchar *sub = pcv_jwt_verify(auth_hdr, &verr);
        if (verr) g_error_free(verr);
        gboolean sent = FALSE;
        if (sub && !_subject_gate(msg, sub, auth_hdr, &sent))
            g_clear_pointer(&sub, g_free);
        if (!sub) {
            if (!sent) pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Invalid or expired token");
            return PCV_REST_STEP_CLEANUP;
        }

        JsonObject *body = pcv_rest_parse_body(msg);
        const gchar *old_pw = body
            ? json_object_get_string_member_with_default(body, "old_password", "")
            : "";
        const gchar *new_pw = body
            ? json_object_get_string_member_with_default(body, "new_password", "")
            : "";

        if (!old_pw || !*old_pw || !new_pw || !*new_pw) {
            if (body) json_object_unref(body);
            g_free(sub);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST",
                   "old_password and new_password are required");
            return PCV_REST_STEP_CLEANUP;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_change_password(sub, old_pw, new_pw, &err);

        const gchar *rip = request_client_ip;

        if (!ok) {
            int code = err && err->code == G_IO_ERROR_PERMISSION_DENIED ? 401 :
                       err && err->code == G_IO_ERROR_INVALID_ARGUMENT ? 400 : 500;
            const char *ec = code == 401 ? "UNAUTHORIZED" :
                             code == 400 ? "BAD_REQUEST" : "SERVER_ERROR";
            pcv_audit_log(sub, "auth.password.change", "self change",
                          "fail", code, 0, rip);
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, code, ec,
                   err ? err->message : "Failed to change password");
            if (err) g_error_free(err);
            g_free(sub);
            return PCV_REST_STEP_CLEANUP;
        }

        pcv_audit_log(sub, "auth.password.change", "self change",
                      "ok", 0, 0, rip);
        PCV_LOG_INFO(REST_LOG_DOM, "Password changed for '%s' (refresh sessions revoked)", sub);
        g_free(sub);
        if (body) json_object_unref(body);

        pcv_rest_send_json(msg, 200,
                   "{\"status\":\"ok\",\"message\":\"Password changed. Refresh sessions revoked; existing access tokens remain valid until expiry.\"}");
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                            
                                                    
PcvRestStep
pcv_rest_auth_register(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                         
      
                                                         
                                                                 
      
                                                         
                                                                 
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/register") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        const gchar *enabled = pcv_config_get_string("auth", "allow_self_register", "false");
        if (g_ascii_strcasecmp(enabled, "true") != 0 &&
            g_ascii_strcasecmp(enabled, "1")    != 0 &&
            g_ascii_strcasecmp(enabled, "yes")  != 0) {
            pcv_rest_send_error(msg, 403, "FORBIDDEN", "Self-registration is disabled");
            return PCV_REST_STEP_CLEANUP;
        }

        JsonObject *body = pcv_rest_parse_body(msg);
        const gchar *username = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";

        size_t ulen = username ? strlen(username) : 0;
        gboolean uvalid = (ulen >= 3 && ulen <= 32);
        for (size_t i = 0; uvalid && i < ulen; i++) {
            char c = username[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                uvalid = FALSE;
        }
        if (!uvalid) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST",
                   "username must be 3-32 chars, lowercase alphanumeric or underscore");
            return PCV_REST_STEP_CLEANUP;
        }

        size_t plen = password ? strlen(password) : 0;
        if (plen < 8) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "password must be at least 8 characters");
            return PCV_REST_STEP_CLEANUP;
        }
        if (g_strcmp0(username, password) == 0) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "password must differ from username");
            return PCV_REST_STEP_CLEANUP;
        }

        const gchar *adm = pcv_config_get_admin_user();
        if (adm && g_strcmp0(username, adm) == 0) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 409, "CONFLICT", "username is reserved");
            return PCV_REST_STEP_CLEANUP;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_user_create(username, password,
                                           PCV_ROLE_VIEWER, NULL, &err);

        const gchar *rip = request_client_ip;

        if (!ok) {
            pcv_audit_log(username, "auth.register", "self register",
                          "fail", 409, 0, rip);
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 409, "CONFLICT",
                   err ? err->message : "Failed to create user (already exists?)");
            if (err) g_error_free(err);
            return PCV_REST_STEP_CLEANUP;
        }

        pcv_audit_log(username, "auth.register", "self register",
                      "ok", 0, 0, rip);

        gchar *resp = g_strdup_printf(
            "{\"status\":\"ok\",\"username\":\"%s\",\"role\":\"viewer\"}", username);
        pcv_rest_send_json(msg, 201, resp);
        g_free(resp);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Self-registered user '%s'", username);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                                
                                                    
PcvRestStep
pcv_rest_auth_totp_verify(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                                              
      
                                                         
                                                         
                                                    
                                                            
                                                  
      
                            
                                                                
                                                                      
                                                          
                                              
          
                                               
                                                      
                                                                     
                                                   
                                                         
                                                
                                                                       
                                                             
      
                                                            
                                                          
                                                    
                                                 
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/verify") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body = pcv_rest_parse_body(msg);
        const gchar *pending_tok = body
            ? json_object_get_string_member_with_default(body, "pending_token", "")
            : "";
        const gchar *code = body
            ? json_object_get_string_member_with_default(body, "code", "")
            : "";
        const gchar *client_ip = request_client_ip;

        if (!code || !*code) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "code is required");
            return PCV_REST_STEP_CLEANUP;
        }

        gboolean is_pending_path = (pending_tok && *pending_tok);
        gchar   *username = nullptr;

        if (is_pending_path) {
                                      
            GError *verr  = nullptr;
            gchar  *scope = nullptr;
            username = pcv_jwt_verify_scoped(pending_tok, client_ip, &scope, &verr);
            gboolean sent = FALSE;
            if (username && g_strcmp0(scope, "totp") == 0 && !_subject_gate(msg, username, pending_tok, &sent)) {
                g_clear_pointer(&username, g_free);                                         
                if (sent) {
                    if (body) json_object_unref(body);
                    g_free(scope);
                    if (verr) g_error_free(verr);
                    return PCV_REST_STEP_CLEANUP;
                }
            }
            if (!username || g_strcmp0(scope, "totp") != 0) {
                gboolean expired = verr && verr->code == G_IO_ERROR_TIMED_OUT;
                if (body) json_object_unref(body);
                g_free(username);
                g_free(scope);
                if (expired) {
                    pcv_rest_send_error(msg, 401, "TOTP_PENDING_EXPIRED",
                           "Pending token expired — please log in again");
                } else {
                    pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Invalid pending token");
                }
                if (verr) g_error_free(verr);
                return PCV_REST_STEP_CLEANUP;
            }
            g_free(scope);
        } else {
                                                                         
            SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
            const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
            if (!auth_hdr || !g_str_has_prefix(auth_hdr, "Bearer ")) {
                if (body) json_object_unref(body);
                pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
                       "pending_token or Authorization: Bearer token required");
                return PCV_REST_STEP_CLEANUP;
            }
            GError *verr = nullptr;
            username = pcv_jwt_verify(auth_hdr, &verr);
            gboolean sent = FALSE;
            if (username && !_subject_gate(msg, username, auth_hdr, &sent)) {
                g_clear_pointer(&username, g_free);
                if (sent) {
                    if (body) json_object_unref(body);
                    if (verr) g_error_free(verr);
                    return PCV_REST_STEP_CLEANUP;
                }
            }
            if (!username) {
                if (body) json_object_unref(body);
                pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
                       verr ? verr->message : "Invalid or expired token");
                if (verr) g_error_free(verr);
                return PCV_REST_STEP_CLEANUP;
            }
            if (verr) g_error_free(verr);
        }

        gboolean first_confirm = FALSE;
        GError  *cerr = nullptr;
                                                  
                                               
        gboolean allow_recovery = is_pending_path;
        if (!pcv_rbac_totp_verify_code(username, code, allow_recovery, &first_confirm, &cerr)) {
                                                          
                                                          
                                                      
            pcv_audit_log(username, "auth.totp.verify.failed", username,
                          "fail", 401, 0, client_ip);

            gint lock_sec = pcv_rbac_totp_get_remaining_lockout(username);
            if (body) json_object_unref(body);
            if (lock_sec > 0) {
                SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", lock_sec);
                soup_message_headers_replace(rh, "Retry-After", retry_buf);
                pcv_rest_send_error(msg, 429, "TOTP_LOCKED", "Too many failed TOTP attempts");
            } else {
                pcv_rest_send_error(msg, 401, "TOTP_INVALID_CODE",
                       cerr ? cerr->message : "Invalid TOTP code");
            }
            if (cerr) g_error_free(cerr);
            g_free(username);
            return PCV_REST_STEP_CLEANUP;
        }

                                                  
                                                              
                                                
                        
        GPtrArray *recovery = nullptr;
        if (first_confirm) {
            GError *rerr = nullptr;
            recovery = pcv_rbac_totp_generate_recovery(username, &rerr);
            if (!recovery) {
                PCV_LOG_WARN(REST_LOG_DOM,
                    "TOTP first-confirm recovery code generation failed for '%s': %s",
                    username, rerr ? rerr->message : "unknown");
            }
            if (rerr) g_error_free(rerr);
        }

        if (!is_pending_path) {
                                                      
                                           
            pcv_audit_log(username, "auth.totp.verify", username, "ok", 0, 0, client_ip);

            GString *resp = g_string_new(NULL);
            if (recovery) {
                g_string_append(resp, "{\"confirmed\":true,\"recovery_codes\":[");
                for (guint i = 0; i < recovery->len; i++) {
                    if (i > 0) g_string_append_c(resp, ',');
                    g_string_append_printf(resp, "\"%s\"",
                        (const gchar *)g_ptr_array_index(recovery, i));
                }
                g_string_append(resp, "]}");
            } else {
                g_string_append(resp, "{\"verified\":true}");
            }
            pcv_rest_send_json(msg, 200, resp->str);
            g_string_free(resp, TRUE);
            if (recovery) g_ptr_array_unref(recovery);
            if (body) json_object_unref(body);
            PCV_LOG_INFO(REST_LOG_DOM, "TOTP self-service confirm for '%s'%s",
                         username, first_confirm ? " (first confirm)" : "");
            g_free(username);
            return PCV_REST_STEP_CLEANUP;
        }

                                                                    
                                                                                            
        gchar  *refresh = nullptr;
        GError *ierr    = nullptr;
        g_autofree gchar *pending_nonce = pcv_jwt_identity_nonce(pending_tok);
        gchar  *token   = pcv_rbac_issue_tokens(username, pending_nonce, &refresh, &ierr);
        if (!token) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 500, "INTERNAL",
                   ierr ? ierr->message : "Failed to issue tokens");
            if (ierr) g_error_free(ierr);
            g_free(refresh);
            if (recovery) g_ptr_array_unref(recovery);
            g_free(username);
            return PCV_REST_STEP_CLEANUP;
        }

        pcv_audit_log(username, "auth.totp.verify", username, "ok", 0, 0, client_ip);

        GString *resp = g_string_new(NULL);
        g_string_append_printf(resp, "{\"access_token\":\"%s\",", token);
        if (refresh) {
            g_string_append_printf(resp, "\"refresh_token\":\"%s\",", refresh);
        }
        g_string_append(resp, "\"token_type\":\"Bearer\",\"expires_in\":900");
        if (refresh) {
            g_string_append(resp, ",\"refresh_expires_in\":604800");
        }
        if (recovery) {
            g_string_append(resp, ",\"recovery_codes\":[");
            for (guint i = 0; i < recovery->len; i++) {
                if (i > 0) g_string_append_c(resp, ',');
                g_string_append_printf(resp, "\"%s\"",
                    (const gchar *)g_ptr_array_index(recovery, i));
            }
            g_string_append_c(resp, ']');
        }
        g_string_append_c(resp, '}');

        pcv_rest_send_json(msg, 200, resp->str);
        g_string_free(resp, TRUE);
        g_free(token);
        g_free(refresh);
        if (recovery) g_ptr_array_unref(recovery);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "TOTP verify completed login for '%s'%s",
                     username, first_confirm ? " (first confirm)" : "");
        g_free(username);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}

                                                                                                                                                             
                                                    
PcvRestStep
pcv_rest_auth_totp_enroll(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    const gchar *path = rq->path;
    const gchar *method = rq->method;
    const gchar *request_client_ip = rq->request_client_ip;
      
                                                                                 
      
                                                         
                                                    
                                                      
                                                          
                                        
      
                  
                                                                             
                                                                         
                                                               
      
                                                         
                                                  
                                                 
                                                 
                                                                
                                                                  
                                                           
                                               
      
                                           
                                                      
       
    if (g_strcmp0(path, REST_API_PREFIX "/auth/totp/enroll") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body = pcv_rest_parse_body(msg);
        const gchar *pending_tok = body
            ? json_object_get_string_member_with_default(body, "pending_token", "")
            : "";
        const gchar *client_ip = request_client_ip;
        gboolean     is_pending_path = (pending_tok && *pending_tok);
        gchar       *username = nullptr;
        const gchar *subject_token = pending_tok;                                                 

        if (is_pending_path) {
                                   
            GError *verr  = nullptr;
            gchar  *scope = nullptr;
            username = pcv_jwt_verify_scoped(pending_tok, client_ip, &scope, &verr);
            gboolean sent = FALSE;
            if (username && g_strcmp0(scope, "totp") == 0 && !_subject_gate(msg, username, pending_tok, &sent)) {
                g_clear_pointer(&username, g_free);                                         
                if (sent) {
                    if (body) json_object_unref(body);
                    g_free(scope);
                    if (verr) g_error_free(verr);
                    return PCV_REST_STEP_CLEANUP;
                }
            }
            if (!username || g_strcmp0(scope, "totp") != 0) {
                gboolean expired = verr && verr->code == G_IO_ERROR_TIMED_OUT;
                if (body) json_object_unref(body);
                g_free(username);
                g_free(scope);
                if (expired) {
                    pcv_rest_send_error(msg, 401, "TOTP_PENDING_EXPIRED",
                           "Pending token expired — please log in again");
                } else {
                    pcv_rest_send_error(msg, 401, "UNAUTHORIZED", "Invalid pending token");
                }
                if (verr) g_error_free(verr);
                return PCV_REST_STEP_CLEANUP;
            }
            g_free(scope);
        } else {
                                                                          
            SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
            const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
            if (!auth_hdr || !g_str_has_prefix(auth_hdr, "Bearer ")) {
                if (body) json_object_unref(body);
                pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
                       "pending_token or Authorization: Bearer token required");
                return PCV_REST_STEP_CLEANUP;
            }
            GError *verr = nullptr;
            username = pcv_jwt_verify(auth_hdr, &verr);
            gboolean sent = FALSE;
            if (username && !_subject_gate(msg, username, auth_hdr, &sent)) {
                g_clear_pointer(&username, g_free);
                if (sent) {
                    if (body) json_object_unref(body);
                    if (verr) g_error_free(verr);
                    return PCV_REST_STEP_CLEANUP;
                }
            }
            if (!username) {
                if (body) json_object_unref(body);
                pcv_rest_send_error(msg, 401, "UNAUTHORIZED",
                       verr ? verr->message : "Invalid or expired token");
                if (verr) g_error_free(verr);
                return PCV_REST_STEP_CLEANUP;
            }
            if (verr) g_error_free(verr);
            subject_token = auth_hdr;
        }

        GError *eerr   = nullptr;
        g_autofree gchar *enroll_nonce = pcv_jwt_identity_nonce(subject_token);
        gchar  *secret = pcv_rbac_totp_enroll(username, enroll_nonce, &eerr);
        if (!secret) {
            if (body) json_object_unref(body);
            pcv_rest_send_error(msg, 409, "CONFLICT",
                   eerr ? eerr->message : "TOTP enrollment failed");
            if (eerr) g_error_free(eerr);
            g_free(username);
            return PCV_REST_STEP_CLEANUP;
        }

        const gchar *hostname = g_get_host_name();
        gchar *secret_uri = pcv_totp_build_uri(
            username, hostname ? hostname : "purecvisor", secret);
        gchar *secret_response = g_strdup_printf(
            "{\"secret\":\"%s\",\"otpauth_uri\":\"%s\"}", secret, secret_uri);
        pcv_rest_send_json(msg, 200, secret_response);
                                                                             
                                                  
        pcv_secure_free_str(&secret_response);
        pcv_secure_free_str(&secret_uri);
        pcv_secure_free_str(&secret);                                   
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "TOTP enroll issued secret for '%s' (%s path)",
                     username, is_pending_path ? "pending" : "self-service");
        g_free(username);
        return PCV_REST_STEP_CLEANUP;
    }
    return PCV_REST_STEP_NEXT;
}
