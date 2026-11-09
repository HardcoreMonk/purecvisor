                                                               
                           
                                                         
                                                               
                                                                 
                                                        
                                                                    
   
#include "handler_vm_inspect.h"
#include "rpc_utils.h"
#include "modules/dispatcher/rpc_completion.h"
#include "../virt/vm_inspection.h"
#include "../virt/virt_conn_pool.h"
#include "../../api/drain.h"
#include "../audit/pcv_audit.h"

#ifndef PCV_INSPECTION_TIMEOUT_MS
#define PCV_INSPECTION_TIMEOUT_MS 30000
#endif

typedef struct {
    gchar *uuid;
    gchar *rpc_id;
    gchar *subject;
    UdsServer *server;
    GSocketConnection *connection;
    GSource *timer;
    gint64 deadline;
    gint64 started;
} InspectRequest;

                                                                 
static void request_free(gpointer data)
{
    InspectRequest *ctx = data;
    if (ctx->timer) {
        g_source_destroy(ctx->timer);
        g_source_unref(ctx->timer);
    }
    g_free(ctx->uuid);
    g_free(ctx->rpc_id);
    g_free(ctx->subject);
    g_object_unref(ctx->server);
    g_object_unref(ctx->connection);
    g_free(ctx);
}

static gboolean expire_request(gpointer data)
{
    g_cancellable_cancel(G_CANCELLABLE(data));
    return G_SOURCE_REMOVE;
}

                                          
                                                       
static const gchar *request_subject(JsonObject *params, GSocketConnection *connection)
{
    (void)params; (void)connection;                                                                     
    const gchar *actor = pcv_rpc_completion_actor();
    return actor ? actor : PCV_AUDIT_ACTOR_LOCAL;
}

                                                                                         
                                                                         
                                                              
static gboolean request_members_valid(JsonObject *params)
{
    if (!params) return FALSE;
    GList *members = json_object_get_members(params);
    gboolean valid = TRUE;
    for (GList *item = members; item && valid; item = item->next) {
        const gchar *key = item->data;
        JsonNode *node = json_object_get_member(params, key);
        if (g_strcmp0(key, "uuid") == 0) continue;
        if (g_strcmp0(key, "_pcv_caller_sub") == 0)
            valid = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING;
        else if (g_strcmp0(key, "_pcv_caller_role") == 0)
            valid = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64;
        else if (g_strcmp0(key, "_pcv_caller_tenant") == 0)
            valid = JSON_NODE_HOLDS_NULL(node) ||
                    (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING);
        else valid = FALSE;
    }
    g_list_free(members);
    return valid;
}

                                                                
static void inspect_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
    (void)object;
    InspectRequest *ctx = data;
    GError *error = NULL;
    virConnectPtr conn = NULL;
    if (!g_cancellable_is_cancelled(cancel) && g_get_monotonic_time() < ctx->deadline)
        conn = virt_conn_pool_acquire();
    JsonNode *result = pcv_vm_inspection_collect(conn, ctx->uuid, cancel, ctx->deadline, &error);
    if (conn) virt_conn_pool_release(conn);
    if (error) g_task_return_error(task, error);
    else g_task_return_pointer(task, result, (GDestroyNotify)json_node_unref);
}

                                                                          
static void inspect_complete(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    InspectRequest *ctx = data;
    g_source_destroy(ctx->timer);
    g_source_unref(ctx->timer);
    ctx->timer = NULL;
    GError *error = NULL;
    JsonNode *node = g_task_propagate_pointer(G_TASK(result), &error);
    gchar *response;
    gint audit_code = 0;
    if (error) {
        gboolean timeout = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        audit_code = timeout ? PURE_RPC_ERR_TIMEOUT : PURE_RPC_ERR_INTERNAL_ERROR;
        response = pure_rpc_build_error_response(ctx->rpc_id,
            audit_code,
            timeout ? "inspection timed out" : "inspection failed");
        g_error_free(error);
    } else {
        JsonObject *report = json_node_get_object(node);
        if (g_strcmp0(json_object_get_string_member(report, "outcome"), "unavailable") == 0) {
            audit_code = g_strcmp0(json_object_get_string_member(report, "failure"), "TIMEOUT") == 0
                ? PURE_RPC_ERR_TIMEOUT : PURE_RPC_ERR_INTERNAL_ERROR;
        }
        response = pure_rpc_build_success_response(ctx->rpc_id, node);
    }
                                                                      
                                                  
    pcv_audit_log(ctx->subject, "vm.inspect", ctx->uuid, audit_code ? "fail" : "ok",
                  audit_code, (g_get_monotonic_time() - ctx->started) / 1000, "local");
    pure_uds_server_send_response(ctx->server, ctx->connection, response);
    g_free(response);
}

void handle_vm_inspect_request(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    JsonNode *id = params ? json_object_get_member(params, "uuid") : NULL;
    const gchar *uuid = id && JSON_NODE_HOLDS_VALUE(id) &&
        json_node_get_value_type(id) == G_TYPE_STRING ? json_node_get_string(id) : NULL;
    if (!request_members_valid(params) || !pcv_vm_inspection_uuid_valid(uuid)) {
        gchar *error = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                                                    "canonical UUID required");
        pure_uds_server_send_response(server, connection, error);
                                                              
        pcv_audit_log(request_subject(params, connection), "vm.inspect",
            pcv_vm_inspection_uuid_valid(uuid) ? uuid : "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        g_free(error);
        return;
    }
    InspectRequest *ctx = g_new0(InspectRequest, 1);
    ctx->uuid = g_strdup(uuid);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->subject = g_strdup(request_subject(params, connection));
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->started = g_get_monotonic_time();
    ctx->deadline = ctx->started + PCV_INSPECTION_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
    GCancellable *cancel = g_cancellable_new();
    GTask *task = pcv_drain_task_new(NULL, cancel, inspect_complete, ctx);
    g_task_set_task_data(task, ctx, request_free);
    g_task_set_return_on_cancel(task, TRUE);
    ctx->timer = g_timeout_source_new(PCV_INSPECTION_TIMEOUT_MS);
    g_source_set_callback(ctx->timer, expire_request, g_object_ref(cancel), g_object_unref);
    g_source_attach(ctx->timer, g_task_get_context(task));
    g_task_run_in_thread(task, inspect_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}
