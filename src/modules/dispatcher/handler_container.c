/**
 * @file handler_container.c
 * @brief LXC 컨테이너 RPC 핸들러 — 생명주기/조회/exec/스냅샷 (11개 메서드)
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("container.*") -> handle_container_*()
 *                                                               -> lxc_driver.c (liblxc API)
 *                                                               -> pcv_spawn_sync (lxc-info 폴백)
 *
 * [처리하는 RPC 메서드] (11개)
 *   container.create            -> handle_container_create            : LXC 컨테이너 생성
 *   container.destroy           -> handle_container_destroy           : 컨테이너 삭제
 *   container.start             -> handle_container_start             : 컨테이너 시작
 *   container.stop              -> handle_container_stop              : 컨테이너 중지
 *   container.list              -> handle_container_list              : 전체 컨테이너 목록
 *   container.metrics           -> handle_container_metrics           : CPU/메모리/네트워크 메트릭
 *   container.exec              -> handle_container_exec              : 컨테이너 내부 명령 실행
 *   container.snapshot.create   -> handle_container_snapshot_create   : ZFS 스냅샷 생성
 *   container.snapshot.rollback -> handle_container_snapshot_rollback : ZFS 스냅샷 복원
 *   container.snapshot.delete   -> handle_container_snapshot_delete   : ZFS 스냅샷 삭제
 *   container.snapshot.list     -> handle_container_snapshot_list     : 스냅샷 목록 조회
 *
 * [핸들러 처리 패턴]
 *   1. params에서 필수 필드(name 등) 추출 및 pcv_validate 검증
 *   2. 필요한 경우 RpcCtx 구조체 할당 (name, rpc_id, server, connection ref 카운트 증가)
 *   3. pcv_lxc_*_async() 호출 (lxc_driver.c의 비동기 래퍼)
 *   4. create/destroy는 accepted 응답 후 콜백에서 audit/WS 완료 이벤트 기록
 *      나머지 콜백 기반 응답 경로는 완료 콜백에서 JSON 응답 전송
 *
 * [fire-and-forget 패턴]
 *   container.create와 container.destroy는 UDS 클라이언트 타임아웃을 피하기 위해
 *   accepted 응답을 먼저 보낸다. 실제 결과는 ADR-0018에 따라 콜백에서 audit DB와
 *   WS 완료 이벤트에 기록한다.
 *
 * [주의사항]
 *   - seccomp 환경에서 liblxc get_ips가 실패할 수 있으므로, lxc-info -iH CLI 폴백이
 *     lxc_driver.c에 구현되어 있습니다.
 *   - container.exec는 pcv_spawn_sync(/bin/sh -c)로 lxc-attach를 호출합니다.
 *     seccomp가 GSubprocess를 차단할 수 있어 폴백 경로를 사용합니다.
 *   - 컨테이너 스토리지 경로: pcvpool/containers/<name>
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(name, image, cmd 등) 누락 또는 검증 실패
 *   -32000 : LXC/ZFS 작업 실패
 */

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

/* ══════════════════════════════════════════════════════════════════════════
 * 공통 내부 컨텍스트
 *
 * 모든 컨테이너 RPC 핸들러가 공유하는 비동기 작업 컨텍스트입니다.
 * pcv_lxc_*_async() 함수에 전달되어 콜백에서 응답 전송에 사용됩니다.
 *
 * [콜백 패턴] (fire-and-forget이 아님!)
 *   이 파일의 모든 핸들러는 pcv_lxc_*_async()의 콜백에서 응답을 전송합니다.
 *   따라서 소켓은 콜백 시점까지 유지되어야 하며,
 *   server/conn은 g_object_ref()로 참조 카운트를 증가시켜 보관합니다.
 *
 * [ContainerCtx 생명주기]
 *   1. _ctx_new()로 할당 (진입점 함수에서)
 *   2. pcv_lxc_*_async()에 user_data로 전달
 *   3. 콜백(_on_*_done)에서 응답 전송 후 _ctx_free()로 해제
 *
 * [주의] GTask의 GDestroyNotify로 등록되지 않습니다 — 콜백에서 수동 해제합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    gchar            *name;       /**< 컨테이너 이름 (검증 완료된 값) */
    gchar            *rpc_id;     /**< JSON-RPC 요청 ID (응답 매칭용) */
    UdsServer        *server;     /**< UDS 서버 인스턴스 (ref 카운트 증가됨) */
    GSocketConnection *conn;      /**< 클라이언트 소켓 연결 (ref 카운트 증가됨) */
    /* 핸들러별 추가 파라미터 (범용 필드) */
    gchar            *str_param;  /**< image, snap_name, exec_cmd 등 핸들러별 문자열 파라미터 */
    gboolean          bool_param; /**< force 플래그 등 핸들러별 불리언 파라미터 */
} ContainerCtx;

