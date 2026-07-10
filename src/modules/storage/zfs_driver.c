/**
 * @file zfs_driver.c
 * @brief ZFS 스토리지 드라이버 — 스냅샷 CRUD + zvol 프로비저닝
 *
 * == 아키텍처에서의 위치 ==
 *   handler_snapshot.c → zfs_driver (스냅샷 create/rollback/delete/list)
 *   vm_manager.c       → zfs_driver (zvol create/destroy)
 *
 * == 비전공자 설명 ==
 * ZFS는 디스크 공간을 단순 폴더가 아니라 "스냅샷과 복제 기능을 가진 저장소"로
 * 관리합니다. PureCVisor의 기본 VM 디스크는 ZFS zvol이며, VM 입장에서는
 * 일반 하드디스크처럼 보이지만 운영자는 ZFS 명령으로 빠른 스냅샷/롤백/삭제를
 * 수행할 수 있습니다. 따라서 이 파일의 삭제·롤백 코드는 사용자 데이터에 직접
 * 영향을 주는 고위험 경로입니다.
 *
 * == 주니어 참고 — zvol vs qcow2 차이 ==
 *   [zvol (ZFS Volume)]
 *     ZFS가 직접 관리하는 블록 디바이스. /dev/zvol/pcvpool/vms/<vm> 경로.
 *     장점: ZFS 스냅샷/복제/압축/암호화 네이티브 지원, CoW 기반 빠른 스냅샷
 *     단점: ZFS 풀 필수, 블록 디바이스이므로 sparse(thin) 용도 시 -s 플래그 필요
 *     용도: PureCVisor 기본 스토리지 (pcvpool/vms/ 하위)
 *
 *   [qcow2 (QEMU Copy-on-Write)]
 *     일반 파일시스템 위의 가상 디스크 이미지 파일.
 *     장점: 어떤 파일시스템에서든 동작, thin provisioning 자동 지원
 *     단점: ZFS 스냅샷/복제 불가, 파일 레벨 I/O 오버헤드
 *     용도: ZFS 풀이 없는 환경에서 폴백 (daemon.conf [storage] image_dir)
 *
 *   [raw]
 *     가공 없는 디스크 이미지. qcow2보다 약간 빠르지만 thin provisioning 불가.
 *
 *   vm.create의 storage_type 파라미터로 선택 가능 (zvol/qcow2/raw).
 *   미지정 시 ZFS 풀 감지 → zvol 우선, ZFS 없으면 qcow2 폴백.
 *
 * == 주니어 참고 — thin provisioning (-s 플래그) ==
 *   purecvisor_zfs_create_volume()에서 `zfs create -s -V 50G` 처럼 -s를 사용합니다.
 *   -s (sparse): 논리적으로 50GB이지만 실제로는 사용한 만큼만 디스크 점유.
 *   예: 50GB zvol 생성 시 실제 점유량 = 0MB (데이터 쓸 때마다 증가).
 *   -s 없으면 50GB를 즉시 할당 → 디스크 낭비, 생성도 느림.
 *
 * == 주니어 참고 — 스냅샷 쿼터 (v1.0) ==
 *   handler_snapshot.c에서 VM당 스냅샷 최대 개수를 제한합니다.
 *   기본 64개 (PCV_SNAPSHOT_MAX_PER_VM).
 *   초과 시 가장 오래된 스냅샷을 자동 삭제하거나 에러를 반환합니다.
 *
 * == 주니어 참고 — 암호화 zvol 생성 (v1.0) ==
 *   vm.create의 encrypted=true 파라미터로 AES-256-GCM 암호화 zvol 생성 가능.
 *   zfs create -o encryption=aes-256-gcm -o keylocation=prompt -o keyformat=passphrase
 *   암호화 키는 daemon.conf [storage] encryption_key에서 읽어 사용.
 *
 * == 주니어 참고 — zfs promote의 용도 ==
 *   zfs clone으로 생성된 클론은 원본 스냅샷에 의존합니다.
 *   원본을 삭제하려면 먼저 zfs promote <clone>으로 클론을 독립시켜야 합니다.
 *   promote 후 클론이 새 원본이 되고, 기존 원본이 의존 관계로 전환됩니다.
 *
 * == 비동기 스냅샷 API ==
 *   GSubprocess + GTask 기반으로 zfs 명령을 비동기 실행합니다.
 *   - 30초 타임아웃: GTask에 타이머를 등록하여 hang 감지 시 GCancellable로 kill
 *   - on_zfs_command_ready: 공통 완료 콜백 (에러/성공 분기)
 *   - on_zfs_list_ready: 리스트 전용 콜백 (stdout 파싱 → 스냅샷 이름 배열)
 *
 * == 동기 zvol API ==
 *   purecvisor_zfs_create_volume / destroy_volume은 pcv_spawn_sync 기반 동기 함수입니다.
 *   vm_manager.c의 GTask 워커 스레드에서 호출되므로 GMainLoop를 블로킹하지 않습니다.
 *
 * == ZFS 명령 형식 ==
 *   스냅샷: zfs snapshot pcvpool/vms/<vm>@<snap>
 *   롤백:   zfs rollback -r pcvpool/vms/<vm>@<snap>  (-r: 이후 스냅샷 강제 삭제)
 *   삭제:   zfs destroy pcvpool/vms/<vm>@<snap>
 *   목록:   zfs list -t snapshot -H -o name pcvpool/vms/<vm>
 *   볼륨:   zfs create -V <size>G pcvpool/vms/<vm>
 *   볼륨삭제: zfs destroy -r pcvpool/vms/<vm>  (-r: 하위 스냅샷 포함)
 *
 * == ZfsOpCtx ==
 *   각 비동기 작업에 GCancellable + 타임아웃 ID를 묶어 관리합니다.
 *   작업 완료 시 타이머를 해제하고, 타임아웃 시 cancellable을 취소합니다.
 */

#include "modules/storage/zfs_driver.h"
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <time.h>
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_zfs_lock.h"   /* BUG-18 F-1: pool 수준 직렬화 락 */
#include "../../utils/pcv_config.h"
#include "../daemons/prometheus_exporter.h"
#if PCV_CLUSTER_ENABLED
#include "../cluster/cluster_manager.h"
#include "../cluster/etcd_client.h"
#endif

#if PCV_CLUSTER_ENABLED
static gdouble
_elapsed_ms_since(gint64 start_us)
{
    return (gdouble)(g_get_monotonic_time() - start_us) / 1000.0;
}

static gboolean
_zfs_acquire_inflight_lock_with_metrics(PcvEtcdClient *etcd,
                                        const gchar *pool_name,
                                        const gchar *node_name,
                                        const gchar *op,
                                        gint ttl_sec)
{
    gint64 start_us = g_get_monotonic_time();
    if (!etcd) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "error", 0.0);
        return FALSE;
    }

    GError *dist_err = NULL;
    if (pcv_etcd_acquire_inflight_lock(etcd, pool_name, node_name,
                                       op, ttl_sec, &dist_err)) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "ok",
                                           _elapsed_ms_since(start_us));
        return TRUE;
    }

    gboolean first_error = (dist_err != NULL);
    if (dist_err) g_error_free(dist_err);

    /* 다른 노드가 작업 중일 수 있으므로 5초 후 1회 재시도한다. */
    g_usleep(5 * G_USEC_PER_SEC);
    GError *retry_err = NULL;
    if (pcv_etcd_acquire_inflight_lock(etcd, pool_name, node_name,
                                       op, ttl_sec, &retry_err)) {
        pcv_prom_zfs_inflight_lock_observe(pool_name, op, "ok",
                                           _elapsed_ms_since(start_us));
        return TRUE;
    }

    pcv_prom_zfs_inflight_lock_observe(pool_name, op,
        (first_error || retry_err) ? "error" : "busy",
        _elapsed_ms_since(start_us));
    if (retry_err) g_error_free(retry_err);
    return FALSE;
}
#endif

