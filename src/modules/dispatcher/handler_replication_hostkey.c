   
                                      
                                                                          
  
                           
                                                                                   
                                                                             
                                                                                   
                                                                             
  
                                                         
                                                                                 
                                                                     
   
#include "handler_replication_hostkey.h"
#include "rpc_utils.h"
#include "api/drain.h"
#include "rpc_completion.h"                                  
#include "../backup/replication_hostkey.h"
#include "../audit/pcv_audit.h"
#include "../../api/uds_server.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

                                                                      
typedef struct {
    UdsServer         *server;
    GSocketConnection *connection;
    gchar             *rpc_id;
    gchar             *target;                       
    gchar             *fingerprint;                               
} HostkeyCtx;

                                                                                      
static void
_hk_ctx_free(gpointer data)
{
    HostkeyCtx *ctx = data;
    if (!ctx)
        return;
    g_free(ctx->rpc_id);
    g_free(ctx->target);
    g_free(ctx->fingerprint);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                            
static void
_hk_send_error(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id,
               gint code, const gchar *message)
{
    gchar *r = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, r);
    g_free(r);
}

                               
static void
_hk_send_result(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id, JsonObject *res)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r);
    g_free(r);
}

                                                                        
static JsonObject *
_hk_key_json(const PcvReplHostKey *k)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "type", k->type);
    json_object_set_string_member(o, "fingerprint", k->fingerprint);
    return o;
}

                                                                           
static JsonObject *
_hk_scan_result(const HostkeyCtx *ctx, GPtrArray *keys, GError **error)
{
    GError *e = NULL;
    PcvReplHostKey *pinned = pcv_repl_hostkey_lookup(ctx->target, &e);
    if (e) {
        g_propagate_error(error, e);
        return NULL;
    }
    gboolean seen = FALSE;
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < keys->len; i++) {
        const PcvReplHostKey *k = g_ptr_array_index(keys, i);
        json_array_add_object_element(arr, _hk_key_json(k));
        if (pinned && g_str_equal(pinned->type, k->type) && g_str_equal(pinned->blob_b64, k->blob_b64))
            seen = TRUE;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "target_node", ctx->target);
    json_object_set_string_member(res, "state", !pinned ? "untrusted" : seen ? "trusted" : "mismatch");
    json_object_set_array_member(res, "keys", arr);
    if (pinned)
        json_object_set_object_member(res, "trusted", _hk_key_json(pinned));
    else
        json_object_set_null_member(res, "trusted");
    pcv_repl_host_key_free(pinned);
    return res;
}

                                                               
static JsonObject *
_hk_trust_result(const HostkeyCtx *ctx, GPtrArray *keys, GError **error)
{
    const PcvReplHostKey *match = NULL;
    for (guint i = 0; i < keys->len && !match; i++) {
        const PcvReplHostKey *k = g_ptr_array_index(keys, i);
        if (g_str_equal(k->fingerprint, ctx->fingerprint))
            match = k;
    }
    if (!match) {
        g_set_error(error, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_MISMATCH,
                    "HOSTKEY_MISMATCH: no host key offered by '%s' has fingerprint %s — compare on the remote "
                    "console and scan again", ctx->target, ctx->fingerprint);
        return NULL;
    }
    gboolean added = FALSE;
    if (!pcv_repl_hostkey_add(ctx->target, match, &added, error))
        return NULL;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "target_node", ctx->target);
    json_object_set_string_member(res, "type", match->type);
    json_object_set_string_member(res, "fingerprint", match->fingerprint);
    json_object_set_string_member(res, "state", "trusted");
    return res;
}

                                                         
static void
_hk_worker(GTask *task, gpointer src, gpointer data, GCancellable *cancellable)
{
    (void)src; (void)cancellable;
    HostkeyCtx *ctx = data;
    GError *err = NULL;
    GPtrArray *keys = NULL;
    JsonObject *res = NULL;
    if (pcv_repl_keyscan(ctx->target, &keys, &err))
        res = ctx->fingerprint ? _hk_trust_result(ctx, keys, &err) : _hk_scan_result(ctx, keys, &err);
    if (keys)
        g_ptr_array_unref(keys);
    if (res)
        g_task_return_pointer(task, res, (GDestroyNotify)json_object_unref);
    else
        g_task_return_error(task, err);
}

                                                                     
static void
_hk_done(GObject *src, GAsyncResult *result, gpointer data)
{
    (void)src;
    HostkeyCtx *ctx = data;
    GError *err = NULL;
    JsonObject *res = g_task_propagate_pointer(G_TASK(result), &err);
    gint code = res ? 0 : pcv_repl_hostkey_rpc_code(err);
    if (res)
        _hk_send_result(ctx->server, ctx->connection, ctx->rpc_id, res);
    else
        _hk_send_error(ctx->server, ctx->connection, ctx->rpc_id, code,
                       err ? err->message : "host key operation failed");
    if (ctx->fingerprint)
        pcv_audit_log(pcv_drain_task_actor(result), "backup.replication.hostkey.trust", ctx->target, res ? "ok" : "fail", code, 0, "local");
    else
        pcv_audit_log(pcv_drain_task_actor(result), "backup.replication.hostkey.scan", ctx->target, res ? "ok" : "fail", code, 0, "local");
    g_clear_error(&err);
}

                                                                    
static void
_hk_start(JsonObject *params, const gchar *rpc_id, UdsServer *server,
          GSocketConnection *connection, gboolean trust)
{
    const gchar *target = params ? json_object_get_string_member_with_default(params, "target_node", NULL) : NULL;
    const gchar *fp = params ? json_object_get_string_member_with_default(params, "fingerprint", NULL) : NULL;
    gchar *canon = pcv_repl_canonical_target(target);
    gint code = 0;
    gchar *msg = NULL;
    if (!canon) {
        code = PURE_RPC_ERR_INVALID_PARAMS;
        msg = g_strdup("Invalid param: target_node (IPv4 literal or RFC1123 hostname)");
    } else if (trust && !pcv_repl_fingerprint_valid(fp)) {
        code = PURE_RPC_ERR_INVALID_PARAMS;
        msg = g_strdup("Invalid param: fingerprint (expected SHA256:<43 base64 characters>)");
    } else if (trust) {
                                                                                  
        GError *e = NULL;
        PcvReplHostKey *pinned = pcv_repl_hostkey_lookup(canon, &e);
        if (!e && pinned && !g_str_equal(pinned->fingerprint, fp))
            g_set_error(&e, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_CONFLICT,
                        "a different host key (%s %s) is already trusted for '%s' — run "
                        "'pcvctl backup hostkey remove %s' first", pinned->type, pinned->fingerprint, canon, canon);
        if (e) {
            code = pcv_repl_hostkey_rpc_code(e);
            msg = g_strdup(e->message);
            g_error_free(e);
        }
        pcv_repl_host_key_free(pinned);
    }
    if (code) {
        _hk_send_error(server, connection, rpc_id, code, msg);
        if (trust)
            pcv_audit_log(pcv_rpc_completion_actor(), "backup.replication.hostkey.trust", canon ? canon : "", "fail", code, 0, "local");
        else
            pcv_audit_log(pcv_rpc_completion_actor(), "backup.replication.hostkey.scan", canon ? canon : "", "fail", code, 0, "local");
        g_free(msg);
        g_free(canon);
        return;
    }
    HostkeyCtx *ctx = g_new0(HostkeyCtx, 1);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->target = canon;
    ctx->fingerprint = trust ? g_strdup(fp) : NULL;
    GTask *task = pcv_drain_task_new(NULL, NULL, _hk_done, ctx);
    g_task_set_task_data(task, ctx, _hk_ctx_free);
    g_task_run_in_thread(task, _hk_worker);
    g_object_unref(task);
}

                                                          
void
handle_backup_replication_hostkey_scan(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    _hk_start(params, rpc_id, server, connection, FALSE);
}

