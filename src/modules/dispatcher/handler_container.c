













































#include "handler_container.h"
#include "modules/lxc/lxc_driver.h"
#include "rpc_utils.h"
#include "purecvisor/pcv_validate.h"
#include "modules/core/vm_state.h"
#include "modules/audit/pcv_audit.h"
#include "api/uds_server.h"
#include "api/ws_server.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include "utils/pcv_spawn.h"




















typedef struct {
    gchar            *name;
    gchar            *rpc_id;
    UdsServer        *server;
    GSocketConnection *conn;

    gchar            *str_param;
    gboolean          bool_param;
} ContainerCtx;







static ContainerCtx *
_ctx_new(const gchar *name, const gchar *rpc_id,
         UdsServer *server, GSocketConnection *conn)
{
    ContainerCtx *ctx = g_new0(ContainerCtx, 1);
    ctx->name   = g_strdup(name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->conn   = g_object_ref(conn);
    return ctx;
}







static void
_ctx_free(ContainerCtx *ctx)
{
    if (!ctx) return;
    g_free(ctx->name);
    g_free(ctx->rpc_id);
    g_free(ctx->str_param);
    g_object_unref(ctx->server);
    g_object_unref(ctx->conn);
    g_free(ctx);
}











static gboolean
_ensure_container_config_ready(const gchar *name,
                               gchar      **out_config_path,
                               GError     **error)
{
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
    if (g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        if (out_config_path) *out_config_path = config_path;
        else g_free(config_path);
        return TRUE;
    }

    gchar *dataset = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
    const gchar *mount_argv[] = { "zfs", "mount", dataset, NULL };
    GError *mount_err = NULL;

    if (!pcv_spawn_sync(mount_argv, NULL, NULL, &mount_err) && mount_err) {

        if (!g_strrstr(mount_err->message, "already mounted") &&
            !g_file_test(config_path, G_FILE_TEST_EXISTS)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to mount container dataset for '%s': %s",
                        name, mount_err->message);
            g_error_free(mount_err);
            g_free(dataset);
            g_free(config_path);
            return FALSE;
        }
        g_error_free(mount_err);
    }
    g_free(dataset);

    if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' config is not visible under %s",
                    name, config_path);
        g_free(config_path);
        return FALSE;
    }

    if (out_config_path) *out_config_path = config_path;
    else g_free(config_path);
    return TRUE;
}








static void
_send_ok(ContainerCtx *ctx)
{
    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
}







static void
_send_error(ContainerCtx *ctx, PureRpcErrorCode code, const gchar *msg)
{
    gchar *resp = pure_rpc_build_error_response(ctx->rpc_id, code, msg);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
}












typedef struct {
    ContainerCtx *base;
    gchar        *image;
    guint         memory_mb;
    guint         vcpu_count;
    gchar        *bridge;
    gint          rootless;
} CreateCtx;

static void
_create_ctx_free(CreateCtx *c)
{
    _ctx_free(c->base);
    g_free(c->image);
    g_free(c->bridge);
    g_free(c);
}












static void
_on_create_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    CreateCtx *ctx = (CreateCtx *)user_data;
    GError    *error = NULL;





    unlock_vm_operation(ctx->base->name);
    gchar *job_id = g_strdup_printf("container.create:%s", ctx->base->name);
    if (!pcv_lxc_create_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown error";

        g_warning("container.create failed for '%s': %s",
                  ctx->base->name, err_msg);
        pcv_audit_log(NULL, "container.create", ctx->base->name, "fail",
                      -32000, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.create",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.create succeeded for '%s'", ctx->base->name);
        pcv_audit_log(NULL, "container.create", ctx->base->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.create",
                                      "completed", NULL);
    }
    g_free(job_id);
    _create_ctx_free(ctx);
}