/* =========================================================================
 * [비동기 안전장치] 타임아웃 및 컨텍스트 관리 구조체 (Private)
 *
 * 모든 비동기 ZFS 작업은 ZfsOpCtx를 GTask의 task_data로 보유합니다.
 * GCancellable: 타임아웃 또는 외부 취소 시 GSubprocess를 강제 종료
 * timeout_id:   GMainLoop 타이머 소스 ID — 작업 완료 시 해제 필요
 * ========================================================================= */
typedef struct {
    GCancellable *cancellable;  /* 비동기 작업 취소용 — 타임아웃 시 cancel() 호출 */
    guint timeout_id;           /* g_timeout_add_seconds()가 반환한 타이머 ID */
} ZfsOpCtx;

/**
 * zfs_op_ctx_free:
 * GTask의 task_data destroy 콜백으로 등록됩니다.
 * 타이머가 아직 동작 중이면 제거하고, GCancellable 참조를 해제합니다.
 *
 * @param data ZfsOpCtx* (gpointer로 캐스팅됨)
 */
static void zfs_op_ctx_free(gpointer data) {
    if (!data) return;
    ZfsOpCtx *ctx = (ZfsOpCtx *)data;
    /* 타이머가 아직 살아있으면 GMainLoop에서 제거 — 이중 fire 방지 */
    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
    }
    if (ctx->cancellable) {
        g_object_unref(ctx->cancellable);
    }
    g_free(ctx);
}

/**
 * on_zfs_timeout:
 * ZFS 명령이 30초 내에 완료되지 않을 때 호출되는 타이머 콜백입니다.
 * GCancellable을 취소하여 GSubprocess에 SIGTERM을 전달합니다.
 *
 * @param user_data GTask* — ctx를 꺼내어 cancellable에 취소 신호 전송
 * @return G_SOURCE_REMOVE — 일회성 타이머 (재실행 안 함)
 */
static gboolean on_zfs_timeout(gpointer user_data) {
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);

    g_warning("[ZFS Driver] Process hang detected! Sending kill signal...");
    g_cancellable_cancel(ctx->cancellable);
    ctx->timeout_id = 0;  /* 타이머 해제됨 표시 — zfs_op_ctx_free에서 이중 제거 방지 */

    return G_SOURCE_REMOVE;
}

/**
 * on_zfs_command_ready:
 * GSubprocess의 communicate_utf8_async 완료 콜백 (create/rollback/delete 공용).
 *
 * 처리 흐름:
 *   1. communicate_utf8 결과 수신 (stdout/stderr)
 *   2. 타이머가 아직 있으면 해제 (정상 완료 시)
 *   3. 에러 분기:
 *      - GError 발생 → task에 에러 전파 (타임아웃/취소 포함)
 *      - 프로세스 exit code != 0 → stderr 메시지로 에러 생성
 *      - 성공 → g_task_return_boolean(TRUE)
 *   4. stdout/stderr 버퍼 해제 후 task unref
 *
 * @param source_object GSubprocess*
 * @param res           비동기 결과
 * @param user_data     GTask* (이 콜백이 task의 최종 소유자 → unref 수행)
 */
static void on_zfs_command_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);
    GError *error = NULL;

    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, &stderr_buf, &error);

    /* 정상 완료 시 타이머 해제 — 이미 fire된 경우 timeout_id는 0 */
    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    if (error != NULL) {
        /* GCancellable 취소(타임아웃) 또는 프로세스 스폰 실패 */
        g_task_return_error(task, error);
    } else if (!g_subprocess_get_successful(proc)) {
        /* zfs 명령이 0이 아닌 exit code로 종료 — stderr에 원인 메시지 있음 */
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS failure");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Error: %s", clean_err);
        g_free(clean_err);
    } else {
        g_task_return_boolean(task, TRUE);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    g_object_unref(task);  /* 이 콜백이 task의 마지막 참조 — 여기서 해제 */
}

/* =========================================================================
 * 공통 헬퍼: ZFS 비동기 명령 실행 (create/rollback/delete 공용)
 * ========================================================================= */
static void
_zfs_async_command(const gchar * const *argv,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    ZfsOpCtx *ctx = g_new0(ZfsOpCtx, 1);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : g_cancellable_new();
    g_task_set_task_data(task, ctx, zfs_op_ctx_free);

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);

    if (!proc || error) {
        if (!error)
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to spawn ZFS process");
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    ctx->timeout_id = g_timeout_add_seconds(30, on_zfs_timeout, task);
    g_subprocess_communicate_utf8_async(proc, NULL, ctx->cancellable, on_zfs_command_ready, task);
    g_object_unref(proc);
}

/* =========================================================================
 * 1. 스냅샷 생성 (Create Snapshot)
 *    명령: zfs snapshot <pool>/vms/<vm>@<snap>
 * ========================================================================= */

/**
 * purecvisor_zfs_snapshot_create_async:
 * ZFS 스냅샷을 비동기로 생성합니다. 30초 타임아웃이 적용됩니다.
 *
 * @param pool_name   ZFS 풀 이름 (예: "pcvpool")
 * @param vm_name     VM 이름 (예: "web-prod") — 풀 내 데이터셋 이름과 동일
 * @param snap_name   스냅샷 이름 (예: "daily-20260325")
 * @param cancellable 외부 취소 핸들 (NULL 가능 — 내부에서 자동 생성)
 * @param callback    완료 시 호출될 GAsyncReadyCallback
 * @param user_data   콜백에 전달할 사용자 데이터
 */
void purecvisor_zfs_snapshot_create_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "snapshot", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

/**
 * purecvisor_zfs_snapshot_create_finish:
 * 스냅샷 생성 비동기 작업의 결과를 회수합니다.
 *
 * @param res   GAsyncResult (GTask)
 * @param error 에러 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패 (error에 원인 설정됨)
 */
gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* =========================================================================
 * 2. 스냅샷 롤백 (Rollback Snapshot)
 *    명령: zfs rollback -r <pool>/vms/<vm>@<snap>
 *    -r 플래그: 해당 스냅샷 이후의 모든 스냅샷을 강제 삭제 후 롤백
 *    주의: VM이 실행 중이면 롤백 불가 — 사전에 VM 정지 필요
 * ========================================================================= */

/**
 * purecvisor_zfs_snapshot_rollback_async:
 * ZFS 스냅샷을 비동기로 롤백합니다. -r 플래그로 이후 스냅샷을 모두 삭제합니다.
 *
 * @param pool_name   ZFS 풀 이름
 * @param vm_name     VM 이름
 * @param snap_name   롤백 대상 스냅샷 이름
 * @param cancellable 외부 취소 핸들 (NULL 가능)
 * @param callback    완료 콜백
 * @param user_data   콜백 사용자 데이터
 */