/**
 * _ctx_new:
 * ContainerCtx를 할당하고 기본 필드를 초기화합니다.
 * server/conn은 g_object_ref()로 참조 카운트를 증가시킵니다.
 * str_param, bool_param은 호출자가 필요 시 별도로 설정합니다.
 */
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

/**
 * _ctx_free:
 * ContainerCtx의 모든 필드를 안전하게 해제합니다.
 * 콜백 함수(_on_*_done)의 마지막에서 반드시 호출해야 합니다.
 * str_param이 NULL이어도 g_free(NULL)은 안전합니다.
 */
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

/**
 * _ensure_container_config_ready:
 * 컨테이너 dataset을 다시 마운트하고, 실제 config 파일이 보이는지 확인합니다.
 *
 * [배경]
 *   컨테이너 dataset이 내려간 상태에서는 /var/lib/purecvisor/lxc/<name> 상위 디렉터리만
 *   보이므로, 이 상태에서 config를 읽거나 쓰면 lower-layer 빈 디렉터리를 잘못 건드릴 수
 *   있습니다. env/volume 설정이 성공처럼 보여도 실제 dataset config에는 반영되지 않는
 *   false-success를 막기 위해, 모든 config read/write 전에 이 helper를 거칩니다.
 */
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
        /* 이미 보이는 config가 없고, mount 실패도 generic이면 false-fail을 막기 위해 한 번 더 확인 */
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

/* ── 응답 헬퍼 ────────────────────────────────────────────────────────── */

/**
 * _send_ok:
 * 성공 응답 {"success": true}을 클라이언트에 전송합니다.
 * 대부분의 컨테이너 RPC(create/destroy/start/stop/snapshot)의 성공 경로에서 사용됩니다.
 */
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

/**
 * _send_error:
 * 에러 응답을 클라이언트에 전송합니다.
 * @code: JSON-RPC 에러 코드 (-32602: 파라미터 오류, -32000: 내부 오류)
 * @msg: 에러 메시지 문자열 (NULL이면 기본 메시지 사용)
 */
static void
_send_error(ContainerCtx *ctx, PureRpcErrorCode code, const gchar *msg)
{
    gchar *resp = pure_rpc_build_error_response(ctx->rpc_id, code, msg);
    pure_uds_server_send_response(ctx->server, ctx->conn, resp);
    g_free(resp);
}

/* ══════════════════════════════════════════════════════════════════════════
 * container.create
 *
 * 파라미터:
 *   name*         : 컨테이너 이름
 *   image         : "ubuntu:22.04" (기본값)
 *   memory_mb     : 메모리 제한 (기본 512)
 *   vcpu_count    : vCPU 수 (기본 1)
 *   network_bridge: 브릿지 이름 (기본 virbr0)
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    ContainerCtx *base;
    gchar        *image;
    guint         memory_mb;
    guint         vcpu_count;
    gchar        *bridge;
    gint          rootless;    /* -1=global default, 0=force off, 1=force on (C-6) */
} CreateCtx;

static void
_create_ctx_free(CreateCtx *c)
{
    _ctx_free(c->base);
    g_free(c->image);
    g_free(c->bridge);
    g_free(c);
}

/**
 * _on_create_done:
 * pcv_lxc_create_async() 완료 시 호출되는 콜백입니다.
 *
 * [콜백 패턴 핵심]
 *   1. 오퍼레이션 잠금 해제 (unlock_vm_operation) — 반드시 최우선으로 실행
 *   2. 성공/실패에 따라 _send_ok() 또는 _send_error() 호출
 *   3. CreateCtx 메모리 해제 (_create_ctx_free)
 *
 * [주의] 잠금 해제를 누락하면 해당 컨테이너에 대한 모든 후속 RPC가 영구 차단됩니다.
 */
