/**
 * @file handler_snapshot.c
 * @brief VM ZFS 스냅샷 RPC 핸들러 — 생성/목록/롤백/삭제
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c -> handle_vm_snapshot_*()
 *                                              -> zfs_driver.c (zfs snapshot/rollback/destroy)
 *                                              -> virt_conn_pool.c (rollback 시 VM 중지/재기동)
 *
 * [처리하는 RPC 메서드] (4개)
 *   vm.snapshot.create   -> handle_vm_snapshot_create   : ZFS 스냅샷 생성
 *   vm.snapshot.list     -> handle_vm_snapshot_list     : 특정 VM의 스냅샷 목록 조회
 *   vm.snapshot.rollback -> handle_vm_snapshot_rollback : ZFS 스냅샷 복원 (VM 안전 중지 포함)
 *   vm.snapshot.delete   -> handle_vm_snapshot_delete   : ZFS 스냅샷 삭제
 *
 * [fire-and-forget 패턴]
 *   vm.snapshot.rollback만 fire-and-forget 비동기 패턴을 사용합니다:
 *     1. 즉시 {"status":"accepted"} 응답 전송 -> 소켓 닫힘
 *     2. GTask 워커 스레드에서 실행:
 *        a. VM 실행 중이면 virDomainDestroy (graceful 5초 -> force)
 *        b. zfs rollback -r 수행
 *        c. 원래 실행 중이었으면 virDomainCreate 재기동
 *     3. 결과는 로그에만 기록 (콜백에서 send_response 호출 금지)
 *   나머지 3개 메서드(create, list, delete)는 동기 응답입니다.
 *
 * [주의사항]
 *   - 파라미터 키를 이중 지원합니다:
 *     REST 레이어: "name" + "snapshot_name"
 *     UDS 레이어:  "vm_id" + "snap_name"
 *     두 형식 모두 자동 인식하여 처리합니다.
 *   - rollback 중 VM이 실행 상태이면 반드시 중지 후 복원합니다 (데이터 손상 방지).
 *   - ZFS zvol 경로는 pcv_config_get_zvol_pool()로 조회합니다 (기본값: pcvpool/vms).
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(vm_id/name, snap_name/snapshot_name) 누락
 *   -32000 : ZFS 명령 실행 실패
 */

#include "handler_snapshot.h"
#include "rpc_utils.h"
#include "../storage/zfs_driver.h"
#include "../../api/uds_server.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../modules/virt/virt_conn_pool.h"
#include "../audit/pcv_audit.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>
#include "modules/core/vm_state.h"   /* AF-P1: lock_vm_operation / VM_OP_SNAPSHOT */

#define SNAP_LOG_DOM "snapshot"

#define PCV_SYSTEM_SNAP_NS "pcv-"   /* 시스템 스냅샷 예약 네임스페이스 (STO-2/AF-S4) */

/* RpcAsyncContext / rpc_async_context_free — removed with dead callbacks (B5-W1 migration). */

/* ── ZFS 데이터셋 존재 여부 확인 (libvirt fallback 판단용) ── */
static gboolean _zfs_dataset_exists(const gchar *vm_id) {
    gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    const gchar *argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
    g_free(dataset);
    return ok;
}

