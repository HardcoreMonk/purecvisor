/**
 * @file handler_storage.c
 * @brief ZFS 스토리지 RPC 핸들러 — 풀 목록, zvol CRUD
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c -> handle_storage_*()
 *                                              -> pcv_spawn_sync ("zfs list", "zfs create", "zfs destroy")
 *
 * [처리하는 RPC 메서드] (4개)
 *   storage.pool.list  -> handle_storage_pool_list_request  : ZFS 풀 목록 (이름, 크기, 사용량, 상태)
 *   storage.zvol.list  -> handle_storage_zvol_list_request  : ZFS zvol 블록 디바이스 목록
 *   storage.zvol.create-> handle_storage_zvol_create_request: 새 zvol 생성 (VM 디스크용)
 *   storage.zvol.delete-> handle_storage_zvol_delete_request: zvol 삭제
 *
 * [fire-and-forget 패턴 미사용]
 *   모든 메서드가 동기 응답입니다.
 *   ZFS CLI 명령이 빠르게 완료되므로 비동기 처리가 불필요합니다.
 *
 * [주의사항]
 *   - execute_zfs_command_to_json() 내부 헬퍼가 ZFS 명령 출력을
 *     탭/줄바꿈 기준으로 파싱하여 JSON 배열로 변환합니다.
 *   - zvol.create의 경로는 pcvpool/vms/<name> 형식입니다.
 *   - zvol.delete는 멱등 — 이미 없는 zvol 삭제 시에도 에러를 반환할 수 있으므로 주의.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(name, size) 누락
 *   -32000 : ZFS 명령 실행 실패
 */
#include "handler_storage.h"
#include "rpc_utils.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "modules/storage/zfs_driver.h"
#include <glib.h>
#include <string.h>

/**
 * execute_zfs_command_to_json:
 * @command: 실행할 ZFS/zpool 명령 문자열 (예: "zpool list -H -o name,size")
 * @keys: 각 컬럼에 대응하는 JSON 키 이름 배열 (예: {"name", "size", ...})
 * @num_keys: keys 배열의 크기
 *
 * [공통 헬퍼] ZFS CLI 명령을 실행하고 결과를 JSON 배열로 변환합니다.
 *
 * [파싱 방식]
 *   - ZFS CLI에 -H 옵션을 사용하면 헤더 없이 탭(\t)으로 구분된 출력을 얻습니다.
 *   - 각 줄을 탭으로 분리하여 keys[i]에 대응하는 JSON 객체로 매핑합니다.
 *   - 빈 줄은 무시합니다.
 *
 * [보안] command 문자열은 g_shell_parse_argv()로 분리 후 pcv_spawn_sync()로 실행합니다.
 *   셸 해석(shell interpretation) 없이 직접 execve하므로 명령 인젝션이 방지됩니다.
 *
 * [에러 처리] 명령 실패 시 빈 JSON 배열을 반환합니다 (에러는 stderr에 출력).
 *
 * @returns: JsonArray* — 호출자가 소유권을 가짐 (json_node_take_array로 이전 가능)
 */