void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "rollback", "-r", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

/** @see purecvisor_zfs_snapshot_create_finish — 동일한 패턴 */
gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* =========================================================================
 * 3. 스냅샷 삭제 (Delete Snapshot)
 *    명령: zfs destroy <pool>/vms/<vm>@<snap>
 *    멱등성: 이미 없는 스냅샷 삭제 시 zfs가 에러 반환 → 호출자에서 처리
 * ========================================================================= */

/**
 * purecvisor_zfs_snapshot_delete_async:
 * 지정된 ZFS 스냅샷을 비동기로 삭제합니다.
 *
 * @param pool_name   ZFS 풀 이름
 * @param vm_name     VM 이름
 * @param snap_name   삭제할 스냅샷 이름
 * @param cancellable 외부 취소 핸들 (NULL 가능)
 * @param callback    완료 콜백
 * @param user_data   콜백 사용자 데이터
 */
void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "destroy", target, NULL};
    _zfs_async_command((const gchar * const *)argv, cancellable, callback, user_data);
    g_free(target);
}

/** @see purecvisor_zfs_snapshot_create_finish — 동일한 패턴 */
gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* =========================================================================
 * 4. 스냅샷 목록 조회 (List Snapshots)
 *    명령: zfs list -t snapshot -H -o name <pool>/vms/<vm>
 *    -H: 헤더 없음, -o name: 이름 컬럼만 출력
 *    출력 예: pcvpool/vms/web-prod@daily-20260325
 *    @ 이후 문자열만 추출하여 스냅샷 이름 배열로 반환
 * ========================================================================= */

/**
 * on_zfs_list_ready:
 * zfs list 명령의 비동기 완료 콜백입니다.
 * stdout을 줄 단위로 파싱하여 '@' 이후의 스냅샷 이름만 추출합니다.
 *
 * 결과: GPtrArray<gchar*> — 스냅샷 이름 배열 (호출자가 g_ptr_array_unref로 해제)
 *
 * @param source_object GSubprocess*
 * @param res           비동기 결과
 * @param user_data     GTask*
 */
static void on_zfs_list_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    ZfsOpCtx *ctx = g_task_get_task_data(task);
    GError *error = NULL;
    
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, &stderr_buf, &error);

    if (ctx->timeout_id > 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    if (error) {
        g_task_return_error(task, error);
    } else if (!g_subprocess_get_successful(proc)) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS List Error: %s", clean_err);
        g_free(clean_err);
    } else {
        GPtrArray *snapshots = g_ptr_array_new_with_free_func(g_free);
        if (stdout_buf) {
            /* stdout을 줄 단위로 분할하여 파싱 */
            gchar **lines = g_strsplit(stdout_buf, "\n", -1);
            for (gint i = 0; lines[i] != NULL; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (strlen(line) > 0) {
                    /* "pcvpool/vms/web-prod@snap1" → '@' 이후 "snap1"만 추출 */
                    gchar *at_sign = g_strrstr(line, "@");
                    if (at_sign && *(at_sign + 1) != '\0') g_ptr_array_add(snapshots, g_strdup(at_sign + 1));
                }
            }
            g_strfreev(lines);
        }
        g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    g_object_unref(task);
}

/**
 * purecvisor_zfs_snapshot_list_async:
 * 지정 VM의 ZFS 스냅샷 목록을 비동기로 조회합니다.
 *
 * @param pool_name   ZFS 풀 이름 (예: "pcvpool")
 * @param vm_name     VM 이름 — 이 VM의 데이터셋에 속한 스냅샷만 조회
 * @param cancellable 외부 취소 핸들 (NULL 가능)
 * @param callback    완료 콜백
 * @param user_data   콜백 사용자 데이터
 */
void purecvisor_zfs_snapshot_list_async(const gchar *pool_name, const gchar *vm_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    ZfsOpCtx *ctx = g_new0(ZfsOpCtx, 1);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : g_cancellable_new();
    g_task_set_task_data(task, ctx, zfs_op_ctx_free);

    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s", pool_name, vm_name);
    const gchar *argv[] = {"zfs", "list", "-t", "snapshot", "-H", "-o", "name", target, NULL};

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv, 
                                          G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, 
                                          &error);
    g_free(target);
    
    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }

    ctx->timeout_id = g_timeout_add_seconds(30, on_zfs_timeout, task);
    g_subprocess_communicate_utf8_async(proc, NULL, ctx->cancellable, on_zfs_list_ready, task);
    g_object_unref(proc);
}

/**
 * purecvisor_zfs_snapshot_list_finish:
 * 스냅샷 목록 비동기 작업의 결과를 회수합니다.
 *
 * @param res   GAsyncResult (GTask)
 * @param error 에러 시 GError** 설정
 * @return GPtrArray<gchar*> 스냅샷 이름 배열 — 호출자가 g_ptr_array_unref()로 해제
 *         실패 시 NULL
 */
GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* =========================================================================
 * 5. ZFS 볼륨 프로비저닝 (동기식 — Worker 스레드 전용)
 *
 * 이 함수들은 블로킹이므로 GMainLoop에서 직접 호출하면 안 됩니다.
 * vm_manager.c의 GTask 워커 스레드에서 호출됩니다.
 * ========================================================================= */

/**
 * purecvisor_zfs_create_volume:
 * 새 ZFS zvol을 동기적으로 생성합니다.
 * 명령: zfs create -V <size_str> <pool_name>/<vm_name>
 *
 * @param pool_name  ZFS 풀 경로 (예: "pcvpool/vms")
 * @param vm_name    VM 이름 — zvol 이름이 됨
 * @param size_str   볼륨 크기 문자열 (예: "50G", "100G")
 * @param error      실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 *
 * 디자인 결정: pcv_spawn_sync를 사용하여 zfs CLI를 직접 실행합니다.
 * libzfs 라이브러리 대신 CLI를 사용하는 이유:
 *   - libzfs는 안정적인 공개 API가 아님 (ABI 호환성 보장 없음)
 *   - CLI는 모든 ZFS 버전에서 동작하며 에러 메시지가 명확함
 */
gboolean purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error)
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    gchar *stderr_buf = NULL;

    /* BUG-18 fix (F-1 Phase 1): pool 직렬화 락 (intra-node) */
    if (!pcv_zfs_pool_lock(pool_name, "create", 30000, error)) {
        g_free(target_dataset);
        return FALSE;
    }

    /* BUG-18 Phase 2: etcd 분산 락 (inter-node) — cluster 빌드 + etcd 활성 시
     * 1.0: dynamic TTL — size_str("50G") 파싱 → ttl 산출 (5~600s) */
#if PCV_CLUSTER_ENABLED
    PcvEtcdClient *etcd = pcv_cluster_get_etcd();
    const gchar *node_name = pcv_config_get_string("cluster", "node_name", "local");
    /* size_str 파싱: "50G", "100GB", "1024" → gint */
    gint size_gb = 0;
    if (size_str && *size_str) {
        size_gb = (gint)g_ascii_strtoll(size_str, NULL, 10);
        if (size_gb < 1) size_gb = 1;
    }
    gint dyn_ttl = pcv_etcd_compute_inflight_ttl("create", size_gb);
    gboolean dist_locked = _zfs_acquire_inflight_lock_with_metrics(
        etcd, pool_name, node_name, "create", dyn_ttl);
    /* etcd 락 실패해도 계속 진행 (intra-node 락은 이미 보유, etcd는 best-effort) */