/* ── libvirt 스냅샷 fallback: 생성 ── */
static void _libvirt_snapshot_create(const gchar *vm_id, const gchar *snap_name,
                                      const gchar *rpc_id, UdsServer *server,
                                      GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }
    gchar *xml = g_strdup_printf(
        "<domainsnapshot><name>%s</name><description>PureCVisor snapshot</description></domainsnapshot>",
        snap_name);
    virDomainSnapshotPtr snap = virDomainSnapshotCreateXML(dom, xml, 0);
    g_free(xml);

    if (snap) {
        pcv_audit_log(NULL, "vm.snapshot.create", vm_id, "ok", 0, 0, "local");
        virDomainSnapshotFree(snap);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        virErrorPtr verr = virGetLastError();
        pcv_audit_log(NULL, "vm.snapshot.create", vm_id, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            verr ? verr->message : "libvirt snapshot creation failed");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* ── libvirt 스냅샷 fallback: 목록 ── */
static void _libvirt_snapshot_list(const gchar *vm_id, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }

    char **names = NULL;
    int count = virDomainSnapshotNum(dom, 0);
    JsonArray *arr = json_array_new();

    if (count > 0) {
        names = g_new0(char *, count);
        count = virDomainSnapshotListNames(dom, names, count, 0);
        for (int i = 0; i < count; i++) {
            JsonObject *entry = json_object_new();
            json_object_set_string_member(entry, "snapshot", names[i]);
            virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, names[i], 0);
            if (snap) {
                gchar *xml = virDomainSnapshotGetXMLDesc(snap, 0);
                if (xml) {
                    /* <creationTime>epoch</creationTime> 추출 */
                    const gchar *ct = strstr(xml, "<creationTime>");
                    if (ct) {
                        gint64 epoch = g_ascii_strtoll(ct + 14, NULL, 10);
                        GDateTime *dt = g_date_time_new_from_unix_local(epoch);
                        if (dt) {
                            gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M");
                            json_object_set_string_member(entry, "creation", ts);
                            g_free(ts);
                            g_date_time_unref(dt);
                        }
                    }
                    free(xml);
                }
                virDomainSnapshotFree(snap);
            }
            json_array_add_object_element(arr, entry);
            free(names[i]);
        }
        g_free(names);
    }

    pcv_audit_log(NULL, "vm.snapshot.list", vm_id, "ok", 0, 0, "local");

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* ── libvirt 스냅샷 fallback: 삭제 ── */
static void _libvirt_snapshot_delete(const gchar *vm_id, const gchar *snap_name,
                                      const gchar *rpc_id, UdsServer *server,
                                      GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }
    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Snapshot not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    int rc = virDomainSnapshotDelete(snap, 0);
    virDomainSnapshotFree(snap);

    if (rc == 0) {
        pcv_audit_log(NULL, "vm.snapshot.delete", vm_id, "ok", 0, 0, "local");
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        pcv_audit_log(NULL, "vm.snapshot.delete", vm_id, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Failed to delete snapshot");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* ── libvirt 스냅샷 fallback: rollback ── */
static void _libvirt_snapshot_rollback(const gchar *vm_id, const gchar *snap_name,
                                        const gchar *rpc_id, UdsServer *server,
                                        GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }

    int state = 0, reason = 0;
    gboolean was_running = FALSE;
    virDomainGetState(dom, &state, &reason, 0);
    was_running = (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED);

    if (was_running) {
        virDomainShutdown(dom);
        for (int i = 0; i < 50; i++) {
            g_usleep(100 * 1000);
            virDomainGetState(dom, &state, &reason, 0);
            if (state != VIR_DOMAIN_RUNNING &&
                state != VIR_DOMAIN_PAUSED) break;
        }

        virDomainGetState(dom, &state, &reason, 0);
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED)
            virDomainDestroy(dom);
    }

    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Snapshot not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    int rc = virDomainRevertToSnapshot(snap, 0);
    virDomainSnapshotFree(snap);

    if (rc == 0 && was_running) {
        if (virDomainCreate(dom) != 0) {
            virErrorPtr verr = virGetLastError();
            PCV_LOG_WARN(SNAP_LOG_DOM,
                         "libvirt rollback restart failed for '%s': %s",
                         vm_id, verr ? verr->message : "unknown");
        }
    }

    if (rc == 0) {
        pcv_audit_log(NULL, "vm.snapshot.rollback", vm_id, "ok", 0, 0, "local");
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        virErrorPtr verr = virGetLastError();
        pcv_audit_log(NULL, "vm.snapshot.rollback", vm_id, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            verr ? verr->message : "Failed to rollback snapshot");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* ── 유효성 검사 ──────────────────────────────────────────── */
static gboolean pcv_validate_zfs_token(const gchar *s) {
    if (!s || *s == '\0' || strlen(s) > 128) return FALSE;
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}

/* 파라미터 키 이중 지원 헬퍼: REST ("name"/"snapshot_name") + 원래 ("vm_id"/"snap_name") */
static const gchar *_get_param(JsonObject *params,
                                const gchar *primary,
                                const gchar *fallback)
{
    if (json_object_has_member(params, primary))
        return json_object_get_string_member(params, primary);
    if (fallback && json_object_has_member(params, fallback))
        return json_object_get_string_member(params, fallback);
    return NULL;
}

#define VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, conn)                \
    do {                                                                        \
        const gchar *_vid = _get_param(params, "name", "vm_id");               \
        const gchar *_sn  = _get_param(params, "snapshot_name", "snap_name");  \
        if (!_vid || !_sn) {                                                    \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS,                            \
                "Missing vm name or snapshot_name");                            \
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
        if (!pcv_validate_zfs_token(_vid) || !pcv_validate_zfs_token(_sn)) {   \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS,                            \
                "Invalid characters in vm name or snapshot_name");              \
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
    } while (0)

#define VALIDATE_VM_ID_PARAM(params, rpc_id, server, conn)                     \
    do {                                                                        \
        const gchar *_vid = _get_param(params, "name", "vm_id");               \
        if (!_vid || !pcv_validate_zfs_token(_vid)) {                           \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing or invalid name");\
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
    } while (0)

/* run_zfs_subprocess — removed: dead code after B5-W1 GTask async migration. */

/* ═══════════════════════════════════════════════════════════════
 * Sprint F: 스냅샷 롤백 — 비동기 파이프라인
 *
 * GTask 워커 스레드에서 실행:
 *   1. virDomainGetState → 실행 중 여부 기록
 *   2. 실행 중이면 graceful shutdown (최대 5초) → force destroy
 *   3. zfs rollback -r <dataset>@<snap>
 *   4. 원래 실행 중이었으면 virDomainCreate
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar *vm_name;     /* VM 이름 */
    gchar *snap_name;   /* 스냅샷 이름 */
    /* RPC 응답 정보 */
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} RollbackTaskData;

static void _rollback_task_data_free(gpointer p)
{
    RollbackTaskData *d = (RollbackTaskData *)p;
    g_free(d->vm_name);
    g_free(d->snap_name);
    g_free(d->rpc_id);
    if (d->server)     g_object_unref(d->server);
    if (d->connection) g_object_unref(d->connection);
    g_free(d);
}

static void
_rollback_worker(GTask        *task,
                 gpointer      source __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancel __attribute__((unused)))
{
    RollbackTaskData *d = (RollbackTaskData *)task_data;
    GError *err = NULL;

    /* ── 1. libvirt 연결 + 도메인 조회 ─────────────────────── */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->vm_name);
    gboolean was_running = FALSE;

    if (dom) {
        /* ── 2. 실행 중 여부 확인 ───────────────────────────── */
        int state = 0, reason = 0;
        virDomainGetState(dom, &state, &reason, 0);
        was_running = (state == VIR_DOMAIN_RUNNING ||
                       state == VIR_DOMAIN_PAUSED);

        if (was_running) {
            PCV_LOG_INFO(SNAP_LOG_DOM,
                         "Rollback: shutting down VM '%s' before ZFS rollback",
                         d->vm_name);

            /* Graceful shutdown 시도 (최대 5초 대기) */
            virDomainShutdown(dom);
            for (int i = 0; i < 50; i++) {
                g_usleep(100 * 1000); /* 100ms */
                virDomainGetState(dom, &state, &reason, 0);
                if (state != VIR_DOMAIN_RUNNING &&
                    state != VIR_DOMAIN_PAUSED) break;
            }

            /* 아직 실행 중이면 강제 종료 */
            virDomainGetState(dom, &state, &reason, 0);
            if (state == VIR_DOMAIN_RUNNING ||
                state == VIR_DOMAIN_PAUSED) {
                PCV_LOG_WARN(SNAP_LOG_DOM,
                             "Rollback: graceful shutdown timeout — "
                             "force-destroying VM '%s'", d->vm_name);
                virDomainDestroy(dom);
            }
        }
        virDomainFree(dom);
    }
    /* dom == NULL: libvirt에 없는 VM — ZFS 롤백은 계속 진행 */

    /* ── 3. ZFS 롤백 ─────────────────────────────────────────── */
    gchar *dataset = g_strdup_printf("%s/%s@%s",
                                      pcv_config_get_zvol_pool(), d->vm_name, d->snap_name);
    PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS rollback: %s", dataset);

    const gchar *zfs_argv[] = {"zfs", "rollback", "-r", dataset, NULL};
    gchar *stderr_buf = NULL;
    gboolean zfs_ok = pcv_spawn_sync(zfs_argv, NULL, &stderr_buf, &err);
    g_free(dataset);

    if (!zfs_ok) {
        const gchar *errmsg = err ? err->message
                            : (stderr_buf ? stderr_buf : "ZFS rollback failed");
        PCV_LOG_WARN(SNAP_LOG_DOM, "ZFS rollback failed: %s", errmsg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "%s", errmsg);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        virt_conn_pool_release(conn);
        return;
    }
    g_free(stderr_buf);
    PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS rollback complete for '%s'", d->vm_name);

    /* ── 4. 원래 실행 중이었으면 재기동 ─────────────────────── */
    if (was_running) {
        dom = virDomainLookupByName(conn, d->vm_name);
        if (dom) {
            PCV_LOG_INFO(SNAP_LOG_DOM,
                         "Rollback: restarting VM '%s'", d->vm_name);
            if (virDomainCreate(dom) != 0) {
                virErrorPtr ve = virGetLastError();
                PCV_LOG_WARN(SNAP_LOG_DOM,
                             "Rollback: VM restart failed: %s",
                             ve ? ve->message : "unknown");
                /* 재기동 실패는 경고만 — ZFS 롤백은 성공했으므로 true 반환 */
            }
            virDomainFree(dom);
        }
    }

    virt_conn_pool_release(conn);
    g_task_return_boolean(task, TRUE);
}

static void
_on_rollback_done(GObject *src __attribute__((unused)),
                  GAsyncResult *res,
                  gpointer user_data)
{
    RollbackTaskData *d = (RollbackTaskData *)user_data;
    GError *err = NULL;
    gchar  *resp;
    gboolean _ok = g_task_propagate_boolean(G_TASK(res), &err);
    /* ADR-0018: 워커 결과 audit (target=vm:snap) */
    {
        gchar *_target = g_strdup_printf("%s:%s", d->vm_name ?: "?", d->snap_name ?: "?");
        pcv_audit_log(NULL, "vm.snapshot.rollback", _target,
                      _ok ? "ok" : "fail",
                      _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        g_free(_target);
    }
    if (_ok) {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        resp = pure_rpc_build_success_response(d->rpc_id, node);
        PCV_LOG_INFO(SNAP_LOG_DOM,
                     "Snapshot rollback OK: %s@%s",
                     d->vm_name, d->snap_name);
    } else {
        resp = pure_rpc_build_error_response(d->rpc_id,
                   PURE_RPC_ERR_ZFS_OPERATION,
                   err ? err->message : "Rollback failed");
        if (err) g_error_free(err);
    }

    pure_uds_server_send_response(d->server, d->connection, resp);
    g_free(resp);
    /* AF-P1: 핸들러가 잡은 VM_OP_SNAPSHOT 해제(async rollback 은 항상 락 보유). */
    unlock_vm_operation(d->vm_name);
    /* B5-M1: d는 GTask의 task_data destroy notify(_rollback_task_data_free)가 해제.
     * GLib 보장: callback은 task_data destroy 전에 실행 완료됨 — UAF 아님. */
}

/* on_snapshot_create_completed, on_snapshot_delete_completed, on_snapshot_list_completed,
 * run_zfs_subprocess, _bool_node — removed: dead code after B5-W1 GTask async migration.
 * All snapshot operations now use the GTask worker pattern below. */

/* ═══════════════════════════════════════════════════════════════
 * B5-W1: GTask 비동기 래퍼 — pcv_spawn_sync을 워커 스레드에서 실행하여
 * dispatcher 메인 루프 차단 방지. ZFS I/O가 느릴 때(scrub/degraded)
 * 전체 RPC 처리가 멈추는 것을 예방.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar    *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    GPtrArray *argv;          /* null-terminated string array */
    gboolean  capture_stdout;
    gchar    *audit_method;
    gchar    *audit_target;
    gchar    *lock_vm;        /* AF-P1: 설정 시 콜백에서 VM_OP_SNAPSHOT 해제 대상 VM (쓰기 op만; list 는 NULL) */
    /* 워커 → 콜백 결과 */
    gboolean  ok;
    gchar    *stdout_buf;
    gchar    *err_msg;
} SnapSyncCtx;

static void _snap_sync_ctx_free(gpointer p) {
    SnapSyncCtx *c = p;
    g_free(c->rpc_id); g_free(c->audit_method); g_free(c->audit_target);
    g_free(c->stdout_buf); g_free(c->err_msg); g_free(c->lock_vm);
    if (c->argv) g_ptr_array_unref(c->argv);
    if (c->server) g_object_unref(c->server);
    if (c->connection) g_object_unref(c->connection);
    g_free(c);
}

static void
_snap_sync_worker(GTask *task, gpointer src __attribute__((unused)),
                  gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    SnapSyncCtx *c = task_data;
    GError *error = NULL;
    gchar *stderr_buf = NULL;

    c->ok = pcv_spawn_sync((const gchar * const *)c->argv->pdata,
                            c->capture_stdout ? &c->stdout_buf : NULL,
                            &stderr_buf, &error);

    /* ADR-0018-audit: vm.snapshot.create, vm.snapshot.list, vm.snapshot.delete
     * (method 이름이 변수로 전달되므로 정적 분석용 annotation 명시) */
    if (c->audit_method) {
        pcv_audit_log(NULL, c->audit_method, c->audit_target ?: "",
                      c->ok ? "ok" : "fail",
                      c->ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    }
    if (!c->ok) {
        c->err_msg = g_strdup(error ? error->message
                              : (stderr_buf ? stderr_buf : "Unknown ZFS error"));
    }
    g_free(stderr_buf);
    if (error) g_error_free(error);
    g_task_return_boolean(task, TRUE);
}

static void
_snap_sync_callback(GObject *src __attribute__((unused)),
                    GAsyncResult *res __attribute__((unused)),
                    gpointer user_data)
{
    SnapSyncCtx *c = user_data;
    gchar *resp;

    if (!c->ok) {
        resp = pure_rpc_build_error_response(c->rpc_id,
                   PURE_RPC_ERR_ZFS_OPERATION, c->err_msg);
    } else if (c->capture_stdout && c->stdout_buf) {
        /* B5-M3: stdout → 구조화된 JSON 배열 (탭 구분 파싱) */
        JsonArray *arr  = json_array_new();
        gchar    **lines = g_strsplit(g_strstrip(c->stdout_buf), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            gchar **cols = g_strsplit(*l, "\t", -1);
            guint ncols = g_strv_length(cols);
            if (ncols >= 2) {
                /* "name\tcreation" → {"snapshot":"...", "creation":"..."} */
                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "snapshot", cols[0]);
                json_object_set_string_member(entry, "creation", cols[1]);
                json_array_add_object_element(arr, entry);
            } else if (ncols == 1) {
                /* fallback: 단일 컬럼이면 plain string */
                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "snapshot", cols[0]);
                json_array_add_object_element(arr, entry);
            }
            g_strfreev(cols);
        }
        g_strfreev(lines);

        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, arr);
        resp = pure_rpc_build_success_response(c->rpc_id, node);
    } else {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        resp = pure_rpc_build_success_response(c->rpc_id, node);
    }

    pure_uds_server_send_response(c->server, c->connection, resp);
    g_free(resp);
    /* AF-P1: 쓰기 op(create/delete)가 잡은 VM_OP_SNAPSHOT 해제. list 는 lock_vm=NULL. */
    if (c->lock_vm) unlock_vm_operation(c->lock_vm);
    /* c는 GTask task_data destroy notify가 해제 */
}