void
handle_backup_replication_hostkey_trust(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    _hk_start(params, rpc_id, server, connection, TRUE);
}

                                                                            
                                       
void
handle_backup_replication_hostkey_list(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    GError *err = NULL;
    GPtrArray *hosts = pcv_repl_hostkey_list(&err);
    if (!hosts) {
        _hk_send_error(server, connection, rpc_id, pcv_repl_hostkey_rpc_code(err), err->message);
        g_error_free(err);
        return;
    }
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < hosts->len; i++) {
        const PcvReplHostKey *k = g_ptr_array_index(hosts, i);
        JsonObject *o = _hk_key_json(k);
        json_object_set_string_member(o, "target_node", k->target);
        json_array_add_object_element(arr, o);
    }
    g_ptr_array_unref(hosts);
    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "hosts", arr);
    _hk_send_result(server, connection, rpc_id, res);
}

                                                                              
                                 
void
handle_backup_replication_hostkey_remove(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *target = params ? json_object_get_string_member_with_default(params, "target_node", NULL) : NULL;
    GError *err = NULL;
    if (!pcv_repl_hostkey_remove(target, &err)) {
        _hk_send_error(server, connection, rpc_id, pcv_repl_hostkey_rpc_code(err), err->message);
        g_error_free(err);
        return;
    }
    gchar *canon = pcv_repl_canonical_target(target);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "target_node", canon);
    json_object_set_boolean_member(res, "removed", TRUE);
    g_free(canon);
    _hk_send_result(server, connection, rpc_id, res);
}