void
handle_container_create(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); return;
    }

    const gchar *name   = json_object_get_string_member(params, "name");
    const gchar *image  = json_object_has_member(params, "image")
                          ? json_object_get_string_member(params, "image")
                          : "ubuntu:22.04";
    guint memory_mb   = json_object_has_member(params, "memory_mb")
                        ? (guint)json_object_get_int_member(params, "memory_mb") : 512;
    guint vcpu_count  = json_object_has_member(params, "vcpu_count")
                        ? (guint)json_object_get_int_member(params, "vcpu_count") : 1;
    const gchar *bridge = json_object_has_member(params, "network_bridge")
                          ? json_object_get_string_member(params, "network_bridge")
                          : NULL;













    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_container_image(image)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid image: use 'distro:release' format (e.g. ubuntu:22.04)");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (bridge && !pcv_validate_bridge_name(bridge)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid network_bridge: 1-16 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }










    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_CREATING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }


    gint rootless = -1;
    if (json_object_has_member(params, "rootless")) {
        rootless = json_object_get_boolean_member(params, "rootless") ? 1 : 0;
    }



    {
        JsonObject *accepted = json_object_new();
        json_object_set_string_member(accepted, "status", "accepted");
        json_object_set_string_member(accepted, "name", name);
        json_object_set_string_member(accepted, "message", "Container creation started");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, accepted);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }


    CreateCtx *ctx  = g_new0(CreateCtx, 1);
    ctx->base       = _ctx_new(name, rpc_id, server, conn);
    ctx->image      = g_strdup(image);
    ctx->memory_mb  = memory_mb;
    ctx->vcpu_count = vcpu_count;
    ctx->bridge     = g_strdup(bridge ? bridge : PCV_LXC_DEFAULT_BRIDGE);
    ctx->rootless   = rootless;


    pcv_lxc_create_async_full(name, ctx->image, memory_mb, vcpu_count,
                               ctx->bridge, rootless,
                               NULL, _on_create_done, ctx);
}



















static void
_on_destroy_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    unlock_vm_operation(ctx->name);
    gchar *job_id = g_strdup_printf("container.destroy:%s", ctx->name);
    if (!pcv_lxc_destroy_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown";
        g_warning("container.destroy failed for '%s': %s",
                  ctx->name, err_msg);
        pcv_audit_log(NULL, "container.destroy", ctx->name, "fail",
                      -32000, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.destroy",
                                      "failed", err_msg);
        if (error) g_error_free(error);
    } else {
        g_info("container.destroy succeeded for '%s'", ctx->name);
        pcv_audit_log(NULL, "container.destroy", ctx->name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete(job_id, "container.destroy",
                                      "completed", NULL);
    }
    g_free(job_id);
    _ctx_free(ctx);
}

void
handle_container_destroy(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_DELETING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }


    {
        JsonObject *accepted = json_object_new();
        json_object_set_string_member(accepted, "status", "accepted");
        json_object_set_string_member(accepted, "name", name);
        json_object_set_string_member(accepted, "message", "Container deletion started");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, accepted);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_destroy_async(name, NULL, _on_destroy_done, ctx);
}

















static void
_on_start_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    unlock_vm_operation(ctx->name);
    if (!pcv_lxc_start_finish(res, &error)) {
        g_warning("container.start failed for '%s': %s",
                  ctx->name, error ? error->message : "unknown");
        if (error) g_error_free(error);
    } else {
        g_info("container.start succeeded for '%s'", ctx->name);
    }
    _ctx_free(ctx);
}

void
handle_container_start(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_STARTING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }




    {
        JsonObject *ok = json_object_new();
        json_object_set_boolean_member(ok, "success", TRUE);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_start_async(name, NULL, _on_start_done, ctx);
}


















static void
_on_stop_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    unlock_vm_operation(ctx->name);
    if (!pcv_lxc_stop_finish(res, &error)) {
        g_warning("container.stop failed for '%s': %s",
                  ctx->name, error ? error->message : "unknown");
        if (error) g_error_free(error);
    } else {
        g_info("container.stop succeeded for '%s'", ctx->name);
    }
    _ctx_free(ctx);
}

void
handle_container_stop(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name  = json_object_get_string_member(params, "name");
    gboolean     force = json_object_has_member(params, "force")
                         ? json_object_get_boolean_member(params, "force")
                         : FALSE;

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_STOPPING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }



    {
        JsonObject *ok = json_object_new();
        json_object_set_boolean_member(ok, "success", TRUE);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->bool_param    = force;
    pcv_lxc_stop_async(name, force, NULL, _on_stop_done, ctx);
}