/* argv 헬퍼: GPtrArray에 문자열 복사 추가 (NULL 종료 보장) */
static GPtrArray *_argv_new(void) {
    return g_ptr_array_new_with_free_func(g_free);
}
static void _argv_add(GPtrArray *a, const gchar *s) {
    g_ptr_array_add(a, g_strdup(s));
}
static void _argv_finish(GPtrArray *a) {
    g_ptr_array_add(a, NULL);  /* sentinel */
}

static void _snap_sync_dispatch(SnapSyncCtx *c) {
    GTask *task = g_task_new(NULL, NULL, _snap_sync_callback, c);
    g_task_set_task_data(task, c, _snap_sync_ctx_free);
    g_task_run_in_thread(task, _snap_sync_worker);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════════════
 * 공개 핸들러
 * ═══════════════════════════════════════════════════════════════ */

void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_id     = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");

    /* STO-2 (AF-S4): "pcv-" 네임스페이스는 시스템 스냅샷 예약. pcv-auto-/pcv-s3-/
     * pcv-incr- 는 리텐션 prune(zfs destroy) 대상이므로 사용자가 이 접두로 만들면
     * 우연히 파괴될 수 있다. 시스템 생성자는 zfs snapshot 을 직접 호출(이 RPC 미경유)
     * 하므로 이 검사는 데몬 스냅샷을 절대 막지 않는다. vm.snapshot.create 는 async
     * 메서드라 디스패처가 자동 audit 하지 않으므로(g_async_methods) 조기 거부도
     * 직접 fail audit 한다(backup.snapshot.verify 조기검증 패턴과 동일). */
    if (g_str_has_prefix(snap_name, PCV_SYSTEM_SNAP_NS)) {
        pcv_audit_log(NULL, "vm.snapshot.create", vm_id, "fail",
                      PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS,
            "Snapshot name prefix '" PCV_SYSTEM_SNAP_NS "' is reserved for system snapshots");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* libvirt fallback: ZFS 데이터셋이 없으면 libvirt 스냅샷 API 사용 (qcow2 등) */
    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot", vm_id);
        _libvirt_snapshot_create(vm_id, snap_name, rpc_id, server, connection);
        return;
    }

    /* B5-W2: Snapshot quota — ZFS-level count (libvirt 카운트는 ZFS 스냅샷과 불일치) */
    {
        gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
        if (!purecvisor_zfs_check_snapshot_quota(dataset, 50)) {
            g_free(dataset);
            gchar *e = pure_rpc_build_error_response(rpc_id,
                PURE_RPC_ERR_ZFS_OPERATION,
                "Snapshot quota exceeded: maximum 50 snapshots per VM");
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            return;
        }
        g_free(dataset);
    }

    /* AF-P1: ZFS async 경로만 VM_OP_SNAPSHOT 락(동기 libvirt/quota 출구는 메인루프
     * 단일스레드라 이미 직렬화 → 락 불필요). 해제는 _snap_sync_callback 이 c->lock_vm
     * 로 수행. rollback 이 내부 정지/재기동을 virDomain* 직접호출로 하므로
     * VM_OP_STOPPING/STARTING 재획득이 없어 자기충돌/데드락 없음. */
    {
        gchar *snap_lock_err = NULL;
        if (!lock_vm_operation(vm_id, VM_OP_SNAPSHOT, &snap_lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                           snap_lock_err ? snap_lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(snap_lock_err);
            return;
        }
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->lock_vm      = g_strdup(vm_id);   /* AF-P1: 콜백에서 unlock */
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = FALSE;
    c->audit_method = g_strdup("vm.snapshot.create");
    c->audit_target = g_strdup_printf("%s:%s", vm_id, snap_name);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "snapshot");
    gchar *ds = g_strdup_printf("%s/%s@%s", pcv_config_get_zvol_pool(), vm_id, snap_name);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}

void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_VM_ID_PARAM(params, rpc_id, server, connection);
    const gchar *vm_id = _get_param(params, "name", "vm_id");

    /* libvirt fallback: ZFS 데이터셋이 없으면 libvirt 스냅샷 API 사용 */
    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot list", vm_id);
        _libvirt_snapshot_list(vm_id, rpc_id, server, connection);
        return;
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = TRUE;
    c->audit_method = g_strdup("vm.snapshot.list");
    c->audit_target = g_strdup(vm_id);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "list");
    _argv_add(c->argv, "-H"); _argv_add(c->argv, "-o");
    _argv_add(c->argv, "name,creation");
    _argv_add(c->argv, "-t"); _argv_add(c->argv, "snapshot");
    _argv_add(c->argv, "-r");
    gchar *ds = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}