static JsonArray* execute_zfs_command_to_json(const gchar *command, const gchar **keys, int num_keys) {
    gchar *std_out = NULL;
    gchar *std_err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    JsonArray *json_arr = json_array_new();

    /*
     * g_shell_parse_argv()로 명령 문자열을 argv 배열로 분리합니다.
     * 이후 pcv_spawn_sync()로 직접 실행 — system()이나 popen() 대신 사용하여
     * 셸 메타문자 인젝션을 방지합니다.
     */
    gchar **parsed_argv = NULL;
    if (!g_shell_parse_argv(command, NULL, &parsed_argv, &error)) {
        g_printerr("[ZFS] Failed to parse command: %s\n", error->message);
        g_error_free(error);
        return json_arr;
    }
    if (!pcv_spawn_sync((const gchar * const *)parsed_argv, &std_out, &std_err, &error)) {
        g_printerr("[ZFS] Command failed: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_strfreev(parsed_argv);
        return json_arr;
    }
    g_strfreev(parsed_argv);
    exit_status = 0; /* pcv_spawn_sync returns FALSE on non-zero exit */

    /* 명령 출력을 줄(\n) → 탭(\t)으로 2단계 파싱하여 JSON 배열로 변환 */
    if (exit_status == 0 && std_out != NULL) {
        gchar **lines = g_strsplit(std_out, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            if (strlen(lines[i]) == 0) continue; /* 빈 줄 무시 (마지막 줄 등) */

            /* 각 줄을 탭으로 분리하여 keys[j]에 매핑 */
            gchar **tokens = g_strsplit(lines[i], "\t", -1);
            JsonObject *row_obj = json_object_new();

            for (int j = 0; j < num_keys && tokens[j] != NULL; j++) {
                gchar *clean_val = g_strstrip(g_strdup(tokens[j]));  /* 양쪽 공백 제거 */
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
    // zfs list: 타입이 volume(ZVOL)인 것만, 이름, 논리크기, 실제사용량, 참조크기, 압축비, 중복제거, 기록량을 탭(-H)으로 구분하여 출력
    const gchar *cmd = "zfs list -t volume -H -o name,volsize,used,refer,compressratio,dedup,written";
    const gchar *keys[] = {"name", "volsize", "used", "refer", "compression_ratio", "dedup", "written"};

    JsonArray *result_arr = execute_zfs_command_to_json(cmd, keys, 7);
    
    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, result_arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* =================================================================
 * [API 진입점] ZVOL 생성 (동적 프로비저닝)
 *
 * [이중 필드 지원 (CLI + Web UI 호환)]
 *   CLI에서는 "zvol_path"와 "size" 문자열을 사용하고,
 *   Web UI에서는 "name"과 "size_gb" 숫자를 사용합니다.
 *   양쪽 모두 지원하기 위해 두 필드명을 순서대로 확인합니다.
 *
 *   zvol_path → name   (문자열, 예: "pcvpool/vms/web-prod")
 *   size → size_gb     (문자열 "10G" 또는 숫자 10)
 *
 * [보안] zvol_path/name 값은 ZFS 명령에 직접 전달됩니다.
 *   pcv_spawn_sync()가 셸 해석 없이 execve하므로 인젝션은 방지되지만,
 *   경로 탈출(path traversal) 가능성은 ZFS 자체의 데이터셋 이름 규칙으로 차단됩니다.
 * ================================================================= */
void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* zvol_path (CLI: 풀 경로 포함) 또는 name (Web UI: 이름만) — 둘 다 시도 */
    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");
    if (!zvol_path)
        zvol_path = json_object_get_string_member(params, "name");
    /* size (CLI: "10G" 문자열) 또는 size_gb (Web UI: 숫자 → "10G" 변환) */
    const gchar *size = json_object_get_string_member(params, "size");
    gchar *size_buf = NULL;
    if (!size && json_object_has_member(params, "size_gb")) {
        gint64 gb = json_object_get_int_member(params, "size_gb");
        size_buf = g_strdup_printf("%ldG", (long)gb);
        size = size_buf;
    }

    if (!zvol_path || !zvol_path[0] || !size) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path'/'name' or 'size'/'size_gb'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); g_free(size_buf); return;
    }

    /* name 검증: 영문/숫자/_.- 만 허용 (command injection 방어) */
    for (const gchar *p = zvol_path; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != '/') {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid zvol name — allowed: [a-zA-Z0-9_.-/]");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(size_buf); return;
        }
    }

    /* name에 '/'가 없으면 ZFS 풀 경로를 자동으로 추가 (Web UI 호환)
     * 예: "data-disk" → "pcvpool/vms/data-disk" */
    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/')) {
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);
        zvol_path = full_path;
    }

    /* pcv_spawn_sync argv 배열 방식 — command injection 방지 */
    const gchar *zfs_argv[] = {"zfs", "create", "-V", size, zvol_path, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
        gchar *err_msg = error ? error->message
                       : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        PCV_LOG_INFO("storage", "zvol created: %s (%s)", zvol_path, size);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(full_path);
    g_free(size_buf);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}

/* =================================================================
 * [API 진입점] ZVOL 삭제
 *
 * [이중 필드 지원] zvol_path (CLI) 또는 name (Web UI) 모두 허용
 *
 * [zfs destroy -r 옵션]
 *   -r (recursive): 해당 데이터셋의 모든 자식 스냅샷을 포함하여 삭제합니다.
 *   VM의 zvol에 스냅샷이 남아있으면 -r 없이는 삭제 실패하므로 항상 -r을 사용합니다.
 *
 * [주의] 이 작업은 되돌릴 수 없습니다!
 *   zvol과 모든 스냅샷이 즉시 영구 삭제됩니다.
 * ================================================================= */
void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* zvol_path (CLI: 풀 경로 포함) 또는 name (Web UI: 이름만) — 둘 다 시도 */
    const gchar *zvol_path = json_object_get_string_member(params, "zvol_path");
    if (!zvol_path)
        zvol_path = json_object_get_string_member(params, "name");

    if (!zvol_path || !zvol_path[0]) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'zvol_path' or 'name'");
        pure_uds_server_send_response(server, connection, resp); g_free(resp); return;
    }

    /* name에 '/'가 없으면 ZFS 풀 경로를 자동으로 추가 (Web UI 호환) */
    gchar *full_path = NULL;
    if (!strchr(zvol_path, '/')) {
        full_path = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), zvol_path);
        zvol_path = full_path;
    }

    /* [감사 AF-S1] 이전엔 무검증 `zfs destroy -r <zvol_path>`라 zvol_path="pcvpool/vms"
     * 같은 상위 데이터셋을 지정하면 하위 전 VM zvol을 재귀 삭제할 수 있었다.
     * 방어 2중: (1) 문자 화이트리스트 + traversal 차단, (2) 대상이 실제 zvol(volume)
     * 타입인지 확인 — 상위 데이터셋(filesystem)이면 거부. -r은 zvol의 스냅샷 정리에만
     * 작용하고, volume은 데이터셋 자식이 없으므로 형제 파괴가 불가능해진다. */
    {
        gboolean bad = (zvol_path[0] == '/') || strstr(zvol_path, "..") || strstr(zvol_path, "//");
        for (const gchar *c = zvol_path; !bad && *c; c++) {
            if (!g_ascii_isalnum(*c) && *c != '_' && *c != '-' && *c != '.' && *c != '/')
                bad = TRUE;
        }
        if (bad) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid zvol_path");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
            g_free(full_path);
            return;
        }
        const gchar *type_argv[] = {"zfs", "list", "-H", "-o", "type", zvol_path, NULL};
        gchar *type_out = NULL;
        gboolean ok = pcv_spawn_sync(type_argv, &type_out, NULL, NULL);
        gboolean is_vol = ok && type_out && g_strcmp0(g_strstrip(type_out), "volume") == 0;
        g_free(type_out);
        if (!is_vol) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "Refused: target is not a zvol (volume) — parent datasets cannot be recursively destroyed");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
            g_free(full_path);
            return;
        }
    }

    /* pcv_spawn_sync argv 배열 방식 — command injection 방지 */
    const gchar *zfs_argv[] = {"zfs", "destroy", "-r", zvol_path, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
        gchar *err_msg = error ? error->message
                       : (std_err ? g_strstrip(std_err) : "Unknown ZFS execution error");
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        PCV_LOG_INFO("storage", "zvol destroyed: %s", zvol_path);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(full_path);
    if (std_err) g_free(std_err);
    if (error) g_error_free(error);
}