void
handle_container_list(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    GError    *error = NULL;
    GPtrArray *list  = pcv_lxc_list(&error);

    if (!list) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       error ? error->message : "container.list failed");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < list->len; i++) {
        PcvLxcInfo *info = g_ptr_array_index(list, i);
        JsonObject *obj  = json_object_new();
        json_object_set_string_member(obj, "name",      info->name);
        json_object_set_string_member(obj, "state",     info->state_str);
        json_object_set_string_member(obj, "ip_addr",   info->ip_addr);
        json_object_set_string_member(obj, "image",     info->image);
        json_array_add_object_element(arr, obj);
    }
    g_ptr_array_unref(list);


    gint pg_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    gint pg_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;

    if (pg_limit > 0) {


        if (pg_limit > 10000) pg_limit = 10000;
        gint total = (gint)json_array_get_length(arr);
        if (pg_offset < 0) pg_offset = 0;
        if (pg_offset > total) pg_offset = total;
        JsonArray *paged = json_array_new();
        for (gint i = pg_offset; i < total && i < pg_offset + pg_limit; i++)
            json_array_add_element(paged, json_array_dup_element(arr, (guint)i));
        JsonObject *pg = json_object_new();
        json_object_set_array_member(pg, "items", paged);
        json_object_set_int_member(pg, "total", total);
        json_object_set_int_member(pg, "offset", pg_offset);
        json_object_set_int_member(pg, "limit", pg_limit);
        json_object_set_boolean_member(pg, "has_more", pg_offset + pg_limit < total);
        json_array_unref(arr);
        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, pg);
        gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    } else {
        JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(result_node, arr);
        gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
    }
}

















void
handle_container_metrics(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    GError *error = NULL;
    PcvLxcMetrics *m = pcv_lxc_get_metrics(name, &error);
    if (!m) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       error ? error->message : "metrics unavailable");
        pure_uds_server_send_response(server, conn, e); g_free(e);
        if (error) g_error_free(error);
        return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name",         m->name);
    json_object_set_string_member(obj, "state",        m->state_str);
    json_object_set_double_member(obj, "mem_used_mb",
        (gdouble)m->mem_used_bytes  / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "mem_limit_mb",
        (gdouble)m->mem_limit_bytes / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "cpu_percent",  m->cpu_percent);
    json_object_set_double_member(obj, "net_rx_mb",
        (gdouble)m->net_rx_bytes / (1024.0 * 1024.0));
    json_object_set_double_member(obj, "net_tx_mb",
        (gdouble)m->net_tx_bytes / (1024.0 * 1024.0));
    json_object_set_string_member(obj, "ip_addr",      m->ip_addr);
    json_object_set_int_member   (obj, "init_pid",     (gint64)m->init_pid);
    pcv_lxc_metrics_free(m);

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}













static void
_on_exec_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;


    gchar *output = pcv_lxc_exec_finish(res, &error);
    if (!output) {
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "container.exec failed");
        if (error) g_error_free(error);
        _ctx_free(ctx); return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "output", output);
    g_free(output);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
    _ctx_free(ctx);
}
















void
handle_container_exec(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "cmd")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'cmd'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *cmd  = json_object_get_string_member(params, "cmd");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_exec_cmd(cmd)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid cmd: must be 1-1024 chars with no null bytes");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    const gchar *argv[] = { "/bin/sh", "-c", cmd, NULL };

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_exec_async(name, argv, NULL, _on_exec_done, ctx);
}














static void
_on_snap_create_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                     gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    if (!pcv_lxc_snapshot_create_finish(res, &error))
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.create failed");
    else
        _send_ok(ctx);
    if (error) g_error_free(error);
    _ctx_free(ctx);
}

void
handle_container_snapshot_create(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    pcv_lxc_snapshot_create_async(name, snap_name, NULL, _on_snap_create_done, ctx);
}













static void
_on_snap_rollback_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                       gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    if (!pcv_lxc_snapshot_rollback_finish(res, &error))
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.rollback failed");
    else
        _send_ok(ctx);
    if (error) g_error_free(error);
    _ctx_free(ctx);
}

void
handle_container_snapshot_rollback(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    pcv_lxc_snapshot_rollback_async(name, snap_name, NULL, _on_snap_rollback_done, ctx);
}











static void
_on_snap_delete_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                     gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;
    if (!pcv_lxc_snapshot_delete_finish(res, &error))
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.delete failed");
    else
        _send_ok(ctx);
    if (error) g_error_free(error);
    _ctx_free(ctx);
}