/* Sprint F: 완전 재구현 — 비동기 정지→롤백→재기동 파이프라인 */
void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_name   = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");

    /* qcow2 등 ZFS 데이터셋이 없는 VM은 libvirt snapshot rollback으로 처리한다. */
    if (!_zfs_dataset_exists(vm_name)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot rollback", vm_name);
        _libvirt_snapshot_rollback(vm_name, snap_name, rpc_id, server, connection);
        return;
    }

    /* AF-P1: async rollback 경로만 VM_OP_SNAPSHOT 락. 해제는 _on_rollback_done.
     * rollback 내부 정지→롤백→재기동은 virDomain* 직접호출(락 획득 RPC 아님)이라
     * 자기충돌 없음. */
    {
        gchar *snap_lock_err = NULL;
        if (!lock_vm_operation(vm_name, VM_OP_SNAPSHOT, &snap_lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                           snap_lock_err ? snap_lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(snap_lock_err);
            return;
        }
    }

    RollbackTaskData *d = g_new0(RollbackTaskData, 1);
    d->vm_name   = g_strdup(vm_name);
    d->snap_name = g_strdup(snap_name);
    d->rpc_id    = g_strdup(rpc_id);
    d->server    = g_object_ref(server);
    d->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _on_rollback_done, d);
    g_task_set_task_data(task, d, _rollback_task_data_free);
    g_task_run_in_thread(task, _rollback_worker);
    g_object_unref(task);
}