#endif

    /* -s: sparse(thin provisioning) — 실제 사용량만큼만 디스크 점유, 즉시 할당 없음 */
    const gchar *argv[] = {"zfs", "create", "-s", "-V", size_str, target_dataset, NULL};

    success = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!success && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS creation error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Create Error: %s", clean_err);
        g_free(clean_err);
    }

#if PCV_CLUSTER_ENABLED
    if (dist_locked && etcd) {
        pcv_etcd_release_inflight_lock(etcd, pool_name, NULL);
    }
#endif
    pcv_zfs_pool_unlock(pool_name);   /* BUG-18 F-1 */
    g_free(stderr_buf);
    g_free(target_dataset);
    return success;
}

/**
 * purecvisor_zfs_destroy_volume:
 * ZFS zvol과 하위 스냅샷을 동기적으로 삭제합니다.
 * 명령: zfs destroy -r <pool_name>/<vm_name>
 * -r 플래그: 하위 스냅샷도 재귀적으로 삭제합니다.
 *
 * @param pool_name  ZFS 풀 경로
 * @param vm_name    VM 이름
 * @param error      실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 *
 * 주의: virDomainDestroy 직후 호출 시 zvol이 아직 busy일 수 있습니다.
 * vm_manager.c의 _zfs_destroy_thread에서 지수 백오프 재시도로 처리합니다.
 */
gboolean purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error)
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    gchar *stderr_buf = NULL;

    /* BUG-18 fix (F-1): pool 직렬화 락 */
    if (!pcv_zfs_pool_lock(pool_name, "destroy", 30000, error)) {
        g_free(target_dataset);
        return FALSE;
    }

#if PCV_CLUSTER_ENABLED
    PcvEtcdClient *etcd = pcv_cluster_get_etcd();
    const gchar *node_name = pcv_config_get_string("cluster", "node_name", "local");
    gint dyn_ttl = pcv_etcd_compute_inflight_ttl("destroy", 0);
    gboolean dist_locked = _zfs_acquire_inflight_lock_with_metrics(
        etcd, pool_name, node_name, "destroy", dyn_ttl);
    /* etcd 락 실패해도 intra-node 락 보호 하에 graceful degradation */
#endif

    /* -r: 하위 스냅샷 포함 재귀 삭제 */
    const gchar *argv[] = {"zfs", "destroy", "-r", target_dataset, NULL};

    success = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!success && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS destroy error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Destroy Error: %s", clean_err);
        g_free(clean_err);
    }

#if PCV_CLUSTER_ENABLED
    if (dist_locked && etcd) {
        pcv_etcd_release_inflight_lock(etcd, pool_name, NULL);
    }
#endif
    pcv_zfs_pool_unlock(pool_name);   /* BUG-18 F-1 */
    g_free(stderr_buf);
    g_free(target_dataset);
    return success;
}

/* =========================================================================
 * 6. ZFS 풀 관리 (Pool Create / Destroy / Scrub) — 동기식
 *
 * 블로킹 함수이므로 GTask 워커 스레드에서만 호출해야 합니다.
 * ========================================================================= */

/**
 * purecvisor_zfs_create_pool:
 * 새 ZFS 풀을 생성합니다.
 * 명령: zpool create -f -o ashift=12 -O compression=lz4 -O atime=off <name> [vdev_type] <disk1> ...
 *
 * @param name        풀 이름 (예: "mypool")
 * @param vdev_type   VDEV 유형 (예: "mirror", "raidz", "raidz2", NULL이면 stripe)
 * @param disks       디스크 경로 배열 (예: {"/dev/sdb", "/dev/sdc"})
 * @param n_disks     디스크 수
 * @param compression 압축 알고리즘 ("lz4"/"zstd"/"gzip"/"off", NULL→"lz4")
 * @param error       실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_zfs_create_pool(const gchar *name, const gchar *vdev_type,
                                     const gchar **disks, gint n_disks,
                                     const gchar *compression, GError **error)
{
    const gchar *comp = (compression && *compression) ? compression : "lz4";
    gchar *comp_opt = g_strdup_printf("compression=%s", comp);

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"zpool");
    g_ptr_array_add(argv, (gchar *)"create");
    g_ptr_array_add(argv, (gchar *)"-f");
    g_ptr_array_add(argv, (gchar *)"-o");
    g_ptr_array_add(argv, (gchar *)"ashift=12");
    g_ptr_array_add(argv, (gchar *)"-O");
    g_ptr_array_add(argv, comp_opt);
    g_ptr_array_add(argv, (gchar *)"-O");
    g_ptr_array_add(argv, (gchar *)"atime=off");
    g_ptr_array_add(argv, (gchar *)name);
    if (vdev_type && *vdev_type)
        g_ptr_array_add(argv, (gchar *)vdev_type);
    for (gint i = 0; i < n_disks; i++)
        g_ptr_array_add(argv, (gchar *)disks[i]);
    g_ptr_array_add(argv, NULL);

    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)argv->pdata, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool create failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    g_free(comp_opt);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

/**
 * purecvisor_zfs_destroy_pool:
 * ZFS 풀을 강제 삭제합니다.
 * 명령: zpool destroy -f <name>
 *
 * @param name   삭제할 풀 이름
 * @param error  실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 *
 * 주의: 풀 내 모든 데이터셋/스냅샷이 즉시 영구 삭제됩니다!
 */
gboolean purecvisor_zfs_destroy_pool(const gchar *name, GError **error)
{
    const gchar *argv[] = {"zpool", "destroy", "-f", name, NULL};
    gchar *std_err = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool destroy failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    return ok;
}

/**
 * purecvisor_zfs_scrub_pool:
 * ZFS 풀 스크럽을 시작합니다. 데이터 무결성 검증 작업입니다.
 * 명령: zpool scrub <name>
 *
 * @param name   스크럽할 풀 이름
 * @param error  실패 시 GError** 설정
 * @return TRUE: 성공(스크럽 시작됨), FALSE: 실패
 *
 * 스크럽은 백그라운드에서 실행되며, 완료까지 수 시간이 걸릴 수 있습니다.
 * 진행 상태는 zpool status로 확인할 수 있습니다.
 */
gboolean purecvisor_zfs_scrub_pool(const gchar *name, GError **error)
{
    const gchar *argv[] = {"zpool", "scrub", name, NULL};
    gchar *std_err = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zpool scrub failed: %s", std_err ? std_err : "unknown");
    }
    g_free(std_err);
    return ok;
}

/* =========================================================================
 * 7. ZFS VM Clone — CoW Clone + Full Copy (동기식 — Worker 스레드 전용)
 * ========================================================================= */

/**
 * purecvisor_zfs_clone_volume:
 * ZFS 스냅샷 기반 CoW 클론을 동기적으로 생성합니다.
 * 즉시 완료되며 실제 데이터 복사는 발생하지 않습니다 (copy-on-write).
 *
 * 명령: zfs clone <pool_name>/<source_vm>@<snap_name> <pool_name>/<clone_vm>
 */