void
handle_container_snapshot_delete(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "snap_name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name' and/or 'snap_name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar  *name      = json_object_get_string_member(params, "name");
    const gchar  *snap_name = json_object_get_string_member(params, "snap_name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (!pcv_validate_snap_name(snap_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid snap_name: 1-128 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx  = _ctx_new(name, rpc_id, server, conn);
    ctx->str_param     = g_strdup(snap_name);
    pcv_lxc_snapshot_delete_async(name, snap_name, NULL, _on_snap_delete_done, ctx);
}















static void
_on_snap_list_done(GObject *src __attribute__((unused)), GAsyncResult *res,
                   gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    GPtrArray *snaps = pcv_lxc_snapshot_list_finish(res, &error);
    if (!snaps) {
        _send_error(ctx, PURE_RPC_ERR_INTERNAL_ERROR,
                    error ? error->message : "snapshot.list failed");
        if (error) g_error_free(error);
        _ctx_free(ctx); return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < snaps->len; i++)
        json_array_add_string_element(arr, (const gchar *)g_ptr_array_index(snaps, i));
    g_ptr_array_unref(snaps);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
    _ctx_free(ctx);
}

void
handle_container_snapshot_list(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");


    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_snapshot_list_async(name, NULL, _on_snap_list_done, ctx);
}























static GPtrArray *
_tail_file(const gchar *path, gint n_lines, gint *total)
{
    gchar *contents = NULL;
    gsize  len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL))
        return NULL;

    gchar **all_lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    gint count = 0;
    while (all_lines[count]) count++;

    if (count > 0 && all_lines[count - 1][0] == '\0')
        count--;

    if (total) *total = count;

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    gint start = count > n_lines ? count - n_lines : 0;
    for (gint i = start; i < count; i++)
        g_ptr_array_add(result, g_strdup(all_lines[i]));

    g_strfreev(all_lines);
    return result;
}

void
handle_container_logs(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    gint n_lines = json_object_has_member(params, "lines")
                   ? (gint)json_object_get_int_member(params, "lines") : 50;

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (n_lines < 1) n_lines = 1;
    if (n_lines > 10000) n_lines = 10000;


    gchar *path1 = g_strdup_printf("%s/%s/%s.log", PCV_LXC_PATH, name, name);
    gchar *path2 = g_strdup_printf("/var/log/lxc/%s.log", name);

    gint total_lines = 0;
    GPtrArray *lines = _tail_file(path1, n_lines, &total_lines);
    if (!lines)
        lines = _tail_file(path2, n_lines, &total_lines);

    g_free(path1);
    g_free(path2);

    if (!lines) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       "No log file found for container");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < lines->len; i++)
        json_array_add_string_element(arr, (const gchar *)g_ptr_array_index(lines, i));
    g_ptr_array_unref(lines);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name);
    json_object_set_array_member(obj, "lines", arr);
    json_object_set_int_member(obj, "total", total_lines);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}