void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_id     = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");

    /* libvirt fallback: ZFS 데이터셋이 없으면 libvirt 스냅샷 API 사용 */
    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot delete", vm_id);
        _libvirt_snapshot_delete(vm_id, snap_name, rpc_id, server, connection);
        return;
    }

    /* AF-P1: ZFS async 삭제 경로만 VM_OP_SNAPSHOT 락. 해제는 _snap_sync_callback. */
    {
        gchar *snap_lock_err = NULL;
        if (!lock_vm_operation(vm_id, VM_OP_SNAPSHOT, &snap_lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                           snap_lock_err ? snap_lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(snap_lock_err);
            return;
        }
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->lock_vm      = g_strdup(vm_id);   /* AF-P1: 콜백에서 unlock */
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = FALSE;
    c->audit_method = g_strdup("vm.snapshot.delete");
    c->audit_target = g_strdup_printf("%s:%s", vm_id, snap_name);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "destroy");
    gchar *ds = g_strdup_printf("%s/%s@%s", pcv_config_get_zvol_pool(), vm_id, snap_name);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}

/* B5-C2 (Phase 2 fix): GTask 워커로 분리하여 dispatcher 메인 루프 차단 회피.
 * 5000개 스냅샷 순차 zfs destroy = 500초+ → 메인 루프 블록 → 모든 RPC 응답 지연.
 * 워커 스레드에서 실행하면 다른 RPC는 영향받지 않음. 응답은 콜백에서 전송. */
