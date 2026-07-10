/**
 * @file handler_backup.c
 * @brief 백업 정책 관리 및 복원 RPC 핸들러 — 정책 CRUD + 이력 조회 + 복원 (5개 메서드)
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("backup.*") -> handle_backup_*()
 *                                                            -> backup_scheduler.c (정책 DB + ZFS 스냅샷)
 *
 * [처리하는 RPC 메서드] (5개)
 *   backup.policy.set    -> handle_backup_policy_set    : VM별 자동 백업 정책 설정
 *     - params: { "vm_name", "interval_hours": 정수, "retention_count": 정수 }
 *     - vm_name에 '*' 지정 시 전체 VM 일괄 백업 정책
 *   backup.policy.list   -> handle_backup_policy_list   : 전체 백업 정책 목록 조회
 *   backup.policy.delete -> handle_backup_policy_delete : 특정 VM의 백업 정책 삭제
 *     - params: { "vm_name" }
 *   backup.history       -> handle_backup_history       : 특정 VM의 ZFS 스냅샷 이력
 *     - params: { "vm_name" }
 *   backup.restore       -> handle_backup_restore       : ZFS 스냅샷 복원
 *     - params: { "vm_name", "snapshot_name" }
 *
 * [fire-and-forget 패턴 사용 여부]
 *   backup.restore와 backup.replicate는 fire-and-forget 비동기 패턴을 사용합니다:
 *     1. 즉시 {"status":"accepted"} 응답 전송 -> 소켓 닫힘
 *     2. GTask 워커 스레드에서 장시간 작업 실행 (수 초~수 분 소요)
 *     3. 결과는 ADR-0018에 따라 worker-result audit과 WS 완료 이벤트로 기록
 *   policy.set/list/delete, history 등 조회/정책 메서드는 동기 응답입니다.
 *
 * [주의사항]
 *   - backup_scheduler.c가 5분 주기로 GMainLoop 타이머에서 정책을 확인하고
 *     자동 스냅샷을 생성합니다. 이 핸들러는 정책 등록/관리만 담당합니다.
 *   - 보존 정책(retention_count)에 따라 오래된 스냅샷은 자동 삭제됩니다.
 *   - restore 중 VM이 실행 상태이면 중지 후 롤백합니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락 또는 범위 초과 (interval_hours < 1 등)
 *   -32000 : ZFS/DB 내부 실패
 */

/*
 * --- 헤더 인클루드 설명 ---
 *
 * handler_backup.h     : 이 파일의 공개 함수 선언 (handle_backup_policy_set 등 5개)
 * rpc_utils.h          : JSON-RPC 응답 빌더 + 에러코드 상수 + UDS 전송 함수
 * backup_scheduler.h   : 백업 코어 모듈 — PcvBackupPolicy 구조체,
 *                        pcv_backup_policy_set/list/delete, pcv_backup_history,
 *                        pcv_backup_restore 함수 (ZFS 스냅샷 기반)
 * uds_server.h         : UdsServer 타입 + pure_uds_server_send_response()
 * pcv_log.h            : PCV_LOG_INFO/WARN/ERR 매크로 (구조화 로깅)
 * gio.h                : GTask, GCancellable 등 비동기 작업 관련 타입 (backup.restore에서 사용)
 */
#include "handler_backup.h"
#include "rpc_utils.h"
#include "../backup/backup_scheduler.h"
#include "../audit/pcv_audit.h"
#include "../../api/uds_server.h"
#include "../../api/ws_server.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/*
 * 로그 태그: journalctl에서 이 핸들러의 로그를 필터링할 때 사용.
 * 예: journalctl -u purecvisorsd | grep backup_handler
 *     journalctl -u purecvisormd | grep backup_handler
 */
#define BACKUP_HANDLER_LOG "backup_handler"