void
handle_container_volume_attach(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "host_path")
        || !json_object_has_member(params, "container_path")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, host_path, container_path");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name           = json_object_get_string_member(params, "name");
    const gchar *host_path      = json_object_get_string_member(params, "host_path");
    const gchar *container_path = json_object_get_string_member(params, "container_path");
    gboolean     readonly       = json_object_has_member(params, "readonly")
                                  ? json_object_get_boolean_member(params, "readonly") : FALSE;

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    char resolved[PATH_MAX];
    if (!realpath(host_path, resolved)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "host_path does not exist or is invalid");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    if (container_path[0] != '/' || strstr(container_path, "..")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "container_path must be absolute with no '..'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    gchar *state = pcv_lxc_get_state(name);
    if (state && g_strcmp0(state, "RUNNING") == 0) {
        GError *run_err = NULL;
        gchar *target_path = g_strdup_printf("%s/%s/rootfs%s", PCV_LXC_PATH, name, container_path);
        const gchar *mkdir_argv[] = {"mkdir", "-p", target_path, NULL};
        if (!pcv_spawn_sync(mkdir_argv, NULL, NULL, &run_err)) {
            gchar *e = pure_rpc_build_error_response(
                           rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                           run_err ? run_err->message : "container mount target prepare failed");
            pure_uds_server_send_response(server, conn, e);
            g_free(e);
            if (run_err) g_error_free(run_err);
            g_free(target_path);
            g_free(state);
            return;
        }
        const gchar *mount_argv[] = {"mount", "--bind", resolved, target_path, NULL};
        if (!pcv_spawn_sync(mount_argv, NULL, NULL, &run_err)) {
            gchar *e = pure_rpc_build_error_response(
                           rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                           run_err ? run_err->message : "bind mount failed");
            pure_uds_server_send_response(server, conn, e);
            g_free(e);
            if (run_err) g_error_free(run_err);
            g_free(target_path);
            g_free(state);
            return;
        }
        if (readonly) {
            const gchar *ro_argv[] = {"mount", "-o", "remount,ro,bind", target_path, NULL};
            if (!pcv_spawn_sync(ro_argv, NULL, NULL, &run_err)) {
                gchar *e = pure_rpc_build_error_response(
                               rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                               run_err ? run_err->message : "bind remount ro failed");
                pure_uds_server_send_response(server, conn, e);
                g_free(e);
                if (run_err) g_error_free(run_err);
                g_free(target_path);
                g_free(state);
                return;
            }
        }
        g_free(target_path);
    }
    g_free(state);


    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }

    const gchar *dest = container_path[0] == '/' ? container_path + 1 : container_path;
    gchar *entry = g_strdup_printf("lxc.mount.entry = %s %s none bind%s 0 0\n",
                                     resolved, dest, readonly ? ",ro" : "");
    FILE *fp = fopen(config_path, "a");
    if (!fp || fputs(entry, fp) == EOF || fclose(fp) == EOF) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       "Failed to persist container volume config");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (fp) fclose(fp);
        g_free(config_path);
        g_free(entry);
        return;
    }
    g_free(config_path);
    g_free(entry);

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}








void
handle_container_volume_detach(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "container_path")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, container_path");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name           = json_object_get_string_member(params, "name");
    const gchar *container_path = json_object_get_string_member(params, "container_path");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (container_path[0] != '/' || strstr(container_path, "..")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "container_path must be absolute with no '..'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }


    gchar *state = pcv_lxc_get_state(name);
    if (state && g_strcmp0(state, "RUNNING") == 0) {
        gchar *target_path = g_strdup_printf("%s/%s/rootfs%s", PCV_LXC_PATH, name, container_path);
        const gchar *umount_argv[] = {"umount", target_path, NULL};
        pcv_spawn_sync(umount_argv, NULL, NULL, NULL);
        g_free(target_path);
    }
    g_free(state);


    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        const gchar *dest = container_path[0] == '/' ? container_path + 1 : container_path;
        gchar **lines = g_strsplit(contents, "\n", -1);
        GString *out = g_string_new(NULL);
        for (gint i = 0; lines[i]; i++) {

            if (strstr(lines[i], "lxc.mount.entry") && strstr(lines[i], dest))
                continue;
            g_string_append(out, lines[i]);
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_file_set_contents(config_path, out->str, -1, NULL);
        g_string_free(out, TRUE);
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(config_path);

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}








void
handle_container_volume_list(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    JsonArray *arr = json_array_new();

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (gint i = 0; lines[i]; i++) {

            if (!g_str_has_prefix(lines[i], "lxc.mount.entry"))
                continue;
            gchar *eq = strchr(lines[i], '=');
            if (!eq) continue;
            gchar *val = g_strstrip(g_strdup(eq + 1));
            gchar **parts = g_strsplit(val, " ", 6);
            g_free(val);
            if (parts[0] && parts[1] && parts[2] && parts[3]) {

                gboolean ro = strstr(parts[3], "ro") != NULL;
                gchar *cpath = g_strdup_printf("/%s", parts[1]);
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "host_path", parts[0]);
                json_object_set_string_member(obj, "container_path", cpath);
                json_object_set_boolean_member(obj, "readonly", ro);
                json_array_add_object_element(arr, obj);
                g_free(cpath);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(config_path);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}










void
handle_container_env_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "key")
        || !json_object_has_member(params, "value")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, key, value");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name  = json_object_get_string_member(params, "name");
    const gchar *key   = json_object_get_string_member(params, "key");
    const gchar *value = json_object_get_string_member(params, "value");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    for (const gchar *p = key; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_') {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                           "Invalid env key: alphanumeric and '_' only");
            pure_uds_server_send_response(server, conn, e); g_free(e); return;
        }
    }


    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    gchar *search_prefix = g_strdup_printf("lxc.environment = %s=", key);

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        GString *out = g_string_new(NULL);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));
            if (g_str_has_prefix(trimmed, search_prefix)) {
                g_free(trimmed);
                continue;
            }
            g_free(trimmed);
            g_string_append(out, lines[i]);
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_strfreev(lines);
        g_free(contents);
        contents = g_string_free(out, FALSE);
    }


    gchar *entry = g_strdup_printf("lxc.environment = %s=%s\n", key, value);
    if (contents) {
        gchar *full = g_strconcat(contents, entry, NULL);
        if (!g_file_set_contents(config_path, full, -1, NULL)) {
            gchar *e = pure_rpc_build_error_response(
                           rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                           "Failed to persist container environment");
            pure_uds_server_send_response(server, conn, e);
            g_free(e);
            g_free(full);
            g_free(contents);
            g_free(entry);
            g_free(search_prefix);
            g_free(config_path);
            return;
        }
        g_free(full);
        g_free(contents);
    } else {
        if (!g_file_set_contents(config_path, entry, -1, NULL)) {
            gchar *e = pure_rpc_build_error_response(
                           rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                           "Failed to persist container environment");
            pure_uds_server_send_response(server, conn, e);
            g_free(e);
            g_free(entry);
            g_free(search_prefix);
            g_free(config_path);
            return;
        }
    }

    g_free(entry);
    g_free(search_prefix);
    g_free(config_path);

    JsonObject *res = json_object_new();
    json_object_set_boolean_member(res, "success", TRUE);
    json_object_set_string_member(res, "note", "restart required");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}