gboolean purecvisor_zfs_clone_volume(const gchar *pool_name, const gchar *source_vm,
                                      const gchar *snap_name, const gchar *clone_vm,
                                      GError **error)
{
    gchar *snap_path  = g_strdup_printf("%s/%s@%s", pool_name, source_vm, snap_name);
    gchar *clone_path = g_strdup_printf("%s/%s", pool_name, clone_vm);

    const gchar *argv[] = {"zfs", "clone", snap_path, clone_path, NULL};
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, error);
    if (!ok && error && !*error) {
        gchar *clean_err = stderr_buf ? g_strstrip(g_strdup(stderr_buf)) : g_strdup("Unknown ZFS clone error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZFS Clone Error: %s", clean_err);
        g_free(clean_err);
    }

    g_free(stderr_buf);
    g_free(snap_path);
    g_free(clone_path);
    return ok;
}

/**
 * purecvisor_zfs_full_copy:
 * ZFS send/recv 기반 전체 복사를 동기적으로 수행합니다.
 * pcv_spawn_pipe_sync()로 send stdout을 recv stdin에 직접 연결합니다.
 * 대용량 볼륨의 경우 수 분이 소요될 수 있습니다.
 */
gboolean purecvisor_zfs_full_copy(const gchar *pool_name, const gchar *source_vm,
                                   const gchar *snap_name, const gchar *clone_vm,
                                   GError **error)
{
    gchar *snap_path  = g_strdup_printf("%s/%s@%s", pool_name, source_vm, snap_name);
    gchar *clone_path = g_strdup_printf("%s/%s", pool_name, clone_vm);
    const gchar *send_argv[] = {"zfs", "send", snap_path, NULL};
    const gchar *recv_argv[] = {"zfs", "recv", clone_path, NULL};
    gchar *stderr_buf = NULL;

    /* 비전공자 기준:
     * 예전 구현처럼 `/tmp` 파일을 만들면 100GB VM 복제 시 /tmp가 가득 찰 수 있다.
     * 셸의 `|`, `>`, `<`를 쓰지 않고 두 프로세스의 입출력만 직접 연결하면
     * 같은 복제를 디스크 임시 공간 없이 스트리밍으로 처리할 수 있다. */
    gboolean result = pcv_spawn_pipe_sync(send_argv, recv_argv,
                                          NULL, &stderr_buf, error);
    if (!result && error && !*error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS full copy failed: %s",
                    (stderr_buf && *stderr_buf) ? stderr_buf : "unknown");
    }

    g_free(stderr_buf);
    g_free(snap_path);
    g_free(clone_path);
    return result;
}

/* =========================================================================
 * 8. ZFS Pool Health Monitoring
 *
 * `zpool status -p <pool>` 및 `zpool list -Hp -o cap <pool>` 파싱으로
 * 풀 상태, 에러 카운트, 스크럽 정보를 수집한다.
 *
 * 텔레메트리 스레드(ebpf_telemetry.c)에서 60초 주기로 호출하여
 * Prometheus 메트릭 + 알림 엔진 연동에 사용한다.
 * ========================================================================= */

/**
 * _parse_error_count — "0" 또는 숫자 문자열을 gint로 변환
 * 숫자가 아닌 경우 0을 반환한다.
 */
static gint
_parse_error_count(const gchar *s)
{
    if (!s || !*s) return 0;
    gchar *endp = NULL;
    glong val = strtol(s, &endp, 10);
    if (endp == s) return 0;  /* 변환 실패 */
    return (gint)val;
}

/**
 * pcv_zfs_pool_health:
 * ZFS 풀의 상태, 에러 카운트, 스크럽 정보, 용량을 수집합니다.
 *
 * 수집 단계 (2단계):
 *   1. zpool status -p: state, 에러(READ/WRITE/CKSUM), 스크럽 상태/시각 파싱
 *   2. zpool list -Hp -o capacity: 용량 사용 백분율 조회
 *
 * @param pool_name ZFS 풀 이름
 * @param out       결과를 채울 ZfsPoolHealth 구조체 포인터
 * @return TRUE: 성공, FALSE: zpool status 실패
 */
gboolean
pcv_zfs_pool_health(const gchar *pool_name, ZfsPoolHealth *out)
{
    if (!pool_name || !out) return FALSE;
    memset(out, 0, sizeof(ZfsPoolHealth));
    out->scrub_age_sec = -1;  /* 기본값: 스크럽 미실행 (-1 = 이력 없음) */
    g_strlcpy(out->state, "UNKNOWN", sizeof(out->state));

    /* ── 1. zpool status -p <pool> ─────────────────────── */
    const gchar *argv[] = {"zpool", "status", "-p", pool_name, NULL};
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    GError *err = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err)) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return FALSE;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(stdout_buf, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);

            /* state: ONLINE */
            if (g_str_has_prefix(line, "state:")) {
                gchar *val = g_strstrip(line + 6);
                g_strlcpy(out->state, val, sizeof(out->state));
            }

            /* errors: No known data errors */
            if (g_str_has_prefix(line, "errors:")) {
                gchar *val = g_strstrip(line + 7);
                if (strstr(val, "No known")) {
                    out->errors_read  = 0;
                    out->errors_write = 0;
                    out->errors_cksum = 0;
                }
            }

            /* Pool-level error line: pool_name  STATE  READ  WRITE  CKSUM */
            if (strstr(line, pool_name) && !g_str_has_prefix(line, "pool:")) {
                gchar **tokens = g_strsplit_set(line, " \t", -1);
                /* Compact non-empty tokens */
                GPtrArray *parts = g_ptr_array_new();
                for (gint t = 0; tokens[t]; t++) {
                    if (tokens[t][0]) g_ptr_array_add(parts, tokens[t]);
                }
                /* Expected: name STATE READ WRITE CKSUM */
                if (parts->len >= 5) {
                    const gchar *name_tok = g_ptr_array_index(parts, 0);
                    if (g_strcmp0(name_tok, pool_name) == 0) {
                        out->errors_read  = _parse_error_count(g_ptr_array_index(parts, 2));
                        out->errors_write = _parse_error_count(g_ptr_array_index(parts, 3));
                        out->errors_cksum = _parse_error_count(g_ptr_array_index(parts, 4));
                    }
                }
                g_ptr_array_free(parts, TRUE);
                g_strfreev(tokens);
            }

            /* scan: scrub repaired ... on Mon Mar 29 02:00:00 2026 */
            if (g_str_has_prefix(line, "scan:")) {
                if (strstr(line, "scrub in progress")) {
                    out->scrub_running = TRUE;
                    out->scrub_age_sec = 0;
                } else if (strstr(line, "scrub repaired") || strstr(line, "scrub")) {
                    /* Parse " on <date>" at end of line */
                    const gchar *on_str = strstr(line, " on ");
                    if (on_str) {
                        on_str += 4;  /* skip " on " */
                        struct tm tm_val;
                        memset(&tm_val, 0, sizeof(tm_val));
                        /* Try standard format: "Wed Mar 29 02:00:00 2026" */
                        gchar *rest = strptime(on_str, "%a %b %d %H:%M:%S %Y", &tm_val);
                        if (rest) {
                            tm_val.tm_isdst = -1;
                            time_t scrub_time = mktime(&tm_val);
                            if (scrub_time != (time_t)-1) {
                                out->scrub_age_sec = (gint64)difftime(time(NULL), scrub_time);
                                if (out->scrub_age_sec < 0) out->scrub_age_sec = 0;
                            }
                        }
                    }
                }
            }
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);

    /* ── 2. zpool list -Hp -o capacity <pool> ──────────── */
    const gchar *cap_argv[] = {"zpool", "list", "-Hp", "-o", "capacity", pool_name, NULL};
    gchar *cap_out = NULL;
    err = NULL;

    if (pcv_spawn_sync(cap_argv, &cap_out, NULL, &err) && cap_out) {
        gchar *trimmed = g_strstrip(cap_out);
        out->capacity_pct = g_ascii_strtod(trimmed, NULL);
    }
    g_free(cap_out);
    if (err) g_error_free(err);

    return TRUE;
}