/* =================================================================
 * [API 진입점] ZFS 풀 생성 — storage.pool.create RPC
 *
 * @param params: { "name": "mypool", "vdev_type": "mirror", "disks": ["sdb","sdc"] }
 *   - name: 풀 이름 (필수)
 *   - vdev_type: VDEV 유형 ("mirror","raidz","raidz2","") — 빈 문자열이면 stripe
 *   - disks: 디스크 경로 배열 (필수, 최소 1개)
 *
 * [동기 응답] zpool create는 빠르게 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_storage_pool_create_request(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *vdev_type = json_object_has_member(params, "vdev_type")
        ? json_object_get_string_member(params, "vdev_type") : NULL;

    if (!name || !json_object_has_member(params, "disks")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name' or 'disks'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonArray *disks_arr = json_object_get_array_member(params, "disks");
    guint n_disks = json_array_get_length(disks_arr);
    if (n_disks == 0) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "'disks' array is empty");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    /* JsonArray -> const gchar** 배열 변환 */
    const gchar **disk_list = g_new0(const gchar *, n_disks);
    for (guint i = 0; i < n_disks; i++) {
        disk_list[i] = json_array_get_string_element(disks_arr, i);
    }

    GError *error = NULL;
    /* compression 파라미터: RPC params에서 읽거나 NULL(기본 lz4) */
    const gchar *compression = NULL;
    if (json_object_has_member(params, "compression"))
        compression = json_object_get_string_member(params, "compression");
    gboolean ok = purecvisor_zfs_create_pool(name, vdev_type, disk_list, (gint)n_disks, compression, &error);
    g_free(disk_list);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool create failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "created", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* =================================================================
 * [API 진입점] ZFS 풀 삭제 — storage.pool.destroy RPC
 *
 * @param params: { "name": "mypool" }
 *
 * [주의] 풀 내 모든 데이터셋/스냅샷이 즉시 영구 삭제됩니다!
 * [동기 응답]
 * ================================================================= */