static void
_on_create_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    CreateCtx *ctx = (CreateCtx *)user_data;
    GError    *error = NULL;

    /* [fire-and-forget 콜백 규칙]
     * handle_container_create()에서 "accepted" 응답이 이미 전송되었으므로
     * 이 콜백에서 pure_uds_server_send_response() 호출 시 크래시/UB 발생.
     * 성공/실패 모두 로그로만 기록한다 */
    unlock_vm_operation(ctx->base->name);   /* 오퍼레이션 잠금 해제 — 반드시 최우선 */
    gchar *job_id = g_strdup_printf("container.create:%s", ctx->base->name);
    if (!pcv_lxc_create_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown error";
        /* fire-and-forget: 응답은 이미 전송됨 — 에러만 로깅 */
        g_warning("container.create failed for '%s': %s",
                  ctx->base->name, err_msg);
        pcv_audit_log(NULL, "container.create", ctx->base->name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
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

    /*
     * [보안] 입력 검증 (A-3 패턴)
     *
     * 모든 사용자 입력은 pcv_validate_* 함수로 검증합니다.
     * 이는 명령어 인젝션(command injection)을 방지하기 위한 필수 단계입니다.
     *
     * pcv_validate_vm_name: 1-64자, [a-zA-Z0-9_-] 만 허용
     * pcv_validate_container_image: "distro:release" 형식만 허용
     * pcv_validate_bridge_name: 1-16자, [a-zA-Z0-9_-] 만 허용
     *
     * 검증 실패 시 -32602 에러를 즉시 반환하고 함수를 종료합니다.
     */
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

    /*
     * [상태 검사] 오퍼레이션 잠금 획득 (B-1 패턴)
     *
     * lock_vm_operation()은 vm_state.c의 SQLite WAL 기반 잠금입니다.
     * 동일 컨테이너에 대한 동시 create/start/stop/destroy를 방지합니다.
     * 잠금 실패 시 "Container is busy" 에러를 반환합니다.
     *
     * [중요] 잠금은 콜백(_on_create_done)에서 반드시 해제해야 합니다.
     */
    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_CREATING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }

    /* rootless 옵션 (C-6): per-container 오버라이드 또는 전역 기본값 사용 */
    gint rootless = -1;  /* -1 = daemon.conf 전역 설정 사용 */
    if (json_object_has_member(params, "rootless")) {
        rootless = json_object_get_boolean_member(params, "rootless") ? 1 : 0;
    }

    /* fire-and-forget: 즉시 "accepted" 응답 전송 후 비동기 생성 실행
     * UDS 클라이언트는 2초 타임아웃이므로, 먼저 응답을 전송합니다. */
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

    /* 확장 컨텍스트 구성: 기본 ctx + create 전용 필드 */
    CreateCtx *ctx  = g_new0(CreateCtx, 1);
    ctx->base       = _ctx_new(name, rpc_id, server, conn);
    ctx->image      = g_strdup(image);
    ctx->memory_mb  = memory_mb;
    ctx->vcpu_count = vcpu_count;
    ctx->bridge     = g_strdup(bridge ? bridge : PCV_LXC_DEFAULT_BRIDGE);
    ctx->rootless   = rootless;

    /* lxc_driver.c의 비동기 래퍼 호출 — 콜백에서 잠금 해제 */
    pcv_lxc_create_async_full(name, ctx->image, memory_mb, vcpu_count,
                               ctx->bridge, rootless,
                               NULL, _on_create_done, ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * container.destroy
 *
 * 컨테이너를 완전히 삭제합니다 (LXC 정의 + rootfs 제거).
 * 실행 중인 컨테이너도 강제 종료 후 삭제합니다.
 *
 * 파라미터: name* (컨테이너 이름)
 * 응답: {"success": true}
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _on_destroy_done:
 * pcv_lxc_destroy_async() 완료 콜백.
 * 오퍼레이션 잠금 해제 -> 로그 기록 -> 컨텍스트 해제 순서를 반드시 지킵니다.
 *
 * [fire-and-forget 콜백] "accepted" 응답은 handle_container_destroy()에서 이미 전송됨.
 * 이 콜백에서 send_response 호출 금지 — 소켓이 이미 닫혀 있어 크래시/UB 발생.
 */
static void
_on_destroy_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    unlock_vm_operation(ctx->name);   /* B-1: 락 해제 — 누락 시 해당 컨테이너 영구 잠김 */
    gchar *job_id = g_strdup_printf("container.destroy:%s", ctx->name);
    if (!pcv_lxc_destroy_finish(res, &error)) {
        const gchar *err_msg = error ? error->message : "unknown";
        g_warning("container.destroy failed for '%s': %s",
                  ctx->name, err_msg);
        pcv_audit_log(NULL, "container.destroy", ctx->name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
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

    /* A-3: 입력 검증 */
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    /* B-1: 오퍼레이션 락 */
    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_DELETING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                       lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, conn, e);
        g_free(e); g_free(lock_err); return;
    }

    /* fire-and-forget: 즉시 응답 후 비동기 삭제 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.start
 *
 * 중지된 컨테이너를 시작합니다. 이미 실행 중이면 lxc_driver에서 에러를 반환합니다.
 *
 * 파라미터: name* (컨테이너 이름)
 * 응답: {"success": true}
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _on_start_done:
 * pcv_lxc_start_async() 완료 콜백.
 * 오퍼레이션 잠금 해제 -> 로그 기록 -> 컨텍스트 해제.
 *
 * [fire-and-forget 콜백] send_response 호출 금지 — 소켓 이미 닫힘.
 */
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

    /* fire-and-forget: 즉시 응답 전송 후 비동기 실행.
     * 이 send_response() 호출 이후 소켓이 닫히므로,
     * _on_start_done 콜백에서 send_response를 절대 호출하면 안 됨 (크래시/UB 발생) */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.stop
 *
 * 실행 중인 컨테이너를 중지합니다.
 * force=true이면 SIGKILL로 강제 종료, false이면 graceful shutdown을 시도합니다.
 *
 * 파라미터: name* (컨테이너 이름), force (bool, 기본 false)
 * 응답: {"success": true}
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _on_stop_done:
 * pcv_lxc_stop_async() 완료 콜백.
 * 오퍼레이션 잠금 해제 -> 로그 기록 -> 컨텍스트 해제.
 *
 * [fire-and-forget 콜백] send_response 호출 금지 — 소켓 이미 닫힘.
 */
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

    /* fire-and-forget: 즉시 응답 전송 후 비동기 중지.
     * _on_stop_done 콜백에서 send_response 호출 금지 (소켓 이미 닫힘) */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.list  (동기 조회 → 직접 응답)
 *
 * 모든 LXC 컨테이너의 목록을 조회합니다.
 * pcv_lxc_list()가 동기적으로 /var/lib/lxc/를 스캔하므로
 * GTask 비동기 없이 즉시 응답합니다.
 *
 * 파라미터: 없음
 * 응답: { "result": [ { "name":"...", "state":"RUNNING", "ip_addr":"10.0.3.5", "image":"ubuntu:22.04" }, ... ] }
 *
 * [IP 주소 조회] seccomp 환경에서 liblxc get_ips가 실패할 수 있어,
 *               lxc_driver.c 내부에서 lxc-info -iH CLI 폴백을 사용합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* 페이지네이션 지원 (offset/limit, 없으면 전체 반환 — 하위 호환) */
    gint pg_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    gint pg_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;

    if (pg_limit > 0) {
        /* OOM 방어: 클라이언트가 limit=999999999를 보내면 수십만 건의 JSON을 한 번에 조립하게 되어
         * 데몬 메모리가 폭증할 수 있음. 10000은 vm.list와 동일한 안전 상한선 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.metrics  (동기 조회)
 *
 * 단일 컨테이너의 상세 메트릭을 조회합니다.
 * pcv_lxc_get_metrics()가 cgroup v2에서 CPU/메모리/네트워크 통계를 읽어옵니다.
 *
 * 파라미터: name* (컨테이너 이름)
 * 응답: { "result": { "name":"...", "state":"RUNNING",
 *                     "mem_used_mb":256.5, "mem_limit_mb":512.0,
 *                     "cpu_percent":12.3,
 *                     "net_rx_mb":5.2, "net_tx_mb":1.8,
 *                     "ip_addr":"10.0.3.5", "init_pid":12345 } }
 *
 * [단위 변환] lxc_driver는 바이트 단위로 반환하며, 이 핸들러에서 MB로 변환합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* A-3: 입력 검증 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.exec
 *
 * 파라미터: name*, cmd* (문자열, 예: "/bin/bash -c 'ls -la'")
 * 응답: { "result": { "output": "..." } }
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _on_exec_done:
 * pcv_lxc_exec_async() 완료 콜백.
 * 명령 실행 결과(stdout)를 {"output": "..."} 형식으로 응답합니다.
 */
static void
_on_exec_done(GObject *src __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    ContainerCtx *ctx = (ContainerCtx *)user_data;
    GError       *error = NULL;

    /* 실행 결과 수신 — NULL이면 exec 실패 */
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

/**
 * handle_container_exec:
 * container.exec RPC 진입점 — 컨테이너 내부에서 명령을 실행합니다.
 *
 * [보안 주의사항]
 *   - pcv_validate_exec_cmd()로 cmd 문자열을 검증합니다 (1-1024자, NULL 바이트 금지)
 *   - cmd는 /bin/sh -c를 통해 컨테이너 내부에서 실행됩니다
 *   - lxc-attach는 seccomp 환경에서 GSubprocess가 차단될 수 있어
 *     pcv_spawn_sync(/bin/sh -c) 폴백을 사용합니다 (lxc_driver.c에서 처리)
 *
 * [명령 인젝션 방지]
 *   pcv_validate_exec_cmd()가 NULL 바이트와 과도한 길이를 차단합니다.
 *   단, 셸 메타문자(;, |, & 등)는 컨테이너 내부에서 실행되므로
 *   호스트 보안에는 영향 없습니다 (컨테이너 격리 의존).
 */
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

    /* [보안] 입력 검증 — 이름과 명령어 모두 검증 */
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

    /* cmd 문자열을 컨테이너 내부 shell에 전달: ["/bin/sh", "-c", cmd] */
    const gchar *argv[] = { "/bin/sh", "-c", cmd, NULL };

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_exec_async(name, argv, NULL, _on_exec_done, ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * container.snapshot.create
 *
 * 컨테이너의 ZFS 스냅샷을 생성합니다.
 * 스토리지 경로: pcvpool/containers/<name>@<snap_name>
 *
 * 파라미터: name* (컨테이너 이름), snap_name* (스냅샷 이름)
 * 응답: {"success": true}
 *
 * [입력 검증] pcv_validate_snap_name()으로 스냅샷 이름의 안전성을 검증합니다.
 *            ZFS 명령어 인젝션을 방지하기 위해 [a-zA-Z0-9_-] 만 허용합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* A-3: 입력 검증 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.snapshot.rollback
 *
 * ZFS 스냅샷을 복원하여 컨테이너를 이전 상태로 되돌립니다.
 * 컨테이너가 실행 중이면 lxc_driver 내부에서 중지 후 복원합니다.
 *
 * 파라미터: name* (컨테이너 이름), snap_name* (복원할 스냅샷 이름)
 * 응답: {"success": true}
 *
 * [주의] 롤백 후 스냅샷 이후에 생성된 데이터는 영구 삭제됩니다.
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* A-3: 입력 검증 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.snapshot.delete
 *
 * ZFS 스냅샷을 삭제합니다.
 * 해당 스냅샷에 의존하는 클론이 있으면 ZFS에서 에러가 발생합니다.
 *
 * 파라미터: name* (컨테이너 이름), snap_name* (삭제할 스냅샷 이름)
 * 응답: {"success": true}
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* A-3: 입력 검증 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.snapshot.list
 *
 * 컨테이너의 모든 ZFS 스냅샷 목록을 조회합니다.
 *
 * 파라미터: name* (컨테이너 이름)
 * 응답: { "result": ["snap1", "snap2", ...] }
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _on_snap_list_done:
 * pcv_lxc_snapshot_list_async() 완료 콜백.
 * GPtrArray(스냅샷 이름 목록)를 JSON 배열로 변환하여 응답합니다.
 */
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

    /* A-3: 입력 검증 */
    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    ContainerCtx *ctx = _ctx_new(name, rpc_id, server, conn);
    pcv_lxc_snapshot_list_async(name, NULL, _on_snap_list_done, ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * container.logs  (동기 조회 — 파일 tail)
 *
 * 컨테이너의 최근 로그 라인을 조회합니다.
 *
 * 파라미터: name* (컨테이너 이름), lines (정수, 기본 50, 최대 10000)
 * 응답: { "result": { "name":"myapp", "lines":["line1",...], "total":150 } }
 *
 * [로그 파일 탐색 순서]
 *   1. /var/lib/purecvisor/lxc/<name>/<name>.log
 *   2. /var/log/lxc/<name>.log
 *   3. 둘 다 없으면 -32000 에러 반환
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _tail_file — 파일 끝에서 N줄 읽기
 *
 * @param path    파일 경로
 * @param n_lines 읽을 줄 수
 * @param total   전체 줄 수 반환 (nullable)
 * @return GPtrArray<gchar*> — 마지막 n_lines줄, NULL이면 파일 열기 실패
 */
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
    /* 마지막 빈 줄 제거 (trailing newline) */
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

    /* 로그 파일 탐색 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.volume.attach  (동기 — bind mount + LXC config 영속)
 *
 * 파라미터: name*, host_path*, container_path*, readonly (bool, 기본 false)
 * 응답: {"success": true}
 *
 * [구현]
 *   1. host_path realpath 검증 (경로 순회 방지)
 *   2. 실행 중이면 host namespace에서 container rootfs 경로로 bind mount
 *   3. LXC config에 lxc.mount.entry 영속화
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* 경로 순회 방지: realpath로 host_path 정규화 */
    char resolved[PATH_MAX];
    if (!realpath(host_path, resolved)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "host_path does not exist or is invalid");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    /* container_path 검증: 절대 경로, '..' 금지 */
    if (container_path[0] != '/' || strstr(container_path, "..")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "container_path must be absolute with no '..'");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    /* 실행 중이면 host namespace에서 container rootfs 경로로 런타임 mount */
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

    /* LXC config에 영속화: lxc.mount.entry 추가 */
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
    /* container_path에서 선행 '/' 제거 (LXC mount.entry 규약) */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.volume.detach  (동기 — umount + LXC config 삭제)
 *
 * 파라미터: name*, container_path*
 * 응답: {"success": true}
 * ══════════════════════════════════════════════════════════════════════════*/

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

    /* 실행 중이면 host namespace에서 container rootfs 경로를 직접 umount */
    gchar *state = pcv_lxc_get_state(name);
    if (state && g_strcmp0(state, "RUNNING") == 0) {
        gchar *target_path = g_strdup_printf("%s/%s/rootfs%s", PCV_LXC_PATH, name, container_path);
        const gchar *umount_argv[] = {"umount", target_path, NULL};
        pcv_spawn_sync(umount_argv, NULL, NULL, NULL);
        g_free(target_path);
    }
    g_free(state);

    /* LXC config에서 해당 mount.entry 줄 제거 */
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
            /* lxc.mount.entry 줄에서 대상 경로 매칭하여 제거 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.volume.list  (동기 — LXC config 파싱)
 *
 * 파라미터: name*
 * 응답: { "result": [{"host_path":"/data","container_path":"/mnt/data","readonly":false}, ...] }
 * ══════════════════════════════════════════════════════════════════════════*/

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
            /* lxc.mount.entry = /host/path dest/path none bind[,ro] 0 0 */
            if (!g_str_has_prefix(lines[i], "lxc.mount.entry"))
                continue;
            gchar *eq = strchr(lines[i], '=');
            if (!eq) continue;
            gchar *val = g_strstrip(g_strdup(eq + 1));
            gchar **parts = g_strsplit(val, " ", 6);
            g_free(val);
            if (parts[0] && parts[1] && parts[2] && parts[3]) {
                /* parts[3]가 "bind" 또는 "bind,ro" */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.env.set  (동기 — LXC config에 lxc.environment 추가)
 *
 * 파라미터: name*, key*, value*
 * 응답: {"success": true, "note": "restart required"}
 *
 * [주의] 환경변수 변경은 컨테이너 재시작 후 적용됩니다 (LXC 제한).
 * ══════════════════════════════════════════════════════════════════════════*/

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
    /* 환경변수 키 검증: 알파뉴메릭 + '_', 공백/특수문자 금지 */
    for (const gchar *p = key; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_') {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                           "Invalid env key: alphanumeric and '_' only");
            pure_uds_server_send_response(server, conn, e); g_free(e); return;
        }
    }

    /* 기존 동일 키가 있으면 먼저 제거, 새 값 추가 */
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
                continue;  /* 기존 키 제거 */
            }
            g_free(trimmed);
            g_string_append(out, lines[i]);
            if (lines[i + 1]) g_string_append_c(out, '\n');
        }
        g_strfreev(lines);
        g_free(contents);
        contents = g_string_free(out, FALSE);
    }

    /* 새 환경변수 추가 */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.env.list  (동기 — LXC config에서 lxc.environment 파싱)
 *
 * 파라미터: name*
 * 응답: { "result": {"DATABASE_URL":"postgres://...", "NODE_ENV":"production"} }
 * ══════════════════════════════════════════════════════════════════════════*/

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
            /* lxc.environment = KEY=VALUE — 첫 번째 '='는 설정 구분, 두 번째가 실제 K=V */
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

/* ══════════════════════════════════════════════════════════════════════════
 * container.env.delete  (동기 — LXC config에서 lxc.environment 제거)
 *
 * 파라미터: name*, key*
 * 응답: {"success": true, "note": "restart required"}
 * ══════════════════════════════════════════════════════════════════════════*/

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


/* ══════════════════════════════════════════════════════════════════════════
 * Container Health Check Probes — TCP/HTTP/Exec 주기적 헬스 프로브
 *
 * [설계]
 *   - 최대 MAX_HEALTH_PROBES(32)개 프로브를 전역 배열에 보관
 *   - GTimeout(1초)으로 due 프로브를 평가 (pcv_spawn_sync 기반)
 *   - failure_threshold 초과 시 healthy=FALSE + auto_restart 시 lxc-stop/start
 *
 * [RPC 메서드]
 *   container.health.set    — 프로브 등록/갱신
 *   container.health.get    — 프로브 상태 조회
 *   container.health.delete — 프로브 삭제
 * ══════════════════════════════════════════════════════════════════════════*/

#define MAX_HEALTH_PROBES 32

typedef struct {
    gchar    name[64];             /* 컨테이너 이름 */
    gchar    probe_type[8];        /* "tcp", "http", "exec" */
    gchar    target[256];          /* "80" / "http://localhost/health" / "/bin/check.sh" */
    gint     timeout_sec;          /* 프로브 타임아웃 (초) */
    gint     interval_sec;         /* 검사 간격 (초) */
    gint     failure_threshold;    /* 연속 실패 임계값 */
    gboolean auto_restart;         /* 임계값 초과 시 자동 재시작 여부 */
    /* 런타임 상태 */
    gint     consecutive_failures; /* 현재 연속 실패 횟수 */
    gboolean healthy;              /* 현재 건강 상태 */
    gint     restart_count;        /* 자동 재시작 횟수 */
    gint64   last_check_time;      /* 마지막 검사 시각 (monotonic usec) */
} ContainerHealthProbe;

static ContainerHealthProbe g_health_probes[MAX_HEALTH_PROBES];
static gint g_n_health_probes = 0;
static GMutex g_health_mu;
static guint g_health_timer_id = 0;

/* 이름으로 프로브 인덱스 검색. 없으면 -1 반환 */
static gint _health_find(const gchar *ctr_name) {
    for (gint i = 0; i < g_n_health_probes; i++) {
        if (g_strcmp0(g_health_probes[i].name, ctr_name) == 0)
            return i;
    }
    return -1;
}

/* 1초마다 호출되는 타이머 콜백 — due 프로브 평가 */
static gboolean _health_check_tick(gpointer user_data) {
    (void)user_data;
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&g_health_mu);
    for (gint i = 0; i < g_n_health_probes; i++) {
        ContainerHealthProbe *p = &g_health_probes[i];
        if ((now - p->last_check_time) < (gint64)p->interval_sec * G_USEC_PER_SEC)
            continue;
        p->last_check_time = now;

        /* 컨테이너 PID 조회 (RUNNING 상태만 프로브 가능) */
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

/* container.health.set — 프로브 등록/갱신 */
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
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Required: name, type (tcp/http/exec), target");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }
    if (g_strcmp0(type, "tcp") != 0 && g_strcmp0(type, "http") != 0 &&
        g_strcmp0(type, "exec") != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "type must be tcp, http, or exec");
        pure_uds_server_send_response(server, conn, e); g_free(e); return;
    }

    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(cname);
    if (idx < 0) {
        if (g_n_health_probes >= MAX_HEALTH_PROBES) {
            g_mutex_unlock(&g_health_mu);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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
    if (p->interval_sec < 5) p->interval_sec = 5;  /* 최소 5초 */
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

/* container.health.get — 프로브 상태 조회 */
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
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

/* container.health.delete — 프로브 삭제 */
void handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!cname) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Required: name");
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
