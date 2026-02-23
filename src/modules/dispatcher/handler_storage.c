// src/modules/dispatcher/handler_storage.c
#include "handler_storage.h"
#include "rpc_utils.h"
#include <glib.h>
#include <string.h>

// [공통 헬퍼] 쉘 명령어를 실행하고 결과를 탭(\t)과 줄바꿈(\n)으로 파싱하여 JSON 배열로 반환
static JsonArray* execute_zfs_command_to_json(const gchar *command, const gchar **keys, int num_keys) {
    gchar *std_out = NULL;
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    JsonArray *json_arr = json_array_new();

    // 🚀 GLib의 서브프로세스 동기 실행
    if (!g_spawn_command_line_sync(command, &std_out, &std_err, &exit_status, &error)) {
        g_printerr("[ZFS] Command failed to execute: %s\n", error->message);
        g_error_free(error);
        return json_arr;
    }

    if (exit_status == 0 && std_out != NULL) {
        gchar **lines = g_strsplit(std_out, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            if (strlen(lines[i]) == 0) continue; // 빈 줄 무시

            gchar **tokens = g_strsplit(lines[i], "\t", -1);
            JsonObject *row_obj = json_object_new();
            
            for (int j = 0; j < num_keys && tokens[j] != NULL; j++) {
                gchar *clean_val = g_strstrip(g_strdup(tokens[j]));
                json_object_set_string_member(row_obj, keys[j], clean_val);
                g_free(clean_val);
            }
            json_array_add_object_element(json_arr, row_obj);
            g_strfreev(tokens);
        }
        g_strfreev(lines);
    }

    g_free(std_out);
    g_free(std_err);
    return json_arr;
}

// =================================================================
// [API 진입점] ZFS Pool 목록 조회
// =================================================================
void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    // zpool list: 이름, 전체크기, 사용량, 남은량, 상태를 탭(-H)으로 구분하여 출력
    const gchar *cmd = "zpool list -H -o name,size,alloc,free,health";
    const gchar *keys[] = {"name", "size", "alloc", "free", "health"};
    
    JsonArray *result_arr = execute_zfs_command_to_json(cmd, keys, 5);
    
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, result_arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

// =================================================================
// [API 진입점] ZVOL 목록 조회
// =================================================================
void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    // zfs list: 타입이 volume(ZVOL)인 것만, 이름, 논리크기, 실제사용량을 탭(-H)으로 구분하여 출력
    const gchar *cmd = "zfs list -t volume -H -o name,volsize,used";
    const gchar *keys[] = {"name", "volsize", "used"};
    
    JsonArray *result_arr = execute_zfs_command_to_json(cmd, keys, 3);
    
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, result_arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

// =================================================================
// [API 진입점] ZVOL 생성 (동적 프로비저닝)
// =================================================================
void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");
    const gchar *size = json_object_get_string_member(params, "size");

    if (!zvol_path || !size) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32602, "Missing 'zvol_path' or 'size'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); return;
    }

    // zfs create -V <size> <pool/path> (예: zfs create -V 10G tank/vms/new-vm)
    gchar *cmd = g_strdup_printf("zfs create -V %s %s", size, zvol_path);
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, &error) || exit_status != 0) {
        gchar *err_msg = error ? error->message : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        // 성공 시 빈 결과 객체를 반환하여 클라이언트 파서가 통과하도록 함
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(cmd);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}

// =================================================================
// [API 진입점] ZVOL 삭제
// =================================================================
void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");

    // zfs destroy <pool/path>
    gchar *cmd = g_strdup_printf("zfs destroy %s", zvol_path);
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, &error) || exit_status != 0) {
        gchar *err_msg = error ? error->message : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(cmd);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}