void
handle_container_env_list(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *conn)
{
    if (!params || !json_object_has_member(params, "name")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing parameter: 'name'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    JsonObject *env_obj = json_object_new();

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));
            if (!g_str_has_prefix(trimmed, "lxc.environment")) {
                g_free(trimmed); continue;
            }
            gchar *eq = strchr(trimmed, '=');
            if (!eq) { g_free(trimmed); continue; }

            gchar *kv = g_strstrip(g_strdup(eq + 1));
            gchar *sep = strchr(kv, '=');
            if (sep) {
                *sep = '\0';
                json_object_set_string_member(env_obj, kv, sep + 1);
            }
            g_free(kv);
            g_free(trimmed);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(config_path);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, env_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}








void
handle_container_env_delete(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *conn)
{
    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "key")) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing required: name, key");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *key  = json_object_get_string_member(params, "key");

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    gchar *config_path = NULL;
    GError *cfg_err = NULL;
    if (!_ensure_container_config_ready(name, &config_path, &cfg_err)) {
        gchar *e = pure_rpc_build_error_response(
                       rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       cfg_err ? cfg_err->message : "container config unavailable");
        pure_uds_server_send_response(server, conn, e);
        g_free(e);
        if (cfg_err) g_error_free(cfg_err);
        return;
    }
    gchar *contents = NULL;
    gchar *search_prefix = g_strdup_printf("lxc.environment = %s=", key);

    if (g_file_get_contents(config_path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        GString *out = g_string_new(NULL);
        for (gint i = 0; lines[i]; i++) {
            gchar *trimmed = g_strstrip(g_strdup(lines[i]));
            if (g_str_has_prefix(trimmed, search_prefix)) {
                g_free(trimmed);
                continue;
            }
            g_free(trimmed);
            g_string_append(out, lines[i]);
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_file_set_contents(config_path, out->str, -1, NULL);
        g_string_free(out, TRUE);
        g_strfreev(lines);
        g_free(contents);
    }

    g_free(search_prefix);
    g_free(config_path);

    JsonObject *res_env = json_object_new();
    json_object_set_boolean_member(res_env, "success", TRUE);
    json_object_set_string_member(res_env, "note", "restart required");
    JsonNode *node_env = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node_env, res_env);
    gchar *resp_env = pure_rpc_build_success_response(rpc_id, node_env);
    pure_uds_server_send_response(server, conn, resp_env);
    g_free(resp_env);
}
















#define MAX_HEALTH_PROBES 32