/**
 * pcv_zfs_pool_health_to_json:
 * ZfsPoolHealth 구조체를 JSON 객체로 변환합니다.
 * 파생 필드 "health"를 state/에러/용량 기반으로 자동 계산합니다.
 *
 * health 등급:
 *   critical — FAULTED/UNAVAIL (풀 접근 불가)
 *   degraded — DEGRADED 또는 에러 카운트 > 0
 *   warning  — 용량 85% 초과
 *   healthy  — 정상
 *
 * @param h ZfsPoolHealth 구조체 포인터
 * @return JsonObject* — 호출자가 소유
 */
JsonObject *
pcv_zfs_pool_health_to_json(const ZfsPoolHealth *h)
{
    JsonObject *obj = json_object_new();
    if (!h) return obj;

    json_object_set_string_member(obj, "state", h->state);
    json_object_set_int_member(obj, "errors_read", h->errors_read);
    json_object_set_int_member(obj, "errors_write", h->errors_write);
    json_object_set_int_member(obj, "errors_cksum", h->errors_cksum);
    json_object_set_int_member(obj, "scrub_age_seconds", h->scrub_age_sec);
    json_object_set_boolean_member(obj, "scrub_running", h->scrub_running);
    json_object_set_double_member(obj, "capacity_percent", h->capacity_pct);

    /* Derived health status */
    const gchar *health = "healthy";
    if (g_strcmp0(h->state, "FAULTED") == 0 || g_strcmp0(h->state, "UNAVAIL") == 0)
        health = "critical";
    else if (g_strcmp0(h->state, "DEGRADED") == 0)
        health = "degraded";
    else if (h->errors_read > 0 || h->errors_write > 0 || h->errors_cksum > 0)
        health = "degraded";
    else if (h->capacity_pct > 85.0)
        health = "warning";
    json_object_set_string_member(obj, "health", health);

    return obj;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 8-2. 스토리지 용량 예측 (Capacity Forecasting)
 *
 * [개요]
 *   링 버퍼에 1시간 간격의 용량 샘플을 최대 168개(7일) 저장하고,
 *   선형 회귀(OLS)로 풀이 언제 가득 찰지 예측합니다.
 *
 * [알고리즘 — OLS(Ordinary Least Squares) 선형 회귀]
 *   y = a + b*x 직선을 데이터에 피팅합니다.
 *   x = 시간(초), y = 사용량(바이트)
 *   기울기(slope) = 초당 증가 바이트 → 일당 증가량 = slope * 86400
 *   남은 용량 / 일당 증가량 = 풀이 가득 차기까지 남은 일수
 *
 * [링 버퍼란?]
 *   고정 크기 배열에 head 포인터가 순환하며 덮어쓰는 자료구조.
 *   새 데이터가 들어오면 가장 오래된 데이터가 자동으로 삭제됩니다.
 *   동적 메모리 할당 없이 최근 N개의 데이터를 유지할 수 있습니다.
 *
 * [호출 경로]
 *   ebpf_telemetry.c (60초 주기) → pcv_zfs_capacity_record() → 샘플 저장
 *   RPC storage.forecast         → pcv_zfs_pool_forecast()    → 예측 결과 반환
 * ═══════════════════════════════════════════════════════════════════════ */

#define CAP_HISTORY_MAX  168  /* 7일 * 24시간 = 최대 168개 샘플 */

/**
 * CapacitySample:
 * 단일 시점의 ZFS 풀 용량 스냅샷.
 * 링 버퍼의 각 슬롯에 저장되는 레코드입니다.
 */
typedef struct {
    gint64 ts;          /* 샘플 수집 시각 (Unix timestamp) */
    gint64 used_bytes;  /* 풀 사용량 (바이트) */
    gint64 total_bytes; /* 풀 전체 용량 (used + available) */
} CapacitySample;

/** 전역 용량 이력 — 링 버퍼 + 뮤텍스 보호 */
static struct {
    CapacitySample samples[CAP_HISTORY_MAX];  /* 고정 크기 링 버퍼 */
    gint           count;    /* 누적 기록 수 (MAX 초과 가능 — 링 순환 판단용) */
    gint           head;     /* 다음 쓰기 위치 (0 ~ MAX-1 순환) */
    GMutex         mu;       /* 멀티스레드 접근 직렬화 */
    gboolean       mu_init;  /* 뮤텍스 초기화 여부 */
} g_cap_hist = {0};

static void
_cap_hist_ensure_init(void)
{
    if (!g_cap_hist.mu_init) {
        g_mutex_init(&g_cap_hist.mu);
        g_cap_hist.mu_init = TRUE;
    }
}

/**
 * pcv_zfs_capacity_record:
 * 현재 풀 사용량을 링 버퍼에 기록합니다. 텔레메트리 스레드에서 60초 주기로 호출.
 *
 * @param pool_name ZFS 풀 이름 (예: "pcvpool")
 */
void
pcv_zfs_capacity_record(const gchar *pool_name)
{
    if (!pool_name) return;
    _cap_hist_ensure_init();

    /* 현재 사용량 조회: zfs get -Hp -o value used/available <pool>
     * -H: 헤더 없음, -p: 바이트 단위(파싱 용이), -o value: 값만 출력 */
    const gchar *argv_used[] = {"zfs", "get", "-Hp", "-o", "value", "used", pool_name, NULL};
    const gchar *argv_avail[] = {"zfs", "get", "-Hp", "-o", "value", "available", pool_name, NULL};
    gchar *used_str = NULL, *avail_str = NULL;

    pcv_spawn_sync(argv_used, &used_str, NULL, NULL);
    pcv_spawn_sync(argv_avail, &avail_str, NULL, NULL);

    if (!used_str || !avail_str) {
        g_free(used_str);
        g_free(avail_str);
        return;
    }

    gint64 used = g_ascii_strtoll(g_strstrip(used_str), NULL, 10);
    gint64 avail = g_ascii_strtoll(g_strstrip(avail_str), NULL, 10);
    g_free(used_str);
    g_free(avail_str);

    if (used <= 0 && avail <= 0) return;

    CapacitySample s = {
        .ts = (gint64)time(NULL),
        .used_bytes = used,
        .total_bytes = used + avail
    };

    g_mutex_lock(&g_cap_hist.mu);
    g_cap_hist.samples[g_cap_hist.head] = s;
    g_cap_hist.head = (g_cap_hist.head + 1) % CAP_HISTORY_MAX;
    g_cap_hist.count++;
    g_mutex_unlock(&g_cap_hist.mu);
}

/**
 * pcv_zfs_pool_forecast:
 * 링 버퍼의 이력 데이터를 기반으로 풀 용량 예측 결과를 JSON으로 반환합니다.
 *
 * [반환 필드]
 *   pool, used_bytes, total_bytes, used_pct     — 현재 상태
 *   history_points                               — 이력 샘플 수
 *   daily_growth_bytes                           — 일당 증가량 (바이트)
 *   days_to_full, predicted_full_date            — 예측 결과
 *   alert_level (ok/warn/critical)               — 7일 미만이면 critical
 *
 * [최소 요구 샘플]
 *   OLS 회귀는 최소 2개 샘플이 필요합니다. 부족 시 "insufficient data" 반환.
 *
 * @param pool_name ZFS 풀 이름
 * @return JsonObject* — 호출자가 소유
 */
JsonObject *
pcv_zfs_pool_forecast(const gchar *pool_name)
{
    JsonObject *result = json_object_new();
    if (!pool_name) {
        json_object_set_string_member(result, "error", "pool_name required");
        return result;
    }
    _cap_hist_ensure_init();

    json_object_set_string_member(result, "pool", pool_name);

    /* Get current usage for immediate reading */
    const gchar *argv_used[] = {"zfs", "get", "-Hp", "-o", "value", "used", pool_name, NULL};
    const gchar *argv_avail[] = {"zfs", "get", "-Hp", "-o", "value", "available", pool_name, NULL};
    gchar *used_str = NULL, *avail_str = NULL;
    pcv_spawn_sync(argv_used, &used_str, NULL, NULL);
    pcv_spawn_sync(argv_avail, &avail_str, NULL, NULL);

    gint64 cur_used = 0, cur_total = 0;
    if (used_str) { cur_used = g_ascii_strtoll(g_strstrip(used_str), NULL, 10); g_free(used_str); }
    if (avail_str) {
        gint64 avail = g_ascii_strtoll(g_strstrip(avail_str), NULL, 10);
        cur_total = cur_used + avail;
        g_free(avail_str);
    }

    json_object_set_int_member(result, "used_bytes", cur_used);
    json_object_set_int_member(result, "total_bytes", cur_total);

    gdouble used_pct = (cur_total > 0) ? (gdouble)cur_used / cur_total * 100.0 : 0.0;
    json_object_set_double_member(result, "used_pct",
        (double)((gint)(used_pct * 100)) / 100.0);

    /* Linear regression on history */
    g_mutex_lock(&g_cap_hist.mu);
    gint n = (g_cap_hist.count < CAP_HISTORY_MAX) ? g_cap_hist.count : CAP_HISTORY_MAX;

    if (n < 2) {
        g_mutex_unlock(&g_cap_hist.mu);
        json_object_set_int_member(result, "history_points", n);
        json_object_set_string_member(result, "forecast", "insufficient data (need >= 2 hourly samples)");
        json_object_set_double_member(result, "daily_growth_bytes", 0.0);
        json_object_set_int_member(result, "days_to_full", -1);
        json_object_set_string_member(result, "predicted_full_date", "N/A");
        json_object_set_string_member(result, "alert_level", "ok");
        return result;
    }

    /* 뮤텍스 보호 하에 샘플을 로컬 버퍼로 복사 — 회귀 계산 중 lock 미보유 */
    CapacitySample *buf = g_new(CapacitySample, (gsize)n);
    gint oldest_idx;
    if (g_cap_hist.count <= CAP_HISTORY_MAX)
        oldest_idx = 0;           /* 아직 링 순환 전 — 0번부터 순서대로 */
    else
        oldest_idx = g_cap_hist.head; /* 링 순환됨 — head가 가장 오래된 위치 */

    for (gint i = 0; i < n; i++) {
        gint idx = (oldest_idx + i) % CAP_HISTORY_MAX;
        buf[i] = g_cap_hist.samples[idx];
    }
    g_mutex_unlock(&g_cap_hist.mu);

    json_object_set_int_member(result, "history_points", n);

    /* ── OLS(최소자승법) 선형 회귀 ──────────────────────────────────
     * y = used_bytes (사용량), x = time (첫 샘플 이후 경과 초)
     *
     * 기울기(slope) 공식:
     *   slope = (n * sum(x*y) - sum(x) * sum(y)) / (n * sum(x^2) - sum(x)^2)
     *
     * 기울기 = 초당 바이트 증가율 → 일당 증가량 = slope * 86400
     * 남은 용량(remaining) / 일당 증가량 = 풀이 가득 차기까지 남은 일수
     * ────────────────────────────────────────────────────────────── */
    gint64 t0 = buf[0].ts;
    gdouble sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (gint i = 0; i < n; i++) {
        gdouble x = (gdouble)(buf[i].ts - t0);
        gdouble y = (gdouble)buf[i].used_bytes;
        sum_x  += x;
        sum_y  += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    g_free(buf);

    /* 분모가 0에 가까우면 (모든 샘플이 같은 시간) 기울기를 0으로 처리 */
    gdouble denom = (gdouble)n * sum_x2 - sum_x * sum_x;
    gdouble slope_per_sec = 0.0;  /* 초당 바이트 증가율 */
    if (denom > 1e-6)
        slope_per_sec = ((gdouble)n * sum_xy - sum_x * sum_y) / denom;

    gdouble daily_growth = slope_per_sec * 86400.0;  /* 일당 바이트 증가량 */
    json_object_set_double_member(result, "daily_growth_bytes", daily_growth);

    gint64 remaining = cur_total - cur_used;
    gint days_to_full = -1;
    if (slope_per_sec > 1e-3 && remaining > 0)
        days_to_full = (gint)((gdouble)remaining / (slope_per_sec * 86400.0));

    json_object_set_int_member(result, "days_to_full", days_to_full);

    if (days_to_full > 0) {
        time_t full_time = time(NULL) + (time_t)(days_to_full * 86400);
        struct tm tm_val;
        gmtime_r(&full_time, &tm_val);
        gchar date_buf[32];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_val);
        json_object_set_string_member(result, "predicted_full_date", date_buf);
    } else {
        json_object_set_string_member(result, "predicted_full_date",
            slope_per_sec <= 0 ? "not growing" : "N/A");
    }

    /* 알림 수준 결정:
     *   critical : 7일 미만에 풀 가득 참 → 즉시 조치 필요
     *   warn     : 30일 미만 → 용량 확장 계획 수립
     *   ok       : 여유 있음 또는 사용량 감소 추세 */
    const gchar *alert = "ok";
    if (days_to_full > 0 && days_to_full < 7)
        alert = "critical";
    else if (days_to_full > 0 && days_to_full < 30)
        alert = "warn";
    json_object_set_string_member(result, "alert_level", alert);

    return result;
}

/* ═══════════════════════════════════════════════════════════════════════
 * zpool 상태 모니터링 API (CE-6)
 *
 * purecvisor_zfs_pool_health_detail:
 *   zpool list + zpool status를 실행하여 풀 상세 정보를 JsonObject로 반환.
 *   name/health/allocated/size/free/fragmentation/capacity + scrub 상태.
 *
 * purecvisor_zfs_promote:
 *   zfs promote 래퍼 — 클론 의존성을 해소하여 독립 데이터셋으로 전환.
 * ═══════════════════════════════════════════════════════════════════════ */

JsonObject *
purecvisor_zfs_pool_health_detail(const gchar *pool_name)
{
    JsonObject *result = json_object_new();

    /* zpool list -H -o ... */
    const gchar *argv[] = {"zpool", "list", "-H", "-o",
        "name,health,allocated,size,free,fragmentation,capacity",
        pool_name, NULL};
    gchar *out = NULL;
    GError *err = NULL;
    if (pcv_spawn_sync(argv, &out, NULL, &err)) {
        if (out && *out) {
            gchar **fields = g_strsplit(g_strstrip(out), "\t", -1);
            gint nf = g_strv_length(fields);
            if (nf >= 7) {
                json_object_set_string_member(result, "name", fields[0]);
                json_object_set_string_member(result, "health", fields[1]);
                json_object_set_string_member(result, "allocated", fields[2]);
                json_object_set_string_member(result, "size", fields[3]);
                json_object_set_string_member(result, "free", fields[4]);
                json_object_set_string_member(result, "fragmentation", fields[5]);
                json_object_set_string_member(result, "capacity", fields[6]);
            }
            g_strfreev(fields);
        }
    } else {
        json_object_set_string_member(result, "error",
            err ? err->message : "zpool list failed");
        if (err) g_error_free(err);
    }
    g_free(out);

    /* Scrub status — zpool status -p */
    const gchar *status_argv[] = {"zpool", "status", "-p", pool_name, NULL};
    gchar *status_out = NULL;
    if (pcv_spawn_sync(status_argv, &status_out, NULL, NULL) && status_out) {
        gboolean scrub_active = (strstr(status_out, "scrub in progress") != NULL);
        json_object_set_boolean_member(result, "scrub_in_progress", scrub_active);

        /* Parse scrub progress if active */
        if (scrub_active) {
            gchar *pct = strstr(status_out, "done");
            if (pct) {
                /* Walk backward to find the percentage */
                gchar *scan = pct - 1;
                while (scan > status_out &&
                       (*scan == ' ' || *scan == '%' ||
                        g_ascii_isdigit(*scan) || *scan == '.'))
                    scan--;
                gchar progress_str[16] = {0};
                g_strlcpy(progress_str, scan + 1,
                          MIN((gsize)(pct - scan), sizeof(progress_str)));
                json_object_set_string_member(result, "scrub_progress",
                    g_strstrip(progress_str));
            }
        }
    }
    g_free(status_out);

    return result;
}

gboolean
purecvisor_zfs_promote(const gchar *clone_name)
{
    if (!clone_name || !*clone_name) return FALSE;
    const gchar *argv[] = {"zfs", "promote", clone_name, NULL};
    gchar *std_err = NULL;
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);
    if (!ok) {
        g_warning("[ZFS] zfs promote '%s' failed: %s", clone_name,
                  err ? err->message : (std_err ? std_err : "unknown"));
        if (err) g_error_free(err);
    } else {
        g_message("[ZFS] Promoted clone '%s' to independent dataset", clone_name);
    }
    g_free(std_err);
    return ok;
}