typedef struct {
    gchar *vm_id;
    gchar *prefix;
    gint   keep;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    /* 결과 (워커 → 콜백) */
    gint   total;
    gint   to_delete;
    gint   del_count;
    gchar *err_msg;
} DeleteAllCtx;

static void _delete_all_ctx_free(gpointer p) {
    DeleteAllCtx *d = p;
    g_free(d->vm_id); g_free(d->prefix); g_free(d->rpc_id); g_free(d->err_msg);
    if (d->server) g_object_unref(d->server);
    if (d->connection) g_object_unref(d->connection);
    g_free(d);
}

static void
_delete_all_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    DeleteAllCtx *d = task_data;

    /* 1. 스냅샷 목록 조회 */
    gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), d->vm_id);
    const gchar *list_argv[] = {"zfs", "list", "-H", "-o", "name",
                                 "-t", "snapshot", "-s", "creation", "-r", dataset, NULL};
    gchar *out = NULL, *err_out = NULL;
    GError *err = NULL;
    if (!pcv_spawn_sync(list_argv, &out, &err_out, &err)) {
        d->err_msg = g_strdup(err ? err->message : "Failed to list snapshots");
        g_free(dataset); g_free(out); g_free(err_out);
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }
    g_free(dataset);

    /* 2. 필터링 */
    gchar **lines = g_strsplit(out, "\n", -1);
    g_free(out); g_free(err_out);
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        if (d->prefix && d->prefix[0]) {
            const gchar *at = strrchr(lines[i], '@');
            if (!at || !g_str_has_prefix(at + 1, d->prefix)) continue;
        }
        g_ptr_array_add(targets, g_strdup(lines[i]));
    }
    g_strfreev(lines);

    /* 3. keep_recent 적용 */
    d->total = (gint)targets->len;
    d->to_delete = (d->keep > 0) ? (d->total > d->keep ? d->total - d->keep : 0) : d->total;
    d->del_count = 0;
    for (gint i = 0; i < d->to_delete && i < d->total; i++) {
        const gchar *snap = g_ptr_array_index(targets, i);
        const gchar *del_argv[] = {"zfs", "destroy", snap, NULL};
        gchar *del_stderr = NULL; GError *del_err = NULL;
        if (pcv_spawn_sync(del_argv, NULL, &del_stderr, &del_err)) {
            d->del_count++;
        } else {
            /* B5-M2: 개별 실패 진단 로그 */
            PCV_LOG_WARN(SNAP_LOG_DOM, "zfs destroy failed: %s — %s",
                         snap, del_err ? del_err->message
                                       : (del_stderr ?: "unknown"));
        }
        g_free(del_stderr);
        if (del_err) g_error_free(del_err);
    }
    g_ptr_array_unref(targets);
    g_task_return_boolean(task, TRUE);
}