typedef struct {
    gchar    name[64];
    gchar    probe_type[8];
    gchar    target[256];
    gint     timeout_sec;
    gint     interval_sec;
    gint     failure_threshold;
    gboolean auto_restart;

    gint     consecutive_failures;
    gboolean healthy;
    gint     restart_count;
    gint64   last_check_time;
} ContainerHealthProbe;

static ContainerHealthProbe g_health_probes[MAX_HEALTH_PROBES];
static gint g_n_health_probes = 0;
static GMutex g_health_mu;
static guint g_health_timer_id = 0;


static gint _health_find(const gchar *ctr_name) {
    for (gint i = 0; i < g_n_health_probes; i++) {
        if (g_strcmp0(g_health_probes[i].name, ctr_name) == 0)
            return i;
    }
    return -1;
}


static gboolean _health_check_tick(gpointer user_data) {
    (void)user_data;
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&g_health_mu);
    for (gint i = 0; i < g_n_health_probes; i++) {
        ContainerHealthProbe *p = &g_health_probes[i];
        if ((now - p->last_check_time) < (gint64)p->interval_sec * G_USEC_PER_SEC)
            continue;
        p->last_check_time = now;


        const gchar *pid_argv[] = {"lxc-info", "-P", PCV_LXC_PATH,
                                    "-n", p->name, "-p", "-H", NULL};
        gchar *pid_out = NULL;
        gboolean ok = FALSE;

        if (!pcv_spawn_sync(pid_argv, &pid_out, NULL, NULL) || !pid_out ||
            !g_strstrip(pid_out)[0]) {
            g_free(pid_out);
            p->consecutive_failures++;
            goto hc_check_threshold;
        }

        if (g_strcmp0(p->probe_type, "tcp") == 0) {
            const gchar *argv[] = {"nsenter", "-t", pid_out, "-n", "--",
                                    "nc", "-z", "-w", "1", "localhost", p->target, NULL};
            ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
        } else if (g_strcmp0(p->probe_type, "http") == 0) {
            gchar tmo[16];
            g_snprintf(tmo, sizeof(tmo), "%d", p->timeout_sec);
            const gchar *argv[] = {"nsenter", "-t", pid_out, "-n", "--",
                                    "curl", "-sf", "--max-time", tmo, p->target, NULL};
            ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
        } else if (g_strcmp0(p->probe_type, "exec") == 0) {
            const gchar *argv[] = {"lxc-attach", "-P", PCV_LXC_PATH,
                                    "-n", p->name, "--", p->target, NULL};
            ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
        }
        g_free(pid_out);

        if (ok) {
            p->consecutive_failures = 0;
            p->healthy = TRUE;
            continue;
        }
        p->consecutive_failures++;

hc_check_threshold:
        if (p->consecutive_failures >= p->failure_threshold) {
            p->healthy = FALSE;
            if (p->auto_restart) {
                g_message("[HealthCheck] %s unhealthy (%d failures), restarting",
                          p->name, p->consecutive_failures);
                const gchar *stop_argv[]  = {"lxc-stop", "-P", PCV_LXC_PATH,
                                              "-n", p->name, NULL};
                const gchar *start_argv[] = {"lxc-start", "-P", PCV_LXC_PATH,
                                              "-n", p->name, NULL};
                pcv_spawn_sync(stop_argv, NULL, NULL, NULL);
                pcv_spawn_sync(start_argv, NULL, NULL, NULL);
                p->restart_count++;
                p->consecutive_failures = 0;
            }
        }
    }
    g_mutex_unlock(&g_health_mu);
    return G_SOURCE_CONTINUE;
}


