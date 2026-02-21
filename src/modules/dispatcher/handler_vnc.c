/**
 * @file handler_vnc.c
 * @brief VNC 및 WebSocket 연결 정보 조회 디스패처 (Phase 6 규격 최종본)
 */

#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vnc.h"

// =================================================================
// 1. 비동기 컨텍스트 구조체
// =================================================================
typedef struct {
    gchar *vm_id;
    gchar *rpc_id;                  
    UdsServer *server;              
    GSocketConnection *connection;  
    gint vnc_port;
    gint websocket_port;
} VncContext;

static void free_vnc_context(gpointer data) {
    if (!data) return;
    VncContext *ctx = (VncContext *)data;
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

// =================================================================
// 2. 워커 스레드 (Worker Thread): XML 추출 및 파싱
// =================================================================
static void vnc_worker_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    VncContext *ctx = (VncContext *)task_data;
    GError *error = NULL;
    gchar *xml_desc = NULL;
    
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        g_task_return_error(task, error);
        return;
    }

    virDomainPtr dom = virDomainLookupByUUIDString(conn, ctx->vm_id);
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM UUID %s not found.", ctx->vm_id);
        goto cleanup_conn;
    }

    if (virDomainIsActive(dom) != 1) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM is not running. Cannot retrieve VNC ports.");
        goto cleanup_dom;
    }

    xml_desc = virDomainGetXMLDesc(dom, 0);
    if (!xml_desc) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to retrieve VM XML description.");
        goto cleanup_dom;
    }

    GRegex *regex_port = g_regex_new("<graphics\\s+type='vnc'[^>]*\\bport='(\\d+)'", 0, 0, NULL);
    GRegex *regex_ws   = g_regex_new("<graphics\\s+type='vnc'[^>]*\\bwebsocket='(\\d+)'", 0, 0, NULL);
    GMatchInfo *match_info;

    ctx->vnc_port = -1;
    ctx->websocket_port = -1;

    if (g_regex_match(regex_port, xml_desc, 0, &match_info)) {
        gchar *port_str = g_match_info_fetch(match_info, 1);
        ctx->vnc_port = atoi(port_str);
        g_free(port_str);
    }
    g_match_info_free(match_info);

    if (g_regex_match(regex_ws, xml_desc, 0, &match_info)) {
        gchar *ws_str = g_match_info_fetch(match_info, 1);
        ctx->websocket_port = atoi(ws_str);
        g_free(ws_str);
    }
    g_match_info_free(match_info);

    g_regex_unref(regex_port);
    g_regex_unref(regex_ws);

    if (ctx->vnc_port == -1) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VNC graphics configuration not found in VM XML.");
    }

cleanup_dom:
    if (dom) virDomainFree(dom);
cleanup_conn:
    if (conn) virConnectClose(conn);
    if (xml_desc) free(xml_desc); 

    if (error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
}

// =================================================================
// 3. 콜백 (Main Thread): 결과 JSON 포맷팅 및 응답
// =================================================================
static void vnc_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VncContext *ctx = (VncContext *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);

    if (!success) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        // 회원님의 rpc_utils.h 규칙에 맞춰 JsonNode 객체 생성
        JsonObject *data_obj = json_object_new();
        json_object_set_int_member(data_obj, "vnc_port", ctx->vnc_port);
        json_object_set_int_member(data_obj, "websocket_port", ctx->websocket_port);

        JsonNode *data_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(data_node, data_obj);

        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, data_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        
        g_free(succ_resp);
        json_node_free(data_node); 
    }
}

// =================================================================
// 4. 진입점 (Main Thread)
// =================================================================
void handle_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *vm_id = json_object_get_string_member(params, "vm_id");

    VncContext *ctx = g_new0(VncContext, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vnc_callback, ctx);
    g_task_set_task_data(task, ctx, (GDestroyNotify)free_vnc_context);
    
    g_task_run_in_thread(task, vnc_worker_thread);
    g_object_unref(task);
}