static void
_delete_all_callback(GObject *src __attribute__((unused)),
                     GAsyncResult *res, gpointer user_data)
{
    DeleteAllCtx *d = user_data;
    GError *err = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);

    /* AF-P1: 핸들러가 잡은 VM_OP_SNAPSHOT 해제 — 성공/실패 양 return 경로 최우선. */
    unlock_vm_operation(d->vm_id);

    if (!ok || d->err_msg) {
        gchar *resp = pure_rpc_build_error_response(d->rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            d->err_msg ? d->err_msg : (err ? err->message : "delete_all failed"));
        pure_uds_server_send_response(d->server, d->connection, resp);
        g_free(resp);
        pcv_audit_log(NULL, "vm.snapshot.delete_all", d->vm_id, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        if (err) g_error_free(err);
        return;
    }

    /* 응답 전송 */
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "deleted", d->del_count);
    json_object_set_int_member(obj, "total_before", d->total);
    json_object_set_int_member(obj, "remaining", d->total - d->del_count);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(d->rpc_id, node);
    pure_uds_server_send_response(d->server, d->connection, resp);
    g_free(resp);

    /* ADR-0018 partial_fail 정확 기록 */
    const gchar *audit_result;
    gint audit_code;
    if (d->to_delete == 0 || d->del_count == d->to_delete) {
        audit_result = "ok"; audit_code = 0;
    } else if (d->del_count == 0) {
        audit_result = "fail"; audit_code = PURE_RPC_ERR_ZFS_OPERATION;
    } else {
        audit_result = "partial_fail"; audit_code = PURE_RPC_ERR_ZFS_OPERATION;
    }
    pcv_audit_log(NULL, "vm.snapshot.delete_all", d->vm_id,
                  audit_result, audit_code, 0, "local");

    PCV_LOG_INFO(SNAP_LOG_DOM, "Bulk delete: vm=%s prefix=%s keep=%d deleted=%d/%d",
                 d->vm_id, d->prefix ?: "*", d->keep, d->del_count, d->total);
}