void handle_storage_pool_destroy_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_destroy_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool destroy failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "destroyed", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* =================================================================
 * [API 진입점] ZFS 풀 스크럽 — storage.pool.scrub RPC
 *
 * @param params: { "name": "pcvpool" }
 *
 * 스크럽은 백그라운드에서 실행되며, 완료까지 수 시간이 걸릴 수 있습니다.
 * [동기 응답] zpool scrub 명령 자체는 즉시 반환 (백그라운드 실행).
 * ================================================================= */
void handle_storage_pool_scrub_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_get_string_member(params, "name");
    if (!name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing 'name'");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *error = NULL;
    gboolean ok = purecvisor_zfs_scrub_pool(name, &error);

    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            error ? error->message : "zpool scrub failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (error) g_error_free(error);
        return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "pool", name);
    json_object_set_boolean_member(res_obj, "scrub_started", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══ ISO LIST ═══ */
#include "../../utils/pcv_config.h"
#include <sys/stat.h>

void handle_iso_list(JsonObject *params, const gchar *rpc_id,
                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    const gchar *iso_dirs_str = pcv_config_get_iso_dirs();
    gchar **dirs = g_strsplit(iso_dirs_str, ",", -1);
    JsonArray *arr = json_array_new();

    for (gint i = 0; dirs[i] != NULL; i++) {
        gchar *iso_dir = g_strstrip(dirs[i]);
        if (iso_dir[0] == '\0') continue;
        if (strstr(iso_dir, "..")) continue;

        GDir *dir = g_dir_open(iso_dir, 0, NULL);
        if (!dir) continue;

        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_suffix(name, ".iso") || g_str_has_suffix(name, ".ISO") ||
                g_str_has_suffix(name, ".img") || g_str_has_suffix(name, ".IMG")) {
                gchar *full = g_strdup_printf("%s/%s", iso_dir, name);
                struct stat st;
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_object_set_string_member(obj, "path", full);
                json_object_set_string_member(obj, "dir", iso_dir);
                if (stat(full, &st) == 0)
                    json_object_set_int_member(obj, "size_mb", (gint64)(st.st_size / (1024*1024)));
                json_array_add_object_element(arr, obj);
                g_free(full);
            }
        }
        g_dir_close(dir);
    }
    g_strfreev(dirs);

    JsonNode *iso_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(iso_node, arr);
    gchar *iso_resp = pure_rpc_build_success_response(rpc_id, iso_node);
    pure_uds_server_send_response(server, connection, iso_resp);
    g_free(iso_resp);
}