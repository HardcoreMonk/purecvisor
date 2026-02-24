// src/modules/dispatcher/handler_snapshot.c

#include "handler_snapshot.h"
#include "rpc_utils.h"
#include "../src/modules/storage/zfs_driver.h"  // ZFS 드라이버 헤더 
#include "../../api/uds_server.h"   // UDS 서버 헤더 (경로에 유의하세요)
#include <string.h>

/* ========================================================================= */
/* 1. 비동기 컨텍스트 구조체 정의 (반드시 함수들보다 최상단에 위치해야 합니다) */
/* ========================================================================= */
typedef struct {
    gchar *rpc_id;
    UdsServer *server;             
    GSocketConnection *connection; 
} RpcAsyncContext;

static void rpc_async_context_free(RpcAsyncContext *ctx) {
    if (ctx) {
        g_free(ctx->rpc_id);
        // [추가된 코드] 비동기 작업이 끝났으므로 레퍼런스 카운트 감소
        if (ctx->server) g_object_unref(ctx->server);           
        if (ctx->connection) g_object_unref(ctx->connection);        
        g_free(ctx);
    }
}

/* 공통 성공 응답 생성 헬퍼 (내부용) */
static JsonNode* create_boolean_success_node(void) {
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "success", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    return node;
}


/* ========================================================================= */
/* 2. 비동기 작업 완료 콜백 함수들 */
/* ========================================================================= */

static void
on_snapshot_create_completed(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    RpcAsyncContext *ctx = (RpcAsyncContext *)user_data;
    GError *error = NULL;
    gchar *response_str = NULL;

    if (!purecvisor_zfs_snapshot_create_finish(res, &error)) {
        response_str = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        g_error_free(error);
    } else {
        response_str = pure_rpc_build_success_response(ctx->rpc_id, create_boolean_success_node());
    }

    pure_uds_server_send_response(ctx->server, ctx->connection, response_str);
    g_free(response_str);
    rpc_async_context_free(ctx);
}

static void
on_snapshot_rollback_completed(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    RpcAsyncContext *ctx = (RpcAsyncContext *)user_data;
    GError *error = NULL;
    gchar *response_str = NULL;

    if (!purecvisor_zfs_snapshot_rollback_finish(res, &error)) {
        response_str = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        g_error_free(error);
    } else {
        response_str = pure_rpc_build_success_response(ctx->rpc_id, create_boolean_success_node());
    }

    pure_uds_server_send_response(ctx->server, ctx->connection, response_str);
    g_free(response_str);
    rpc_async_context_free(ctx);
}

static void
on_snapshot_delete_completed(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    RpcAsyncContext *ctx = (RpcAsyncContext *)user_data;
    GError *error = NULL;
    gchar *response_str = NULL;

    if (!purecvisor_zfs_snapshot_delete_finish(res, &error)) {
        response_str = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        g_error_free(error);
    } else {
        response_str = pure_rpc_build_success_response(ctx->rpc_id, create_boolean_success_node());
    }

    pure_uds_server_send_response(ctx->server, ctx->connection, response_str);
    g_free(response_str);
    rpc_async_context_free(ctx);
}

static void
on_snapshot_list_completed(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    RpcAsyncContext *ctx = (RpcAsyncContext *)user_data;
    GError *error = NULL;
    gchar *response_str = NULL;
    
    GPtrArray *snapshots = purecvisor_zfs_snapshot_list_finish(res, &error);
    
    if (error != NULL) {
        response_str = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        g_error_free(error);
    } else {
        JsonArray *json_arr = json_array_new();
        if (snapshots) {
            for (guint i = 0; i < snapshots->len; i++) {
                json_array_add_string_element(json_arr, (const gchar *)g_ptr_array_index(snapshots, i));
            }
            g_ptr_array_unref(snapshots);
        }
        
        JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(result_node, json_arr);
        
        response_str = pure_rpc_build_success_response(ctx->rpc_id, result_node);
    }
    
    pure_uds_server_send_response(ctx->server, ctx->connection, response_str);
    g_free(response_str);
    rpc_async_context_free(ctx);
}


/* ========================================================================= */
/* 3. 라우팅 진입점 핸들러 (Dispatcher에서 호출됨) */
/* ========================================================================= */

// 파라미터 검증 헬퍼 매크로
#define VALIDATE_PARAMS(params, rpc_id, server, conn) \
    if (!json_object_has_member(params, "vm_name") || !json_object_has_member(params, "snap_name")) { \
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing vm_name or snap_name"); \
        pure_uds_server_send_response(server, conn, err); \
        g_free(err); \
        return; \
    }



// 1. [창조] 0.1초 만에 시간의 흐름을 얼려버립니다.
void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *snap_name = json_object_get_string_member(params, "snap_name");
    
    // 타격 명령어: zfs snapshot rpool/vms/target-vm@snap1
    gchar *cmd = g_strdup_printf("zfs snapshot rpool/vms/%s@%s", vm_id, snap_name);
    gchar *std_err = NULL; gint exit_status = 0;

    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, NULL) || exit_status != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, std_err ? g_strstrip(std_err) : "Snapshot Create Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE); json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    g_free(cmd); if (std_err) g_free(std_err);
}

// 2. [회귀] 특정 시점의 과거로 디스크 블록을 완벽하게 되돌립니다.
void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *snap_name = json_object_get_string_member(params, "snap_name");
    
    // 타격 명령어: zfs rollback -r rpool/vms/target-vm@snap1 
    // (-r 플래그는 돌아가려는 과거 이후에 생긴 미래의 스냅샷들을 자비 없이 파괴합니다)
    gchar *cmd = g_strdup_printf("zfs rollback -r rpool/vms/%s@%s", vm_id, snap_name);
    gchar *std_err = NULL; gint exit_status = 0;

    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, NULL) || exit_status != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, std_err ? g_strstrip(std_err) : "Rollback Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE); json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    g_free(cmd); if (std_err) g_free(std_err);
}

// 3. [조회] 얼어붙은 시간선(타임라인)들을 나열합니다.
void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    gchar *cmd = g_strdup_printf("zfs list -H -o name,creation -t snapshot -r rpool/vms/%s", vm_id);
    gchar *std_out = NULL; gchar *std_err = NULL; gint exit_status = 0;

    if (g_spawn_command_line_sync(cmd, &std_out, &std_err, &exit_status, NULL) && exit_status == 0) {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(res, std_out && strlen(std_out) > 0 ? g_strstrip(std_out) : "[ NO SNAPSHOTS DETECTED ]");
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, std_err ? g_strstrip(std_err) : "List Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    }
    g_free(cmd); if (std_out) g_free(std_out); if (std_err) g_free(std_err);
}

// =================================================================
// 4. [소멸] 얼어붙은 시간선(스냅샷)을 영구적으로 파괴합니다.
// =================================================================
void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *snap_name = json_object_get_string_member(params, "snap_name");
    
    // 타격 명령어: zfs destroy rpool/vms/target-vm@snap1
    gchar *cmd = g_strdup_printf("zfs destroy rpool/vms/%s@%s", vm_id, snap_name);
    gchar *std_err = NULL; gint exit_status = 0;

    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, NULL) || exit_status != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, std_err ? g_strstrip(std_err) : "Snapshot Delete Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE); json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    
    g_free(cmd); if (std_err) g_free(std_err);
}