void handle_vm_snapshot_delete_all(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_VM_ID_PARAM(params, rpc_id, server, connection);
    const gchar *vm_id = _get_param(params, "name", "vm_id");
    const gchar *prefix = json_object_has_member(params, "prefix")
        ? json_object_get_string_member(params, "prefix") : NULL;
    gint keep = json_object_has_member(params, "keep_recent")
        ? (gint)json_object_get_int_member(params, "keep_recent") : 0;

    /* B5-W3 (Phase 4): 비존재 VM을 명확히 구분 — libvirt에서 VM 존재 확인.
     * 이전엔 zfs list가 빈 결과로 성공(deleted=0) 반환 → "VM 있는데 스냅샷 0개"와 구분 불가. */
    {
        virConnectPtr qconn = virt_conn_pool_acquire();
        if (qconn) {
            virDomainPtr dom = virDomainLookupByName(qconn, vm_id);
            if (!dom) {
                virt_conn_pool_release(qconn);
                gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                    "VM not found — cannot bulk delete snapshots for non-existent VM");
                pure_uds_server_send_response(server, connection, err);
                g_free(err);
                return;
            }
            virDomainFree(dom);
            virt_conn_pool_release(qconn);
        }
    }

    /* AF-P1: async bulk-delete 경로 VM_OP_SNAPSHOT 락. 해제는 _delete_all_callback. */
    {
        gchar *snap_lock_err = NULL;
        if (!lock_vm_operation(vm_id, VM_OP_SNAPSHOT, &snap_lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                           snap_lock_err ? snap_lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(snap_lock_err);
            return;
        }
    }

    DeleteAllCtx *d = g_new0(DeleteAllCtx, 1);
    d->vm_id = g_strdup(vm_id);
    d->prefix = g_strdup(prefix);
    d->keep = keep;
    d->rpc_id = g_strdup(rpc_id);
    d->server = g_object_ref(server);
    d->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _delete_all_callback, d);
    g_task_set_task_data(task, d, _delete_all_ctx_free);
    g_task_run_in_thread(task, _delete_all_worker);
    g_object_unref(task);
}