/* =========================================================================
 * CE-A3: ZFS Native Encryption — 암호화된 zvol 생성
 *
 * AES-256-GCM 암호화를 적용한 zvol을 생성합니다.
 * passphrase를 stdin으로 전달하여 명령행 노출을 방지합니다.
 * GSubprocess를 직접 사용하여 stdin 파이프를 지원합니다.
 * ========================================================================= */

/**
 * purecvisor_zfs_create_zvol_encrypted:
 * 암호화된 ZFS zvol을 동기적으로 생성합니다.
 * 명령: zfs create -V <size> -o encryption=aes-256-gcm
 *       -o keyformat=passphrase -o keylocation=prompt -s <name>
 *
 * @param name        전체 데이터셋 경로 (예: "pcvpool/vms/secure-vm")
 * @param size        볼륨 크기 문자열 (예: "50G", "100G")
 * @param passphrase  암호화 패스프레이즈 (최소 8자 권장)
 * @param error       실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean
purecvisor_zfs_create_zvol_encrypted(const gchar *name, const gchar *size,
                                      const gchar *passphrase, GError **error)
{
    if (!name || !size || !passphrase || !*passphrase) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name, size, and passphrase are required");
        return FALSE;
    }

    const gchar *argv[] = {
        "zfs", "create",
        "-V", size,
        "-o", "encryption=aes-256-gcm",
        "-o", "keyformat=passphrase",
        "-o", "keylocation=prompt",
        "-s",  /* thin provisioning */
        name, NULL
    };

    /* passphrase를 stdin으로 2회 전달 (확인 입력 포함) */
    gchar *input = g_strdup_printf("%s\n%s\n", passphrase, passphrase);

    GSubprocess *proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!proc) {
        g_free(input);
        return FALSE;
    }

    gchar *stderr_out = NULL;
    gboolean ok = g_subprocess_communicate_utf8(proc, input, NULL, NULL, &stderr_out, error);
    gint exit_status = g_subprocess_get_exit_status(proc);
    g_free(input);
    g_object_unref(proc);

    if (!ok || exit_status != 0) {
        if (stderr_out && *stderr_out && error && !*error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", g_strstrip(stderr_out));
        g_free(stderr_out);
        return FALSE;
    }
    g_free(stderr_out);

    g_message("[ZFS] Created encrypted zvol: %s (aes-256-gcm)", name);
    return TRUE;
}