/* ═══════════════════════════════════════════════════════════
 * backup.policy.set
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_policy_set:
 * @params: JSON-RPC params — {"vm_name", "interval_hours", "retention_count"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 필수 파라미터:
 *   - vm_name         : 대상 VM 이름 (libvirt 도메인 이름과 동일)
 *   - interval_hours  : 자동 스냅샷 주기 (시간 단위, 1 이상)
 *   - retention_count : 보존할 스냅샷 최대 개수 (1 이상, 초과 시 가장 오래된 것 삭제)
 *
 * 이미 정책이 존재하면 덮어쓰기 (UPSERT 동작).
 * 성공 시 반환: true (JsonNode boolean)
 */
void handle_backup_policy_set(JsonObject       *params,
                               const gchar      *rpc_id,
                               UdsServer        *server,
                               GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    gint interval_hours  = 0;
    gint retention_count = 0;

    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (params && json_object_has_member(params, "interval_hours"))
        interval_hours = (gint)json_object_get_int_member(params, "interval_hours");

    if (params && json_object_has_member(params, "retention_count"))
        retention_count = (gint)json_object_get_int_member(params, "retention_count");

    /* B8-M1/M2: interval_hours 상한 8760(1년), retention_count 상한 365 */
    if (!vm_name || interval_hours < 1 || interval_hours > 8760
        || retention_count < 1 || retention_count > 365) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid params: vm_name required, interval_hours 1~8760, retention_count 1~365");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    /* B8-M4: vm_name 문자 검증 ('*' 와일드카드 허용) */
    if (g_strcmp0(vm_name, "*") != 0 && !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_set(vm_name, interval_hours,
                                         retention_count, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to set policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.policy.list
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_policy_list:
 * @params: 사용하지 않음 (빈 객체 허용)
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 전체 백업 정책 목록 반환.
 * 반환 형식: JSON 배열 [{"vm_name", "interval_hours", "retention_count", "enabled"}, ...]
 * GPtrArray<PcvBackupPolicy*> → JsonArray 변환 후 g_ptr_array_unref 로 해제.
 */
void handle_backup_policy_list(JsonObject       *params __attribute__((unused)),
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
    GPtrArray *policies = pcv_backup_policy_list();

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(policies, i);
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", p->vm_name);
        json_object_set_int_member(obj, "interval_hours", p->interval_hours);
        json_object_set_int_member(obj, "retention_count", p->retention_count);
        json_object_set_boolean_member(obj, "enabled", p->enabled);
        json_array_add_object_element(arr, obj);
    }
    g_ptr_array_unref(policies);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);  /* arr 소유권 → node 이전 */
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.policy.delete
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_policy_delete:
 * @params: JSON-RPC params — {"vm_name"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 필수 파라미터: vm_name
 * 존재하지 않는 정책 삭제 시: pcv_backup_policy_delete 가 GError 반환 → -32000 에러
 * 성공 시 반환: true (JsonNode boolean)
 */
void handle_backup_policy_delete(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_delete(vm_name, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to delete policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.history
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_history:
 * @params: JSON-RPC params — {"vm_name"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 필수 파라미터: vm_name
 * 반환 형식: JSON 배열 ["snap1", "snap2", ...] (ZFS 스냅샷 이름 목록)
 * pcv_backup_history() 가 GPtrArray<gchar*> 반환 → JsonArray 문자열 배열로 변환.
 */
void handle_backup_history(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GPtrArray *snaps = pcv_backup_history(vm_name);

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < snaps->len; i++) {
        json_array_add_string_element(arr,
            (const gchar *)g_ptr_array_index(snaps, i));
    }
    g_ptr_array_unref(snaps);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.restore — fire-and-forget 비동기 패턴
 *
 * ⚠ 이 핸들러만 다른 핸들러와 패턴이 다릅니다:
 *   1. 즉시 {"status":"accepted"} 응답 → 소켓 닫힘
 *   2. GTask 워커 스레드에서 ZFS rollback 수행 (장시간 작업)
 *   3. 결과는 audit DB와 WS 완료 이벤트로 기록 (send_response 호출 금지 — 소켓 이미 닫힘)
 *
 * 왜 fire-and-forget 인가?
 *   ZFS rollback 은 디스크 크기에 따라 수 초~수 분 소요.
 *   클라이언트가 그 동안 소켓을 잡고 있으면 타임아웃 위험.
 * ═══════════════════════════════════════════════════════════ */

/**
 * RestoreTaskData:
 * GTask 워커 스레드에 전달할 컨텍스트 구조체.
 *
 * 왜 별도 구조체가 필요한가?
 *   fire-and-forget 패턴에서는 응답 전송 후 소켓이 닫히고 params(JsonObject)가 해제됩니다.
 *   GTask 워커는 별도 스레드에서 나중에 실행되므로, 그 시점에 params 포인터는 이미 dangling.
 *   따라서 필요한 데이터를 g_strdup()으로 복사하여 이 구조체에 보관합니다.
 *
 * 해제 책임:
 *   GTask에 GDestroyNotify(_restore_task_data_free)를 등록하여
 *   워커 완료 시 자동으로 해제됩니다. 수동 해제 불필요.
 */
typedef struct {
    gchar *vm_name;       /* g_strdup()으로 복사된 VM 이름 */
    gchar *snapshot_name; /* g_strdup()으로 복사된 스냅샷 이름 */
} RestoreTaskData;

/** GTask data 소멸자: vm_name, snapshot_name 문자열 해제 */
static void _restore_task_data_free(gpointer p)
{
    RestoreTaskData *d = (RestoreTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d->snapshot_name);
    g_free(d);
}

/**
 * _restore_worker:
 * GTask 스레드 풀에서 실행되는 워커 함수.
 *
 * GLib의 GTask 워커 함수 시그니처:
 *   void (*)(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
 *
 * 이 함수는 GLib 내부 스레드 풀의 워커 스레드에서 실행됩니다 (메인 스레드가 아님).
 * 따라서 GMainLoop(이벤트 루프)를 블록하지 않고 장시간 작업을 수행할 수 있습니다.
 *
 * pcv_backup_restore() 내부 동작:
 *   1. VM이 실행 중이면 virDomainShutdown()으로 중지
 *   2. zfs rollback -r pcvpool/vms/<vm_name>@<snapshot_name> 실행
 *   3. VM 재시작 (선택적)
 *   이 과정이 수 초~수 분 소요되므로 동기 응답으로는 처리할 수 없습니다.
 *
 * 결과 처리:
 *   - 성공/실패 모두 로그, audit DB, WS 완료 이벤트에 기록합니다.
 *   - 소켓은 이미 닫혀 있으므로 pure_uds_server_send_response()를 호출하면
 *     크래시(SEGFAULT) 또는 정의되지 않은 동작(UB)이 발생합니다. 절대 금지!
 *   - g_task_return_boolean/error: GTask 결과를 설정하지만, callback=NULL이므로
 *     실제 콜백은 호출되지 않습니다. GTask 내부 정리 용도로만 사용됩니다.
 */
static void _restore_worker(GTask        *task,
                              gpointer      source __attribute__((unused)),
                              gpointer      task_data,
                              GCancellable *cancel __attribute__((unused)))
{
    RestoreTaskData *d = (RestoreTaskData *)task_data;
    GError *err = NULL;

    gboolean ok = pcv_backup_restore(d->vm_name, d->snapshot_name, &err);
    gchar *target = g_strdup_printf("%s@%s", d->vm_name, d->snapshot_name);
    gchar *job_id = g_strdup_printf("backup.restore:%s", target);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async restore failed: %s@%s — %s",
                     d->vm_name, d->snapshot_name, err_msg);
        pcv_audit_log(NULL, "backup.restore", target, "fail", -32000, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err_msg);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Backup restore failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async restore complete: %s@%s",
                     d->vm_name, d->snapshot_name);
        pcv_audit_log(NULL, "backup.restore", target, "ok", 0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "completed", NULL);
        g_task_return_boolean(task, TRUE);
    }
    g_free(job_id);
    g_free(target);
}

/**
 * handle_backup_restore:
 * @params: JSON-RPC params — {"vm_name", "snapshot_name"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 필수 파라미터: vm_name, snapshot_name
 * 즉시 accepted 응답 전송 후 GTask 비동기로 ZFS 스냅샷 복원 실행.
 *
 * ⚠ 중요: 응답 전송 후 소켓이 닫히므로, GTask 콜백에서
 *   pure_uds_server_send_response 를 호출하면 안 됨 (크래시/UB 발생).
 *   → 워커 결과는 PCV_LOG, audit DB, WS 완료 이벤트로 기록.
 */
void handle_backup_restore(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name       = NULL;
    const gchar *snapshot_name = NULL;

    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");
    if (params && json_object_has_member(params, "snapshot_name"))
        snapshot_name = json_object_get_string_member(params, "snapshot_name");

    if (!vm_name || !snapshot_name) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: vm_name, snapshot_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    /* B8-M3/M4: vm_name + snapshot_name 문자 검증 */
    if (!pcv_validate_vm_name(vm_name) || !pcv_validate_vm_name(snapshot_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name or snapshot_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    /*
     * ======== fire-and-forget 패턴 시작 ========
     *
     * 핵심 원리:
     *   1. 즉시 "accepted" 응답을 클라이언트에 전송 → 소켓 닫힘
     *   2. 실제 ZFS rollback은 GTask 워커 스레드에서 비동기 실행
     *   3. 워커 결과는 로그에만 기록 (콜백에서 send_response 호출 금지)
     *
     * 왜 이 패턴이 필요한가?
     *   ZFS rollback은 디스크 크기에 따라 수 초~수 분 소요됩니다.
     *   동기 응답 방식에서는 클라이언트가 그 동안 소켓을 잡고 있어야 하며,
     *   30초 타임아웃(drain 설정)을 초과하면 강제 종료됩니다.
     *   따라서 즉시 "접수했다(accepted)"고 알려주고 백그라운드에서 처리합니다.
     *
     * 동기 핸들러와의 차이점 비교:
     *   동기:        검증 → 작업 수행 → 결과 응답 전송
     *   fire-forget: 검증 → "accepted" 응답 전송(소켓 닫힘) → GTask 비동기 작업
     */
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", vm_name);
    json_object_set_string_member(accepted, "snapshot_name", snapshot_name);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    /*
     * GTask 비동기 복원:
     *
     * 중요 — 문자열 복사 필수:
     *   vm_name, snapshot_name은 params(JsonObject)가 소유하는 문자열입니다.
     *   위에서 pure_uds_server_send_response()를 호출한 시점에 소켓이 닫히고,
     *   이후 디스패처가 params를 해제할 수 있습니다.
     *   GTask 워커는 나중에 다른 스레드에서 실행되므로, 그 시점에
     *   원본 포인터가 이미 해제되어 있을 수 있습니다 (use-after-free 위험).
     *   따라서 g_strdup()으로 안전하게 복사합니다.
     *
     * g_new0: g_malloc0과 동일하지만 타입 캐스트를 자동 처리.
     *   RestoreTaskData 크기만큼 힙 할당 + 0으로 초기화.
     */
    RestoreTaskData *d = g_new0(RestoreTaskData, 1);
    d->vm_name       = g_strdup(vm_name);
    d->snapshot_name = g_strdup(snapshot_name);

    /*
     * GTask 생성 및 비동기 실행:
     *
     * g_task_new(source_object, cancellable, callback, user_data):
     *   - source_object = NULL : 이 작업의 소유자 GObject (없으면 NULL)
     *   - cancellable   = NULL : 취소 토큰 (사용 안 함)
     *   - callback      = NULL : 완료 콜백 (fire-and-forget이므로 NULL)
     *   - user_data     = NULL : 콜백에 전달할 데이터 (콜백 없으므로 NULL)
     *
     * g_task_set_task_data(task, data, destroy_notify):
     *   - 워커에 전달할 데이터와 소멸자(해제 함수)를 등록합니다.
     *   - GTask가 완료/해제될 때 _restore_task_data_free(d)가 자동 호출됩니다.
     *
     * g_task_run_in_thread(task, worker_func):
     *   - GLib의 공유 스레드 풀에서 worker_func를 실행합니다.
     *   - 메인 스레드(GMainLoop)를 블록하지 않습니다.
     *
     * g_object_unref(task):
     *   - GTask 참조 카운트를 감소시킵니다.
     *   - 워커가 아직 실행 중이면 즉시 해제되지 않고, 완료 후 자동 정리됩니다.
     *   - 이 줄이 없으면 메모리 누수가 발생합니다.
     */
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_restore_task_data_free);
    g_task_run_in_thread(task, _restore_worker);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════════
 * backup.incremental — 증분 백업 (동기)
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_incremental:
 * @params: JSON-RPC params — {"name": "web-prod"}
 *
 * 최신 스냅샷 대비 증분 스냅샷을 생성하고 증분 스트림을 파일로 저장합니다.
 * 반환: {snapshot, base_snapshot, file, size_bytes, mode}
 */
void handle_backup_incremental(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
    const gchar *name = NULL;
    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");

    if (!name || *name == '\0') {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    JsonObject *result = pcv_backup_incremental(name, &err);
    if (!result) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Incremental backup failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.verify — 스냅샷 무결성 검증 (동기)
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_backup_verify:
 * @params: JSON-RPC params — {"name": "web-prod", "snapshot": "daily-20260329"}
 *
 * 스냅샷 존재 여부 + zfs send -n 무결성 검증.
 * 반환: {verified, snapshot, size_bytes, integrity}
 */
void handle_backup_verify(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection)
{
    const gchar *name     = NULL;
    const gchar *snapshot = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "snapshot"))
        snapshot = json_object_get_string_member(params, "snapshot");

    if (!name || !snapshot) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, snapshot");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    JsonObject *result = pcv_backup_verify(name, snapshot, &err);
    if (!result) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Verify failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ═══════════════════════════════════════════════════════════
 * backup.replicate — 크로스 노드 복제 (fire-and-forget)
 * ═══════════════════════════════════════════════════════════ */

/** GTask 워커 데이터: 복제에 필요한 파라미터를 안전하게 복사 */
typedef struct {
    gchar *vm_name;
    gchar *target_node;
    gchar *ssh_user;
} ReplicateTaskData;

static void _replicate_task_data_free(gpointer p)
{
    ReplicateTaskData *d = (ReplicateTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d->target_node);
    g_free(d->ssh_user);
    g_free(d);
}

/**
 * _replicate_worker:
 * GTask 워커 — pcv_backup_replicate()를 호출합니다.
 * 결과는 로그, audit DB, WS 완료 이벤트로 기록 (fire-and-forget).
 */
static void _replicate_worker(GTask        *task,
                               gpointer      source __attribute__((unused)),
                               gpointer      task_data,
                               GCancellable *cancel __attribute__((unused)))
{
    ReplicateTaskData *d = (ReplicateTaskData *)task_data;
    GError *err = NULL;

    gboolean ok = pcv_backup_replicate(d->vm_name, d->target_node,
                                        d->ssh_user, &err);
    gchar *target = g_strdup_printf("%s:%s", d->vm_name, d->target_node);
    gchar *job_id = g_strdup_printf("backup.replicate:%s", target);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async replication failed: %s → %s — %s",
                     d->vm_name, d->target_node, err_msg);
        pcv_audit_log(NULL, "backup.replicate", target, "fail", -32000, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.replicate", "failed", err_msg);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Backup replication failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async replication complete: %s → %s",
                     d->vm_name, d->target_node);
        pcv_audit_log(NULL, "backup.replicate", target, "ok", 0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.replicate", "completed", NULL);
        g_task_return_boolean(task, TRUE);
    }
    g_free(job_id);
    g_free(target);
}

/**
 * handle_backup_replicate:
 * @params: JSON-RPC params — {"name", "target_node", "ssh_user" (optional)}
 *
 * fire-and-forget 패턴: 즉시 accepted 응답 → GTask 비동기 복제.
 */
void handle_backup_replicate(JsonObject       *params,
                              const gchar      *rpc_id,
                              UdsServer        *server,
                              GSocketConnection *connection)
{
    const gchar *name        = NULL;
    const gchar *target_node = NULL;
    const gchar *ssh_user    = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "target_node"))
        target_node = json_object_get_string_member(params, "target_node");
    if (params && json_object_has_member(params, "ssh_user"))
        ssh_user = json_object_get_string_member(params, "ssh_user");

    if (!name || !target_node) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_remote_host(target_node)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (ssh_user && *ssh_user && !pcv_validate_ssh_user(ssh_user)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: ssh_user");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    /* fire-and-forget: 즉시 accepted 응답 */
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", name);
    json_object_set_string_member(accepted, "target_node", target_node);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    /* GTask 비동기 복제 */
    ReplicateTaskData *d = g_new0(ReplicateTaskData, 1);
    d->vm_name     = g_strdup(name);
    d->target_node = g_strdup(target_node);
    d->ssh_user    = g_strdup(ssh_user ? ssh_user : "");

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_replicate_task_data_free);
    g_task_run_in_thread(task, _replicate_worker);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════════
 * vm.snapshot.schedule.set — 스냅샷 스케줄 설정 (backup policy 래퍼)
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_snapshot_schedule_set:
 * @params: JSON-RPC params — {"vm_name", "interval_hours", "retention_count"}
 *
 * 기존 backup policy set 인프라에 위임하는 씬 래퍼.
 * 동기 응답: true (boolean)
 */
/* B8-M5: thin wrapper — 중복 제거, handle_backup_policy_set에 위임 */
void handle_snapshot_schedule_set(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    handle_backup_policy_set(params, rpc_id, server, connection);
}

/* ═══════════════════════════════════════════════════════════
 * vm.snapshot.schedule.list — 스냅샷 스케줄 목록 (backup policy 래퍼)
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_snapshot_schedule_list:
 * @params: 사용하지 않음
 *
 * 기존 backup policy list 인프라에 위임하는 씬 래퍼.
 * 반환: [{"vm_name","interval_hours","retention_count","enabled"}, ...]
 */
/* B8-M5: thin wrapper — 중복 제거, handle_backup_policy_list에 위임 */
void handle_snapshot_schedule_list(JsonObject       *params,
                                   const gchar      *rpc_id,
                                   UdsServer        *server,
                                   GSocketConnection *connection)
{
    handle_backup_policy_list(params, rpc_id, server, connection);
}

/* ═══════════════════════════════════════════════════════════
 * vm.snapshot.schedule.delete — 스냅샷 스케줄 삭제 (backup policy 래퍼)
 * ═══════════════════════════════════════════════════════════ */

/**
 * handle_snapshot_schedule_delete:
 * @params: JSON-RPC params — {"vm_name"}
 *
 * 기존 backup policy delete 인프라에 위임하는 씬 래퍼.
 * 성공 시 반환: true (boolean). 미존재 시 -32000 에러.
 */
/* B8-M5: thin wrapper — 중복 제거, handle_backup_policy_delete에 위임 */
void handle_snapshot_schedule_delete(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection)
{
    handle_backup_policy_delete(params, rpc_id, server, connection);
}