void handle_container_health_set(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *type = json_object_has_member(params, "type")
        ? json_object_get_string_member(params, "type") : NULL;
    const gchar *target = json_object_has_member(params, "target")
        ? json_object_get_string_member(params, "target") : NULL;

    if (!cname || !type || !target) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "Required: name, type (tcp/http/exec), target");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (g_strcmp0(type, "tcp") != 0 && g_strcmp0(type, "http") != 0 &&
        g_strcmp0(type, "exec") != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "type must be tcp, http, or exec");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(cname);
    if (idx < 0) {
        if (g_n_health_probes >= MAX_HEALTH_PROBES) {
            g_mutex_unlock(&g_health_mu);
            gchar *e = pure_rpc_build_error_response(rpc_id, -32000,
                           "Max health probes reached (32)");
            pure_uds_server_send_response(server, conn, e); g_free(e); return;
        }
        idx = g_n_health_probes++;
    }
    ContainerHealthProbe *p = &g_health_probes[idx];
    g_strlcpy(p->name, cname, sizeof(p->name));
    g_strlcpy(p->probe_type, type, sizeof(p->probe_type));
    g_strlcpy(p->target, target, sizeof(p->target));
    p->timeout_sec = json_object_has_member(params, "timeout_sec")
        ? (gint)json_object_get_int_member(params, "timeout_sec") : 5;
    p->interval_sec = json_object_has_member(params, "interval_sec")
        ? (gint)json_object_get_int_member(params, "interval_sec") : 30;
    if (p->interval_sec < 5) p->interval_sec = 5;
    p->failure_threshold = json_object_has_member(params, "failure_threshold")
        ? (gint)json_object_get_int_member(params, "failure_threshold") : 3;
    p->auto_restart = json_object_has_member(params, "auto_restart")
        ? json_object_get_boolean_member(params, "auto_restart") : FALSE;
    p->consecutive_failures = 0;
    p->healthy = TRUE;
    p->last_check_time = g_get_monotonic_time();

    if (g_health_timer_id == 0)
        g_health_timer_id = g_timeout_add_seconds(1, _health_check_tick, NULL);

    g_mutex_unlock(&g_health_mu);

    JsonObject *r = json_object_new();
    json_object_set_boolean_member(r, "success", TRUE);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, r);
    gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, conn, rsp);
    g_free(rsp);
}


void handle_container_health_get(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    g_mutex_lock(&g_health_mu);

    if (cname) {
        gint idx = _health_find(cname);
        if (idx < 0) {
            g_mutex_unlock(&g_health_mu);
            gchar *e = pure_rpc_build_error_response(rpc_id, -32000,
                           "No health probe for this container");
            pure_uds_server_send_response(server, conn, e); g_free(e); return;
        }
        ContainerHealthProbe *p = &g_health_probes[idx];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", p->name);
        json_object_set_string_member(obj, "type", p->probe_type);
        json_object_set_string_member(obj, "target", p->target);
        json_object_set_int_member(obj, "interval_sec", p->interval_sec);
        json_object_set_int_member(obj, "failure_threshold", p->failure_threshold);
        json_object_set_boolean_member(obj, "healthy", p->healthy);
        json_object_set_int_member(obj, "consecutive_failures", p->consecutive_failures);
        json_object_set_int_member(obj, "restart_count", p->restart_count);
        json_object_set_boolean_member(obj, "auto_restart", p->auto_restart);
        g_mutex_unlock(&g_health_mu);

        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, obj);
        gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
        pure_uds_server_send_response(server, conn, rsp);
        g_free(rsp);
    } else {
        JsonArray *arr = json_array_new();
        for (gint i = 0; i < g_n_health_probes; i++) {
            ContainerHealthProbe *p = &g_health_probes[i];
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "name", p->name);
            json_object_set_string_member(obj, "type", p->probe_type);
            json_object_set_boolean_member(obj, "healthy", p->healthy);
            json_object_set_int_member(obj, "consecutive_failures", p->consecutive_failures);
            json_object_set_int_member(obj, "restart_count", p->restart_count);
            json_array_add_object_element(arr, obj);
        }
        g_mutex_unlock(&g_health_mu);

        JsonNode *n = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(n, arr);
        gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
        pure_uds_server_send_response(server, conn, rsp);
        g_free(rsp);
    }
}


void handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!cname) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602, "Required: name");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(cname);
    if (idx < 0) {
        g_mutex_unlock(&g_health_mu);
        JsonObject *r = json_object_new();
        json_object_set_boolean_member(r, "success", TRUE);
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, r);
        gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
        pure_uds_server_send_response(server, conn, rsp);
        g_free(rsp);
        return;
    }
    if (idx < g_n_health_probes - 1)
        g_health_probes[idx] = g_health_probes[g_n_health_probes - 1];
    g_n_health_probes--;
    g_mutex_unlock(&g_health_mu);

    JsonObject *r = json_object_new();
    json_object_set_boolean_member(r, "success", TRUE);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, r);
    gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, conn, rsp);
    g_free(rsp);
}