/* =========================================================================
 * CE-A4: Snapshot Quota Enforcement — 스냅샷 쿼터 검사
 *
 * 스냅샷 생성 전에 현재 스냅샷 수를 확인하여 쿼터를 초과하지 않도록 합니다.
 * pcv_spawn_sync로 `zfs list -t snapshot`을 실행하여 스냅샷 수를 센다.
 * ========================================================================= */

/**
 * purecvisor_zfs_check_snapshot_quota:
 * 데이터셋의 현재 스냅샷 수가 max_snapshots 미만인지 확인합니다.
 *
 * @param dataset        ZFS 데이터셋 이름 (예: "pcvpool/vms/web-prod")
 * @param max_snapshots  최대 허용 스냅샷 수 (0 또는 음수이면 무제한)
 * @return TRUE: 생성 허용, FALSE: 쿼터 초과
 */
gboolean
purecvisor_zfs_check_snapshot_quota(const gchar *dataset, gint max_snapshots)
{
    if (max_snapshots <= 0) return TRUE;  /* 무제한 */

    const gchar *argv[] = {"zfs", "list", "-H", "-t", "snapshot", "-o", "name", "-r", dataset, NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) {
        g_free(out);
        return TRUE;  /* 조회 실패 시 생성 허용 (안전 방향) */
    }

    gchar *trimmed = g_strstrip(out);
    gint count = 0;
    if (trimmed[0] != '\0') {
        gchar **lines = g_strsplit(trimmed, "\n", -1);
        for (gchar **l = lines; *l && **l; l++) count++;
        g_strfreev(lines);
    }
    g_free(out);

    if (count >= max_snapshots) {
        g_warning("[ZFS] Snapshot quota exceeded for '%s': %d/%d",
                  dataset, count, max_snapshots);
        return FALSE;
    }
    return TRUE;
}
