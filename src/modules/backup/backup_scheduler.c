/**
 * @file backup_scheduler.c
 * @brief ZFS 스냅샷 자동 백업 스케줄러 — 정책 기반 주기적 스냅샷 생성/보존/삭제
 *
 * [파일 역할]
 *   VM별 ZFS 스냅샷을 자동으로 생성하고, 보존 정책에 따라 오래된 스냅샷을
 *   자동 삭제하는 백업 자동화 모듈입니다. GLib 타이머(5분 간격)로 동작하며,
 *   정책은 JSON 파일에 영속화됩니다. RPC를 통한 수동 복원(rollback)도 지원합니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> pcv_backup_scheduler_init()  [이 파일]
 *          -> _policies_load() (JSON 파일에서 정책 로드)
 *          -> g_timeout_add_seconds(300, _backup_check_cb, NULL) (5분 타이머)
 *   handler_backup.c (backup.* RPC 핸들러)
 *     -> pcv_backup_policy_set/delete/list()   [이 파일, 정책 CRUD]
 *     -> pcv_backup_history(vm_name)           [이 파일, 스냅샷 이력]
 *     -> pcv_backup_restore(vm, snap)          [이 파일, zfs rollback]
 *
 * [동작 흐름 — 5분마다 자동 실행]
 *   1. _backup_check_cb() 타이머 콜백 발화 (GMainLoop 메인 스레드)
 *   2. 정책 배열(g_policies)을 순회:
 *      a) vm_name="*" 와일드카드면 libvirt에서 전체 VM 이름 목록 조회
 *      b) 개별 VM이면 해당 VM만 대상
 *   3. 각 대상 VM에 대해 _apply_policy_for_vm() 실행:
 *      a) zfs list -t snapshot으로 기존 pcv-auto-* 스냅샷 목록 조회
 *      b) 마지막 스냅샷 타임스탬프와 현재 시각 비교
 *      c) interval_hours 경과했으면 새 스냅샷 생성:
 *         zfs snapshot pcvpool/vms/<vm>@pcv-auto-YYYYMMDD-HHMMSS
 *      d) 스냅샷 수가 retention_count 초과하면 가장 오래된 것부터 삭제:
 *         zfs destroy pcvpool/vms/<vm>@pcv-auto-...
 *
 * [정책 저장 구조]
 *   파일: /etc/purecvisor/backup_policies.json
 *   형식: JSON 배열 [{"vm_name":"...", "interval_hours":N, "retention_count":N, "enabled":true}, ...]
 *   upsert: 동일 vm_name 정책이 있으면 갱신, 없으면 추가
 *   삭제: 정책만 제거 (기존 스냅샷은 유지)
 *
 * [스냅샷 네이밍 컨벤션]
 *   접두사: "pcv-auto-" (수동 스냅샷과 자동 스냅샷을 구분)
 *   형식: pcv-auto-YYYYMMDD-HHMMSS (예: pcv-auto-20260323-143000)
 *   경로: pcvpool/vms/<vm_name>@pcv-auto-YYYYMMDD-HHMMSS
 *
 * [핵심 패턴]
 *   - ZFS 명령 실행: pcv_spawn_sync() 경유 (fork+exec, 동기 블로킹)
 *     -> 메인 스레드에서 실행되므로 대량 VM 시 타이머 콜백 지연 가능
 *   - 정책 영속화: 변경 시 즉시 JSON 파일에 저장 (_policies_save)
 *   - 와일드카드: vm_name="*"이면 libvirt virConnectListAllDomains()로 전체 VM 조회
 *   - 멱등성: pcv_backup_policy_delete()는 정책 미존재 시 FALSE (에러 반환)
 *
 * [스레드 안전]
 *   GMutex(g_policy_mutex)로 정책 배열(g_policies) 접근을 직렬화합니다.
 *   타이머 콜백은 메인 스레드에서 실행되고, RPC 핸들러도 메인 스레드이므로
 *   실질적으로 경합은 드물지만, 안전을 위해 Mutex를 사용합니다.
 *
 * [주의사항]
 *   - ZFS 풀 경로(pcvpool/vms)는 pcv_config_get_zvol_pool()에서 가져옴
 *   - pcv_backup_restore()는 fire-and-forget 패턴으로 RPC 응답 후 비동기 실행
 *   - 스냅샷 타임스탬프 파싱 실패 시 해당 스냅샷은 건너뜀 (로그 경고)
 *   - CHECK_INTERVAL(300초=5분)은 컴파일 타임 상수 (런타임 변경 불가)
 */

#include "backup_scheduler.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_ssrf.h"
#include "../../modules/virt/virt_conn_pool.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define BACKUP_LOG_DOM   "backup"
#define POLICY_FILE_PATH "/etc/purecvisor/backup_policies.json"
#define CHECK_INTERVAL   300   /* 5분 (초 단위) */
#define SNAP_PREFIX      "pcv-auto-"  /* 자동 스냅샷 이름 접두사 — 수동 스냅샷과 구분 */

/* ══════════════════════════════════════════════════════════════
 * [1] 모듈 전역 상태
 * ══════════════════════════════════════════════════════════════ */

static GPtrArray *g_policies  = nullptr;   /* element-type PcvBackupPolicy* — 정책 배열 */
static guint      g_timer_id  = 0;      /* GLib 타이머 소스 ID (g_source_remove용) */
static GMutex     g_policy_mutex;       /* 정책 배열 접근 보호 뮤텍스 */

/* B8-C3: per-VM 백업 작업 락.
 * 동일 VM에 대한 restore/incremental/replicate/s3_export 동시 실행을 차단합니다.
 * 키: VM 이름(gchar*) — 값 없음(set 용도). g_policy_mutex로 보호. */
static GHashTable *g_vm_inflight = nullptr;

/* ══════════════════════════════════════════════════════════════
 * [2] 메모리 관리 — PcvBackupPolicy 해제/복제
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_backup_policy_free:
 * @p: 해제할 정책 구조체 (NULL 안전)
 *
 * PcvBackupPolicy의 동적 멤버(vm_name)와 구조체 자체를 해제합니다.
 * GPtrArray의 free_func으로 등록되어 자동 호출됩니다.
 */
void pcv_backup_policy_free(PcvBackupPolicy *p)
{
    if (!p) return;
    g_free(p->vm_name);
    g_free(p);
}

/**
 * _policy_dup:
 * @src: 복제할 원본 정책
 *
 * 정책의 깊은 복사본을 생성합니다.
 * pcv_backup_policy_list()에서 뮤텍스 밖으로 안전하게 데이터를 전달하기 위해 사용합니다.
 *
 * Returns: (transfer full): 새로 할당된 PcvBackupPolicy* (pcv_backup_policy_free 필요)
 */
static PcvBackupPolicy *_policy_dup(const PcvBackupPolicy *src)
{
    PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
    p->vm_name         = g_strdup(src->vm_name);
    p->interval_hours  = src->interval_hours;
    p->retention_count = src->retention_count;
    p->enabled         = src->enabled;
    return p;
}

/* ══════════════════════════════════════════════════════════════
 * [2.5] per-VM 백업 락 (B8-C3)
 * ══════════════════════════════════════════════════════════════ */

/**
 * _vm_backup_try_lock:
 * @vm_name: 락을 획득할 VM 이름
 *
 * 동일 VM의 백업 작업이 이미 진행 중인지 확인하고, 없으면 락을 획득합니다.
 * 같은 VM에 대해 zfs snapshot/rollback/send가 겹치면 스냅샷 경합,
 * 파일 충돌, rollback 중 새 스냅샷 생성 등의 위험이 있습니다.
 *
 * Returns: 성공 시 TRUE (락 획득), 이미 진행 중이면 FALSE
 */
static gboolean _vm_backup_try_lock(const gchar *vm_name)
{
    if (!vm_name || *vm_name == '\0') return FALSE;

    g_mutex_lock(&g_policy_mutex);
    if (!g_vm_inflight) {
        g_vm_inflight = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    }
    if (g_hash_table_contains(g_vm_inflight, vm_name)) {
        g_mutex_unlock(&g_policy_mutex);
        return FALSE;
    }
    g_hash_table_add(g_vm_inflight, g_strdup(vm_name));
    g_mutex_unlock(&g_policy_mutex);
    return TRUE;
}

/**
 * _vm_backup_unlock:
 * @vm_name: 해제할 VM 이름
 */
static void _vm_backup_unlock(const gchar *vm_name)
{
    if (!vm_name) return;
    g_mutex_lock(&g_policy_mutex);
    if (g_vm_inflight) {
        g_hash_table_remove(g_vm_inflight, vm_name);
    }
    g_mutex_unlock(&g_policy_mutex);
}

/**
 * _check_backup_disk_usage (B8-W7):
 * @path: 확인할 백업 대상 경로 (파일 또는 디렉터리)
 *
 * statvfs()로 해당 파일시스템의 사용률을 확인하고 임계치 초과 시 경고.
 *   - 90% 초과: CRIT 로그 + Prometheus 알림 가능 상태
 *   - 80% 초과: WARN 로그
 * 실패 시 조용히 무시 (백업 자체는 계속 진행 — 경고만 보조 역할).
 */
static void _check_backup_disk_usage(const gchar *path)
{
    if (!path || !*path) return;
    struct statvfs st;
    if (statvfs(path, &st) != 0) return;
    if (st.f_blocks == 0) return;

    guint64 total = (guint64)st.f_blocks * st.f_frsize;
    guint64 avail = (guint64)st.f_bavail * st.f_frsize;
    if (total == 0) return;
    gdouble used_pct = 100.0 * (gdouble)(total - avail) / (gdouble)total;

    if (used_pct >= 90.0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Backup storage CRITICAL: %s %.1f%% used (avail=%"
                     G_GUINT64_FORMAT " bytes)",
                     path, used_pct, avail);
    } else if (used_pct >= 80.0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Backup storage high usage: %s %.1f%% used",
                     path, used_pct);
    }
}

/* ══════════════════════════════════════════════════════════════
 * [3] 정책 파일 I/O — JSON 직렬화/역직렬화
 * ══════════════════════════════════════════════════════════════ */

/**
 * _policies_save_unlocked:
 *
 * 현재 g_policies 배열을 JSON으로 직렬화하여 POLICY_FILE_PATH에 저장합니다.
 * "unlocked"는 이 함수가 뮤텍스를 잡지 않으므로, 호출자가 반드시
 * g_policy_mutex를 잡은 상태에서 호출해야 한다는 의미입니다.
 *
 * JSON 형식 예:
 *   {"policies": [{"vm_name":"web-prod","interval_hours":6,"retention_count":7,"enabled":true}]}
 */
static void _policies_save_unlocked(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "policies");
    json_builder_begin_array(b);

    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "vm_name");
        json_builder_add_string_value(b, p->vm_name);
        json_builder_set_member_name(b, "interval_hours");
        json_builder_add_int_value(b, p->interval_hours);
        json_builder_set_member_name(b, "retention_count");
        json_builder_add_int_value(b, p->retention_count);
        json_builder_set_member_name(b, "enabled");
        json_builder_add_boolean_value(b, p->enabled);
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);

    GError *err = nullptr;
    if (!json_generator_to_file(gen, POLICY_FILE_PATH, &err)) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Failed to save policies: %s",
                     err->message);
        g_error_free(err);
    }

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);
}

/**
 * _policies_load:
 *
 * POLICY_FILE_PATH에서 JSON을 읽어 g_policies 배열에 로드합니다.
 * 파일이 없으면 정상으로 처리합니다 (첫 실행 시 빈 정책으로 시작).
 *
 * 각 정책의 기본값:
 *   vm_name="*", interval_hours=24, retention_count=7, enabled=true
 */
static void _policies_load(void)
{
    JsonParser *parser = json_parser_new();
    GError *err = nullptr;

    if (!json_parser_load_from_file(parser, POLICY_FILE_PATH, &err)) {
        /* 파일 없음은 정상 (첫 실행) */
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "No policy file found (%s), starting with empty policies",
                     POLICY_FILE_PATH);
        g_error_free(err);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "policies")) {
        g_object_unref(parser);
        return;
    }

    JsonArray *arr = json_object_get_array_member(obj, "policies");
    guint len = json_array_get_length(arr);

    for (guint i = 0; i < len; i++) {
        JsonObject *po = json_array_get_object_element(arr, i);
        if (!po) continue;

        PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
        p->vm_name = g_strdup(
            json_object_get_string_member_with_default(po, "vm_name", "*"));
        p->interval_hours = (gint)
            json_object_get_int_member_with_default(po, "interval_hours", 24);
        p->retention_count = (gint)
            json_object_get_int_member_with_default(po, "retention_count", 7);
        p->enabled =
            json_object_get_boolean_member_with_default(po, "enabled", TRUE);

        g_ptr_array_add(g_policies, p);
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Loaded %u backup policies", g_policies->len);
    g_object_unref(parser);
}

/* ══════════════════════════════════════════════════════════════
 * [3.5] 게스트 파일시스템 동결/해제 — 앱 정합 스냅샷 (fsfreeze)
 * ══════════════════════════════════════════════════════════════ */

/**
 * _fsfreeze_vm:
 * @vm_name: 동결할 VM 이름
 *
 * 스냅샷 생성 직전 게스트 파일시스템을 동결합니다 (virsh domfsfreeze).
 * QEMU guest agent가 설치되어 있어야 동작합니다.
 * 실패해도 스냅샷 생성은 계속 진행합니다 (guest agent 미설치 환경 대응).
 */
static void
_fsfreeze_vm(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domfsfreeze", vm_name, NULL};
    gchar *out = nullptr;
    GError *err = nullptr;
    if (pcv_spawn_sync(argv, &out, NULL, &err)) {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Froze filesystem for VM '%s'", vm_name);
    } else {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "fsfreeze failed for '%s' (guest agent may not be running): %s",
                     vm_name, err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    g_free(out);
}

/**
 * _fsthaw_vm:
 * @vm_name: 해제할 VM 이름
 *
 * 스냅샷 생성 직후 동결된 파일시스템을 해제합니다 (virsh domfsthaw).
 * fsfreeze가 실패했더라도 안전하게 호출합니다 (멱등).
 */
static void
_fsthaw_vm(const gchar *vm_name)
{
    const gchar *argv[] = {"virsh", "domfsthaw", vm_name, NULL};
    (void)pcv_spawn_sync(argv, NULL, NULL, NULL);
    PCV_LOG_INFO(BACKUP_LOG_DOM, "Thawed filesystem for VM '%s'", vm_name);
}

/* ══════════════════════════════════════════════════════════════
 * [3.6] 백업 후 무결성 검증 — 스냅샷 존재/크기 확인
 * ══════════════════════════════════════════════════════════════ */

/**
 * _verify_snapshot:
 * @snapshot_name: ZFS 스냅샷 full path (예: "pcvpool/vms/web-prod@pcv-auto-...")
 *
 * `zfs list`로 스냅샷 존재 여부와 사용량/참조량을 확인합니다.
 * 스냅샷 생성 직후 호출하여 무결성을 보장합니다.
 *
 * Returns: 스냅샷이 정상 존재하면 TRUE
 */
static gboolean
_verify_snapshot(const gchar *snapshot_name)
{
    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "used,referenced",
        "-t", "snapshot", snapshot_name, NULL
    };
    gchar *out = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, &err);
    if (!ok || !out || !*out) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot verification failed for '%s': %s",
                     snapshot_name, err ? err->message : "not found");
        if (err) g_error_free(err);
        g_free(out);
        return FALSE;
    }
    PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot verified: %s (%s)",
                 snapshot_name, g_strstrip(out));
    g_free(out);
    return TRUE;
}

/**
 * _verify_replication:
 * @latest:   검증할 최신 ZFS 스냅샷 full path
 * @peer_ssh: 원격 노드 IP/호스트명
 * @ssh_user: SSH 사용자명
 *
 * 복제 후 원격 노드에 최신 스냅샷이 존재하는지 확인합니다.
 * SSH를 통해 원격 `zfs list` 명령으로 검증합니다.
 *
 * Returns: 원격에 스냅샷이 존재하면 TRUE
 */
static gboolean
_verify_replication(const gchar *latest,
                    const gchar *peer_ssh,
                    const gchar *ssh_user)
{
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, peer_ssh);
    const gchar *check_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-H", "-o", "name", "-t", "snapshot", latest,
        NULL
    };
    gchar *out = nullptr;
    gboolean ok = pcv_spawn_sync(check_argv, &out, NULL, NULL);

    if (ok && out && *out) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Replication verified: remote has %s",
                     g_strstrip(out));
    } else {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Replication verification failed for %s on %s",
                     latest, peer_ssh);
        ok = FALSE;
    }

    g_free(out);
    g_free(remote);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * [4] ZFS 스냅샷 헬퍼 — 생성/삭제/목록 조회
 * ══════════════════════════════════════════════════════════════ */

/**
 * _list_auto_snapshots:
 * @vm_name: 대상 VM 이름
 *
 * `zfs list -t snapshot` 명령으로 해당 VM의 스냅샷을 조회하고,
 * pcv-auto-* 접두사가 있는 자동 백업 스냅샷만 필터링합니다.
 * creation 순으로 정렬되어 반환되므로 [0]=가장 오래된, [len-1]=가장 최신.
 *
 * ZFS 데이터셋: <zvol_pool>/<vm_name> (예: pcvpool/vms/web-prod)
 * 스냅샷 이름 형식: pcv-auto-20260321-143000
 *
 * Returns: (element-type gchar*) (transfer full): 스냅샷 이름 배열
 */
static GPtrArray *_list_auto_snapshots(const gchar *vm_name)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "name", "-s", "creation",
        "-t", "snapshot", "-r", dataset, NULL
    };

    gchar *stdout_buf = nullptr;
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err);
    g_free(dataset);

    if (!ok) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return result;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(g_strstrip(stdout_buf), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            /* zfs list 출력: "pcvpool/vms/web-prod@pcv-auto-20260321-143000"
             * '@' 이후가 스냅샷 이름이므로, pcv-auto- 접두사로 필터링 */
            const gchar *at = strrchr(*l, '@');
            if (at && g_str_has_prefix(at + 1, SNAP_PREFIX)) {
                g_ptr_array_add(result, g_strdup(at + 1));
            }
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return result;
}

/**
 * _create_snapshot:
 * @vm_name:   대상 VM 이름
 * @snap_name: 스냅샷 이름 (예: "pcv-auto-20260321-143000")
 *
 * `zfs snapshot <pool>/<vm>@<snap>` 명령을 실행하여 ZFS 스냅샷을 생성합니다.
 *
 * Returns: 성공 시 TRUE
 */
static gboolean _create_snapshot(const gchar *vm_name, const gchar *snap_name)
{
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *argv[] = {"zfs", "snapshot", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &err);
    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot create failed: %s — %s",
                     target, err ? err->message : (stderr_buf ? stderr_buf : "unknown"));
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot created: %s", target);
    }

    g_free(target);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return ok;
}

/**
 * _destroy_snapshot:
 * @vm_name:   대상 VM 이름
 * @snap_name: 삭제할 스냅샷 이름
 *
 * `zfs destroy <pool>/<vm>@<snap>` 명령으로 스냅샷을 삭제합니다.
 * retention_count 초과 시 가장 오래된 스냅샷을 정리할 때 호출됩니다.
 */
static void _destroy_snapshot(const gchar *vm_name, const gchar *snap_name)
{
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *argv[] = {"zfs", "destroy", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    /* zfs destroy is safe to call on non-existent snapshots — just log and continue */
    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &err);
    if (!ok) {
        /* "dataset does not exist" is expected in TOCTOU race — log but don't fail */
        if (stderr_buf && strstr(stderr_buf, "does not exist")) {
            PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot already gone (TOCTOU safe): %s", target);
        } else {
            PCV_LOG_WARN(BACKUP_LOG_DOM, "Snapshot destroy failed: %s — %s",
                         target, err ? err->message : (stderr_buf ? stderr_buf : "unknown"));
        }
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Snapshot destroyed (retention): %s", target);
    }

    g_free(target);
    g_free(stderr_buf);
    if (err) g_error_free(err);
}

/**
 * _list_snapshots_by_prefix:
 * @vm_name: 대상 VM 이름
 * @prefix:  필터할 스냅샷 접두사 (예: "pcv-s3-", "pcv-incr-")
 *
 * `_list_auto_snapshots` 의 prefix-파라미터화 버전 — 동일 argv/필터 로직을 그대로
 * 미러링하되 접두사만 인자로 받는다. creation 오름차순 정렬이므로
 * [0]=가장 오래된, [len-1]=가장 최신.
 *
 * AF-S4: pcv-s3-/pcv-incr- 스냅샷 리텐션 prune 에서 사용. (pcv-auto- 경로는 회귀 방지를 위해
 * 기존 `_list_auto_snapshots` 를 그대로 두고 건드리지 않는다.)
 *
 * Returns: (element-type gchar*) (transfer full): 스냅샷 이름 배열
 */
static GPtrArray *_list_snapshots_by_prefix(const gchar *vm_name,
                                            const gchar *prefix)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "name", "-s", "creation",
        "-t", "snapshot", "-r", dataset, NULL
    };

    gchar *stdout_buf = nullptr;
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err);
    g_free(dataset);

    if (!ok) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return result;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(g_strstrip(stdout_buf), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            /* zfs list 출력: "pcvpool/vms/web-prod@<prefix>YYYYMMDD-HHMMSS"
             * '@' 이후가 스냅샷 이름이므로, 주어진 prefix 로 필터링 */
            const gchar *at = strrchr(*l, '@');
            if (at && g_str_has_prefix(at + 1, prefix)) {
                g_ptr_array_add(result, g_strdup(at + 1));
            }
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return result;
}

/**
 * _prune_snapshots_by_prefix:
 * @vm_name:         대상 VM 이름
 * @prefix:          prune 대상 스냅샷 접두사 (예: "pcv-s3-", "pcv-incr-")
 * @retention_count: 보존할 최신 스냅샷 개수. 0 이하면 prune 비활성(무제한 = 명시적 opt-out)
 *
 * AF-S4: 기존 pcv-auto- 리텐션(_apply_policy_for_vm) 의 prune 루프를 프리픽스별로
 * 일반화한 best-effort 정리 함수. 주어진 VM·접두사의 스냅샷을 creation 순으로 조회한 뒤
 * 개수가 retention_count 를 초과하면 가장 오래된 것부터 `zfs destroy` 한다.
 *
 * off-by-one 안전: 루프 조건이 `len > retention_count` 이므로 len == retention_count 이면
 * 삭제하지 않는다. 삭제 대상은 항상 인덱스 0(=가장 오래된 것)이며, 가장 최신(index len-1,
 * 방금 만든 스냅샷 포함)은 절대 삭제되지 않는다. 이는 검증된 pcv-auto- prune 루프와 동일 패턴.
 *
 * 실패(zfs destroy 오류)해도 호출측 백업은 성공 처리 — _destroy_snapshot 이 내부에서
 * WARN 로그만 남기고 진행한다.
 */
/* PCV_SAFETY_CONTROL: backup-retention — retention_count 초과 스냅샷을 오래된 것부터 실제 zfs destroy (AF-S4/STO-2) */
static void _prune_snapshots_by_prefix(const gchar *vm_name,
                                       const gchar *prefix,
                                       gint retention_count)
{
    /* 0 이하 = prune 비활성 (무제한 보존, 명시적 opt-out) */
    if (retention_count <= 0)
        return;

    GPtrArray *snaps = _list_snapshots_by_prefix(vm_name, prefix);

    /* retention 초과 시 가장 오래된 것부터 삭제. 최신 N개는 절대 삭제하지 않는다. */
    while ((gint)snaps->len > retention_count) {
        const gchar *oldest = g_ptr_array_index(snaps, 0);
        _destroy_snapshot(vm_name, oldest);
        g_ptr_array_remove_index(snaps, 0);
    }

    g_ptr_array_unref(snaps);
}

/* ══════════════════════════════════════════════════════════════
 * [5] VM 이름 목록 조회 (libvirt)
 * ══════════════════════════════════════════════════════════════ */

/**
 * _get_all_vm_names:
 *
 * libvirt 연결 풀에서 커넥션을 빌려 모든 persistent 도메인의 이름을 조회합니다.
 * vm_name="*" 정책에서 모든 VM에 백업을 적용할 때 사용됩니다.
 *
 * 연결 풀 패턴:
 *   virt_conn_pool_acquire() → 사용 → virt_conn_pool_release()
 *   반드시 release를 호출해야 풀에 커넥션이 반환됩니다.
 *
 * Returns: (transfer full): GPtrArray of gchar* (VM 이름 목록)
 */
static GPtrArray *_get_all_vm_names(void)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) return names;

    virDomainPtr *domains = nullptr;
    int n = virConnectListAllDomains(conn, &domains,
                                     VIR_CONNECT_LIST_DOMAINS_PERSISTENT);
    if (n > 0 && domains) {
        for (int i = 0; i < n; i++) {
            const char *name = virDomainGetName(domains[i]);
            if (name) g_ptr_array_add(names, g_strdup(name));
            virDomainFree(domains[i]);
        }
        free(domains);
    }

    virt_conn_pool_release(conn);
    return names;
}

/* ══════════════════════════════════════════════════════════════
 * [6] 스냅샷 시간 파싱
 * ══════════════════════════════════════════════════════════════ */

/**
 * _parse_snap_time:
 * @snap_name: 스냅샷 이름 (예: "pcv-auto-20260321-143000")
 *
 * 스냅샷 이름에서 타임스탬프를 추출하여 epoch(time_t)로 변환합니다.
 * interval_hours 경과 여부를 판단할 때 사용됩니다.
 *
 * 파싱 형식: "pcv-auto-" + "YYYYMMDD" + "-" + "HHMMSS"
 * strptime() 대신 수동 파싱하여 이식성을 확보합니다 (일부 libc에서 미지원).
 *
 * Returns: epoch 시간, 파싱 실패 시 0
 */
static time_t _parse_snap_time(const gchar *snap_name)
{
    /* snap_name = "pcv-auto-20260321-143000" */
    if (!g_str_has_prefix(snap_name, SNAP_PREFIX)) return 0;

    const gchar *ts = snap_name + strlen(SNAP_PREFIX);
    /* ts = "20260321-143000" (15 chars) */
    if (strlen(ts) < 15) return 0;

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));

    /* strptime 대신 수동 파싱 (이식성) */
    gchar buf[5];

    /* YYYY */
    memcpy(buf, ts, 4); buf[4] = '\0';
    tm_val.tm_year = atoi(buf) - 1900;

    /* MM */
    memcpy(buf, ts + 4, 2); buf[2] = '\0';
    tm_val.tm_mon = atoi(buf) - 1;

    /* DD */
    memcpy(buf, ts + 6, 2); buf[2] = '\0';
    tm_val.tm_mday = atoi(buf);

    /* ts[8] == '-' */

    /* HH */
    memcpy(buf, ts + 9, 2); buf[2] = '\0';
    tm_val.tm_hour = atoi(buf);

    /* MM */
    memcpy(buf, ts + 11, 2); buf[2] = '\0';
    tm_val.tm_min = atoi(buf);

    /* SS */
    memcpy(buf, ts + 13, 2); buf[2] = '\0';
    tm_val.tm_sec = atoi(buf);

    tm_val.tm_isdst = -1;  /* -1: mktime이 DST를 자동 결정하도록 지시 */
    return mktime(&tm_val);
}

/* ══════════════════════════════════════════════════════════════
 * [7] 정책 적용 로직 — 스냅샷 생성 + 보존 정리
 * ══════════════════════════════════════════════════════════════ */

/**
 * _apply_policy_for_vm:
 * @policy:  적용할 백업 정책
 * @vm_name: 대상 VM 이름
 *
 * 단일 VM에 대해 정책을 적용합니다:
 *   1) 기존 pcv-auto-* 스냅샷 목록 조회
 *   2) 마지막 스냅샷으로부터 interval_hours 이상 경과했으면 새 스냅샷 생성
 *   3) 총 스냅샷 수가 retention_count를 초과하면 가장 오래된 것부터 삭제
 *
 * 예) interval_hours=6, retention_count=7이면:
 *   6시간마다 스냅샷을 찍고, 최근 7개만 유지합니다 (42시간분 보존).
 */
static void _apply_policy_for_vm(const PcvBackupPolicy *policy,
                                  const gchar *vm_name)
{
    GPtrArray *snaps = _list_auto_snapshots(vm_name);

    /* 마지막 스냅샷 시간 확인 */
    time_t last_snap_time = 0;
    if (snaps->len > 0) {
        const gchar *newest = g_ptr_array_index(snaps, snaps->len - 1);
        last_snap_time = _parse_snap_time(newest);
    }

    time_t now = time(NULL);
    gdouble diff_hours = difftime(now, last_snap_time) / 3600.0;

    /* interval 초과 시 새 스냅샷 생성 (fsfreeze 래핑) */
    if (diff_hours >= (gdouble)policy->interval_hours) {
        struct tm *tm_now = localtime(&now);
        gchar snap_name[64];
        g_snprintf(snap_name, sizeof(snap_name),
                   SNAP_PREFIX "%04d%02d%02d-%02d%02d%02d",
                   tm_now->tm_year + 1900, tm_now->tm_mon + 1,
                   tm_now->tm_mday, tm_now->tm_hour,
                   tm_now->tm_min, tm_now->tm_sec);

        /* Pre-backup: 게스트 파일시스템 동결 (앱 정합 스냅샷) */
        _fsfreeze_vm(vm_name);

        gboolean snap_ok = _create_snapshot(vm_name, snap_name);

        /* Post-backup: 게스트 파일시스템 해제 (반드시 실행) */
        _fsthaw_vm(vm_name);

        if (snap_ok) {
            /* 스냅샷 무결성 검증 */
            const gchar *pool = pcv_config_get_zvol_pool();
            gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);
            _verify_snapshot(snap_full);
            g_free(snap_full);

            g_ptr_array_add(snaps, g_strdup(snap_name));
        }
    }

    /* retention 초과 시 가장 오래된 스냅샷 삭제 */
    while ((gint)snaps->len > policy->retention_count) {
        const gchar *oldest = g_ptr_array_index(snaps, 0);
        _destroy_snapshot(vm_name, oldest);
        g_ptr_array_remove_index(snaps, 0);
    }

    g_ptr_array_unref(snaps);
}

/* ══════════════════════════════════════════════════════════════
 * [8] GLib 타이머 콜백
 * ══════════════════════════════════════════════════════════════ */

/**
 * _backup_worker:
 * @task: GTask 인스턴스
 * @src:  미사용
 * @data: 미사용
 * @c:    GCancellable (미사용)
 *
 * GTask 워커 스레드에서 실행되는 백업 로직입니다.
 * 메인 루프를 블로킹하지 않도록 pcv_spawn_sync() 호출을 이 스레드에서 수행합니다.
 * 정책 배열의 깊은 복사본을 사용하여 뮤텍스 점유 시간을 최소화합니다.
 */
static void
_backup_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)task; (void)src; (void)data; (void)c;

    /* 뮤텍스를 짧게 잡고 정책 깊은 복사 — 실제 백업은 뮤텍스 밖에서 수행 */
    g_mutex_lock(&g_policy_mutex);
    GPtrArray *snapshot = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (p->enabled)
            g_ptr_array_add(snapshot, _policy_dup(p));
    }
    g_mutex_unlock(&g_policy_mutex);

    /* 뮤텍스 해제 후 백업 수행 — 메인 루프 비블로킹 */
    for (guint i = 0; i < snapshot->len; i++) {
        PcvBackupPolicy *policy = g_ptr_array_index(snapshot, i);

        if (g_strcmp0(policy->vm_name, "*") == 0) {
            /* 전체 VM 대상 */
            GPtrArray *vms = _get_all_vm_names();
            for (guint j = 0; j < vms->len; j++) {
                const gchar *vm = g_ptr_array_index(vms, j);
                _apply_policy_for_vm(policy, vm);
            }
            g_ptr_array_unref(vms);
        } else {
            _apply_policy_for_vm(policy, policy->vm_name);
        }
    }

    g_ptr_array_unref(snapshot);
}

/**
 * _backup_check_cb:
 * @user_data: 미사용 (__attribute__((unused)))
 *
 * 5분(CHECK_INTERVAL)마다 GMainLoop에서 호출되는 타이머 콜백입니다.
 * GTask를 생성하여 워커 스레드에서 백업을 수행합니다 (메인 루프 블로킹 방지).
 *
 * Returns: G_SOURCE_CONTINUE — 타이머를 계속 유지 (G_SOURCE_REMOVE면 타이머 해제)
 */
static gboolean _backup_check_cb(gpointer user_data __attribute__((unused)))
{
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_run_in_thread(task, _backup_worker);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════
 * [9] 공개 API — 초기화 / 종료
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_backup_scheduler_init:
 *
 * 백업 스케줄러를 초기화합니다:
 *   1) GMutex 초기화
 *   2) 정책 배열 생성 (GPtrArray)
 *   3) 정책 파일 로드 (_policies_load)
 *   4) 5분 간격 GLib 타이머 등록
 *
 * main.c에서 다른 모듈(config, virt_conn_pool) 초기화 이후 호출합니다.
 */
void pcv_backup_scheduler_init(void)
{
    g_mutex_init(&g_policy_mutex);
    g_policies = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);

    _policies_load();

    g_timer_id = g_timeout_add_seconds(CHECK_INTERVAL, _backup_check_cb, NULL);
    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Backup scheduler started (%ds interval, %u policies)",
                 CHECK_INTERVAL, g_policies->len);
}

/**
 * pcv_backup_scheduler_shutdown:
 *
 * 타이머를 해제하고 정책 배열 메모리를 정리합니다.
 * g_source_remove()로 타이머를 먼저 제거하여 콜백이 더 이상 발화하지 않도록 합니다.
 */
void pcv_backup_scheduler_shutdown(void)
{
    if (g_timer_id > 0) {
        g_source_remove(g_timer_id);
        g_timer_id = 0;
    }

    g_mutex_lock(&g_policy_mutex);
    if (g_policies) {
        g_ptr_array_unref(g_policies);
        g_policies = nullptr;
    }
    if (g_vm_inflight) {
        g_hash_table_destroy(g_vm_inflight);
        g_vm_inflight = nullptr;
    }
    g_mutex_unlock(&g_policy_mutex);
    g_mutex_clear(&g_policy_mutex);

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Backup scheduler shut down");
}

/* ══════════════════════════════════════════════════════════════
 * [10] 공개 API — 정책 CRUD (set/delete/list)
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_backup_policy_set:
 * @vm_name:         대상 VM 이름 ("*"이면 전체 VM)
 * @interval_hours:  스냅샷 생성 주기 (시간, 최소 1)
 * @retention_count: 보존할 스냅샷 최대 개수 (최소 1)
 * @error:           GError 반환
 *
 * 새 정책을 추가하거나, 동일 vm_name 정책이 있으면 갱신합니다 (upsert 패턴).
 * 변경 후 즉시 JSON 파일에 저장합니다 (디스크 영속화).
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_backup_policy_set(const gchar *vm_name,
                                gint         interval_hours,
                                gint         retention_count,
                                GError     **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }
    if (interval_hours < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interval_hours must be >= 1");
        return FALSE;
    }
    if (retention_count < 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "retention_count must be >= 1");
        return FALSE;
    }

    g_mutex_lock(&g_policy_mutex);

    /* 기존 정책 갱신 또는 신규 추가 */
    gboolean found = FALSE;
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (g_strcmp0(p->vm_name, vm_name) == 0) {
            p->interval_hours  = interval_hours;
            p->retention_count = retention_count;
            p->enabled         = TRUE;
            found = TRUE;
            break;
        }
    }

    if (!found) {
        PcvBackupPolicy *p = g_new0(PcvBackupPolicy, 1);
        p->vm_name         = g_strdup(vm_name);
        p->interval_hours  = interval_hours;
        p->retention_count = retention_count;
        p->enabled         = TRUE;
        g_ptr_array_add(g_policies, p);
    }

    _policies_save_unlocked();
    g_mutex_unlock(&g_policy_mutex);

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Policy %s: vm=%s interval=%dh retention=%d",
                 found ? "updated" : "created",
                 vm_name, interval_hours, retention_count);
    return TRUE;
}

/**
 * pcv_backup_policy_delete:
 * @vm_name: 삭제할 정책의 VM 이름
 * @error:   GError 반환
 *
 * 해당 VM의 백업 정책을 삭제합니다.
 * 정책이 없으면 에러를 반환합니다 (멱등성 아님 — 의도적 설계).
 *
 * 주의: 기존 스냅샷은 삭제하지 않습니다 (정책만 제거).
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_backup_policy_delete(const gchar *vm_name, GError **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }

    g_mutex_lock(&g_policy_mutex);

    gboolean found = FALSE;
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);
        if (g_strcmp0(p->vm_name, vm_name) == 0) {
            g_ptr_array_remove_index(g_policies, i);
            found = TRUE;
            break;
        }
    }

    if (found) {
        _policies_save_unlocked();
    }

    g_mutex_unlock(&g_policy_mutex);

    if (!found) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No policy found for vm '%s'", vm_name);
        return FALSE;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Policy deleted: vm=%s", vm_name);
    return TRUE;
}

/**
 * pcv_backup_policy_list:
 *
 * 현재 등록된 모든 정책의 깊은 복사본을 반환합니다.
 * 뮤텍스 밖에서 안전하게 사용할 수 있도록 _policy_dup()으로 각 항목을 복제합니다.
 *
 * Returns: (transfer full): GPtrArray of PcvBackupPolicy*
 *   호출자가 g_ptr_array_unref()로 해제해야 합니다.
 */
GPtrArray *pcv_backup_policy_list(void)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_backup_policy_free);

    g_mutex_lock(&g_policy_mutex);
    for (guint i = 0; i < g_policies->len; i++) {
        PcvBackupPolicy *src = g_ptr_array_index(g_policies, i);
        g_ptr_array_add(result, _policy_dup(src));
    }
    g_mutex_unlock(&g_policy_mutex);

    return result;
}

/* ══════════════════════════════════════════════════════════════
 * [11] 공개 API — 이력 조회 / 복원
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_backup_history:
 * @vm_name: 조회할 VM 이름
 *
 * 해당 VM의 자동 백업(pcv-auto-*) 스냅샷 이름 목록을 반환합니다.
 * creation 순으로 정렬되어 있습니다.
 *
 * Returns: (transfer full): GPtrArray of gchar* (g_ptr_array_unref 필요)
 */
GPtrArray *pcv_backup_history(const gchar *vm_name)
{
    if (!vm_name || *vm_name == '\0') {
        return g_ptr_array_new_with_free_func(g_free);
    }
    return _list_auto_snapshots(vm_name);
}

/**
 * pcv_backup_history_paged:
 * @vm_name: 조회할 VM 이름
 * @offset:  건너뛸 항목 수 (0-based)
 * @limit:   반환할 최대 항목 수 (0이면 전체)
 * @total_out: (out) (nullable): 전체 스냅샷 개수를 반환
 *
 * 히스토리 페이지네이션 — offset/limit 지원.
 * RPC/REST에서 대량 스냅샷 이력을 페이지 단위로 조회할 때 사용합니다.
 * creation 순(오래된 순)으로 정렬되어 있으며, offset/limit으로 잘라냅니다.
 *
 * 예) offset=10, limit=5 → 11~15번째 스냅샷 반환
 *
 * Returns: (transfer full): GPtrArray of gchar* (g_ptr_array_unref 필요)
 */
GPtrArray *pcv_backup_history_paged(const gchar *vm_name,
                                     guint        offset,
                                     guint        limit,
                                     guint       *total_out)
{
    GPtrArray *all = pcv_backup_history(vm_name);

    if (total_out)
        *total_out = all->len;

    /* limit=0이면 전체 반환 (기존 동작 호환) */
    if (limit == 0 && offset == 0) {
        return all;
    }

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);

    guint start = (offset < all->len) ? offset : all->len;
    guint end   = (limit > 0) ? MIN(start + limit, all->len) : all->len;

    for (guint i = start; i < end; i++) {
        const gchar *snap = g_ptr_array_index(all, i);
        g_ptr_array_add(result, g_strdup(snap));
    }

    g_ptr_array_unref(all);
    return result;
}

/**
 * pcv_backup_restore:
 * @vm_name:       복원할 VM 이름
 * @snapshot_name: 복원할 스냅샷 이름 (예: "pcv-auto-20260321-143000")
 * @error:         GError 반환
 *
 * ZFS 볼륨을 지정된 스냅샷 상태로 안전하게 되돌립니다 (B8-C2, B8-C3).
 *
 * 안전성:
 *   1. per-VM 락 획득 — 동일 VM에 대한 동시 backup/restore/replicate 차단
 *   2. VM 상태 확인 — RUNNING/PAUSED 이면 graceful shutdown → force destroy
 *   3. `zfs rollback -r` 실행
 *   4. 원래 실행 중이었으면 VM 재시작
 *   5. 락 해제
 *
 * -r 플래그:
 *   지정 스냅샷 이후에 생성된 모든 스냅샷이 삭제됩니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_backup_restore(const gchar *vm_name,
                             const gchar *snapshot_name,
                             GError     **error)
{
    if (!vm_name || !snapshot_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name and snapshot_name are required");
        return FALSE;
    }

    /* B8-C3: per-VM 백업 작업 락 획득 */
    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *target = g_strdup_printf("%s/%s@%s", pool, vm_name, snapshot_name);

    PCV_LOG_INFO(BACKUP_LOG_DOM, "Restoring: %s", target);

    /* B8-C2: VM 상태 확인 + 안전 종료.
     * 실행 중 VM의 zvol을 rollback하면 게스트 FS가 즉시 코럽션됩니다. */
    gboolean was_running = FALSE;
    virConnectPtr conn = virt_conn_pool_acquire();
    if (conn) {
        virDomainPtr dom = virDomainLookupByName(conn, vm_name);
        if (dom) {
            int state = 0, reason = 0;
            if (virDomainGetState(dom, &state, &reason, 0) == 0) {
                was_running = (state == VIR_DOMAIN_RUNNING ||
                               state == VIR_DOMAIN_PAUSED);
            }
            if (was_running) {
                PCV_LOG_INFO(BACKUP_LOG_DOM,
                             "Restore: shutting down VM '%s' before rollback",
                             vm_name);
                virDomainShutdown(dom);
                for (int i = 0; i < 50; i++) { /* 최대 5초 */
                    g_usleep(100 * 1000);
                    if (virDomainGetState(dom, &state, &reason, 0) != 0) break;
                    if (state == VIR_DOMAIN_SHUTOFF) break;
                }
                if (virDomainGetState(dom, &state, &reason, 0) == 0 &&
                    state != VIR_DOMAIN_SHUTOFF) {
                    PCV_LOG_WARN(BACKUP_LOG_DOM,
                                 "Restore: graceful shutdown timeout — "
                                 "force-destroying VM '%s'", vm_name);
                    virDomainDestroy(dom);
                    for (int i = 0; i < 50; i++) {
                        g_usleep(100 * 1000);
                        if (virDomainGetState(dom, &state, &reason, 0) != 0) break;
                        if (state == VIR_DOMAIN_SHUTOFF) break;
                    }
                }
            }
            virDomainFree(dom);
        }
        /* conn은 rollback 후 재기동 단계에서 재사용 */
    }

    const gchar *argv[] = {"zfs", "rollback", "-r", target, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        const gchar *msg = local_err ? local_err->message
                         : (stderr_buf ? stderr_buf : "ZFS rollback failed");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", msg);
        PCV_LOG_WARN(BACKUP_LOG_DOM, "Restore failed: %s — %s", target, msg);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM, "Restore complete: %s", target);
    }

    /* 원래 실행 중이었으면 VM 재시작 (rollback 성공/실패 무관 시도) */
    if (conn && was_running) {
        gboolean restart_ok = FALSE;
        virDomainPtr dom = virDomainLookupByName(conn, vm_name);
        if (dom) {
            PCV_LOG_INFO(BACKUP_LOG_DOM,
                         "Restore: restarting VM '%s'", vm_name);
            for (int attempt = 0; attempt < 10; attempt++) {
                if (virDomainCreate(dom) == 0) {
                    restart_ok = TRUE;
                    break;
                }
                g_usleep(200 * 1000);
            }
            if (!restart_ok) {
                PCV_LOG_WARN(BACKUP_LOG_DOM,
                             "Restore: failed to restart VM '%s' after rollback",
                             vm_name);
                if (ok) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Rollback completed but VM '%s' did not restart",
                                vm_name);
                    ok = FALSE;
                }
            }
            virDomainFree(dom);
        }
    }
    if (conn) virt_conn_pool_release(conn);

    g_free(target);
    g_free(stderr_buf);
    if (local_err) g_error_free(local_err);

    _vm_backup_unlock(vm_name);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * [12] 증분 백업 — 이전 스냅샷 대비 증분 스트림 저장
 * ══════════════════════════════════════════════════════════════ */

#define BACKUP_DIR_DEFAULT "/var/lib/purecvisor/backups"
#define INCR_SNAP_PREFIX   "pcv-incr-"   /* STO-2/AF-S4: pcv- 시스템 예약 네임스페이스 */

/**
 * _ensure_backup_dir:
 *
 * 백업 저장 디렉터리가 존재하지 않으면 생성합니다.
 * Returns: 디렉터리 경로 (정적 문자열, 해제 불필요)
 */
static const gchar *_ensure_backup_dir(void)
{
    const gchar *dir = pcv_config_get_string("backup", "backup_dir",
                                              BACKUP_DIR_DEFAULT);
    g_mkdir_with_parents(dir, 0750);
    return dir;
}

/**
 * _list_all_snapshots:
 * @vm_name: 대상 VM 이름
 *
 * 해당 VM의 모든 ZFS 스냅샷(접두사 무관)을 creation 순으로 반환합니다.
 * 증분 백업과 검증에서 최신 스냅샷을 찾을 때 사용합니다.
 *
 * Returns: (element-type gchar*) (transfer full): 스냅샷 full path 배열
 */
static GPtrArray *_list_all_snapshots(const gchar *vm_name)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    const gchar *argv[] = {
        "zfs", "list", "-H", "-o", "name", "-s", "creation",
        "-t", "snapshot", "-r", dataset, NULL
    };

    gchar *stdout_buf = nullptr;
    gchar *stderr_buf = nullptr;
    GError *err = nullptr;

    gboolean ok = pcv_spawn_sync(argv, &stdout_buf, &stderr_buf, &err);
    g_free(dataset);

    if (!ok) {
        g_free(stdout_buf);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        return result;
    }

    if (stdout_buf) {
        gchar **lines = g_strsplit(g_strstrip(stdout_buf), "\n", -1);
        for (gchar **l = lines; *l && **l; l++) {
            g_ptr_array_add(result, g_strdup(*l));
        }
        g_strfreev(lines);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    if (err) g_error_free(err);
    return result;
}

/**
 * pcv_backup_incremental:
 * @vm_name: 대상 VM 이름
 * @error:   GError 반환
 *
 * 증분 백업을 수행합니다:
 *   1. 최신 스냅샷 조회
 *   2. 새 증분 스냅샷 생성 (pcv-incr-YYYYMMDD-HHMMSS)
 *   3. zfs send -i <prev> <new> > 파일  (이전 스냅샷 있을 때)
 *      또는 zfs send <new> > 파일       (이전 스냅샷 없을 때 — 풀 백업)
 *   4. 파일 크기 기록
 *
 * Returns: (transfer full): 결과 JsonObject. 실패 시 NULL.
 */
JsonObject *pcv_backup_incremental(const gchar *vm_name, GError **error)
{
    if (!vm_name || *vm_name == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return NULL;
    }

    /* B8-C3: per-VM 백업 작업 락 */
    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return NULL;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    const gchar *backup_dir = _ensure_backup_dir();

    /* B8-W7: 백업 대상 파일시스템 사용률 점검 */
    _check_backup_disk_usage(backup_dir);

    /* 1. 기존 스냅샷 목록 조회 */
    GPtrArray *snaps = _list_all_snapshots(vm_name);
    const gchar *prev_snap = nullptr;
    if (snaps->len > 0) {
        prev_snap = g_ptr_array_index(snaps, snaps->len - 1);
    }

    /* 2. 새 증분 스냅샷 생성 */
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    gchar snap_name[64];
    g_snprintf(snap_name, sizeof(snap_name),
               INCR_SNAP_PREFIX "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);

    gchar *new_snap = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *snap_argv[] = {"zfs", "snapshot", new_snap, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(snap_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create snapshot %s: %s",
                    new_snap, local_err ? local_err->message
                                        : (stderr_buf ? stderr_buf : "unknown"));
        g_free(new_snap);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        g_ptr_array_unref(snaps);
        _vm_backup_unlock(vm_name);
        return NULL;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    /* 3. 증분 스트림 → 파일 저장 */
    gchar ts_buf[32];
    g_snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);

    gchar *out_file = g_strdup_printf("%s/%s_incr_%s.zfs",
                                       backup_dir, vm_name, ts_buf);

    gchar *cmd = nullptr;
    gchar *base_snap_name = nullptr;
    if (prev_snap) {
        /* 증분 전송 */
        gchar *q_prev = g_shell_quote(prev_snap);
        gchar *q_new  = g_shell_quote(new_snap);
        gchar *q_file = g_shell_quote(out_file);
        cmd = g_strdup_printf("zfs send -i %s %s > %s", q_prev, q_new, q_file);
        /* prev_snap에서 @ 이후가 스냅샷 이름 */
        const gchar *at = strrchr(prev_snap, '@');
        base_snap_name = g_strdup(at ? at + 1 : prev_snap);
        g_free(q_prev);
        g_free(q_new);
        g_free(q_file);
    } else {
        /* 풀 전송 (이전 스냅샷 없음) */
        gchar *q_new  = g_shell_quote(new_snap);
        gchar *q_file = g_shell_quote(out_file);
        cmd = g_strdup_printf("zfs send %s > %s", q_new, q_file);
        g_free(q_new);
        g_free(q_file);
    }

    stderr_buf = nullptr;
    const gchar *sh_argv[] = {"/bin/sh", "-c", cmd, NULL};
    ok = pcv_spawn_sync(sh_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "zfs send failed: %s",
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(cmd);
        g_free(out_file);
        g_free(new_snap);
        g_free(base_snap_name);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        g_ptr_array_unref(snaps);
        _vm_backup_unlock(vm_name);
        return NULL;
    }
    g_free(cmd);
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    /* 4. 파일 크기 측정 */
    struct stat st;
    gint64 file_size = 0;
    if (stat(out_file, &st) == 0) {
        file_size = (gint64)st.st_size;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Incremental backup: %s → %s (%s, %" G_GINT64_FORMAT " bytes)",
                 vm_name, snap_name,
                 prev_snap ? "incremental" : "full",
                 file_size);

    /* 5. 결과 JSON 구성 */
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "snapshot", snap_name);
    json_object_set_string_member(result, "base_snapshot",
                                  base_snap_name ? base_snap_name : "none");
    json_object_set_string_member(result, "file", out_file);
    json_object_set_int_member(result, "size_bytes", file_size);
    json_object_set_string_member(result, "mode",
                                  prev_snap ? "incremental" : "full");

    g_free(new_snap);
    g_free(out_file);
    g_free(base_snap_name);
    g_ptr_array_unref(snaps);

    /* AF-S4: pcv-incr- 스냅샷 리텐션 — zfs send 완료 후 prune (best-effort).
     * send 전에 prune 하면 증분 base(prev_snap)를 지워 send -i 가 깨질 수 있으므로
     * 반드시 send 이후에 실행한다. 최신 N개(방금 만든 것 포함) 보존, 초과분만 오래된
     * 순으로 삭제. prune 실패는 WARN 만 남기고 백업 자체는 이미 성공 처리됨. */
    _prune_snapshots_by_prefix(vm_name, INCR_SNAP_PREFIX,
                               pcv_config_get_int("backup", "incr_retention_count", 7));

    _vm_backup_unlock(vm_name);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 * [13] 백업 검증 — 스냅샷 존재 확인 + 무결성 dry-run
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_backup_verify:
 * @vm_name:       대상 VM 이름
 * @snapshot_name: 검증할 스냅샷 이름
 * @error:         GError 반환
 *
 * 스냅샷 검증:
 *   1. zfs list -t snapshot 으로 존재 확인
 *   2. zfs send -n (dry-run)으로 무결성 검증
 *   3. zfs get used 로 크기 조회
 *
 * Returns: (transfer full): 결과 JsonObject. 실패 시 NULL.
 */
JsonObject *pcv_backup_verify(const gchar *vm_name,
                              const gchar *snapshot_name,
                              GError     **error)
{
    if (!vm_name || !snapshot_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name and snapshot_name are required");
        return NULL;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snapshot_name);

    /* 1. 스냅샷 존재 확인 */
    const gchar *list_argv[] = {
        "zfs", "list", "-t", "snapshot", "-H", snap_full, NULL
    };
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_sync(list_argv, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Snapshot not found: %s", snap_full);
        g_free(snap_full);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        return NULL;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    /* 2. 무결성 검증 — zfs send -n (dry-run) */
    const gchar *send_argv[] = {
        "zfs", "send", "-n", snap_full, NULL
    };
    stderr_buf = nullptr;
    ok = pcv_spawn_sync(send_argv, NULL, &stderr_buf, &local_err);

    const gchar *integrity = ok ? "ok" : "failed";

    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Integrity check failed for %s: %s",
                     snap_full,
                     local_err ? local_err->message
                               : (stderr_buf ? stderr_buf : "unknown"));
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    /* 3. 크기 조회 — zfs get -H -o value used */
    const gchar *size_argv[] = {
        "zfs", "get", "-H", "-o", "value", "-p", "used", snap_full, NULL
    };
    gchar *stdout_buf = nullptr;
    stderr_buf = nullptr;
    gint64 size_bytes = 0;

    if (pcv_spawn_sync(size_argv, &stdout_buf, &stderr_buf, &local_err)) {
        if (stdout_buf) {
            size_bytes = g_ascii_strtoll(g_strstrip(stdout_buf), NULL, 10);
        }
    }
    g_free(stdout_buf);
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "Verify: %s — integrity=%s size=%" G_GINT64_FORMAT,
                 snap_full, integrity, size_bytes);

    /* 4. 결과 JSON */
    JsonObject *result = json_object_new();
    json_object_set_boolean_member(result, "verified", ok);
    json_object_set_string_member(result, "snapshot", snapshot_name);
    json_object_set_int_member(result, "size_bytes", size_bytes);
    json_object_set_string_member(result, "integrity", integrity);

    g_free(snap_full);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 * [14] 크로스 노드 백업 복제 — ZFS send/recv over SSH
 * ══════════════════════════════════════════════════════════════ */

/**
 * _remote_snapshot_exists:
 * @ssh_user: SSH 사용자
 * @target:   대상 노드 IP
 * @snap:     ZFS 스냅샷 full path
 *
 * SSH를 통해 원격 노드에 해당 스냅샷이 존재하는지 확인합니다.
 */
static gboolean _remote_snapshot_exists(const gchar *ssh_user,
                                         const gchar *target,
                                         const gchar *snap)
{
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, target);
    const gchar *argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-t", "snapshot", "-H", snap,
        NULL
    };
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);
    if (err) g_error_free(err);
    g_free(remote);
    return ok;
}

/* ── R-5: 원격 사이트 독립 보존 정책 ─────────────────────────── */

/**
 * _enforce_remote_retention — 원격 노드의 오래된 스냅샷을 삭제하여 보존 정책 적용
 *
 * 복제 완료 후 원격 노드에서 해당 VM의 스냅샷 목록을 생성 시간 역순으로 조회하고,
 * remote_retention 개수를 초과하는 오래된 스냅샷을 zfs destroy로 삭제한다.
 *
 * @param pool             ZFS 풀 이름
 * @param vm_name          VM 이름
 * @param peer_ssh         원격 노드 IP/호스트명
 * @param ssh_user         SSH 사용자명
 * @param remote_retention 원격에 보존할 최대 스냅샷 수
 */
static void
_enforce_remote_retention(const gchar *pool, const gchar *vm_name,
                          const gchar *peer_ssh, const gchar *ssh_user,
                          gint remote_retention)
{
    if (remote_retention <= 0) return;

    gchar *dataset_str = g_strdup_printf("%s/%s", pool, vm_name);
    gchar *dataset_prefix = g_strdup_printf("%s@", dataset_str);
    gchar *remote = g_strdup_printf("%s@%s", ssh_user, peer_ssh);
    const gchar *list_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "zfs", "list", "-H", "-o", "name", "-t", "snapshot",
        "-S", "creation", dataset_str,
        NULL
    };
    gchar *out = nullptr;
    GError *err = nullptr;

    if (!pcv_spawn_sync(list_argv, &out, NULL, &err)) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Failed to enforce remote retention for %s/%s on %s: %s",
                     pool, vm_name, peer_ssh,
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_free(out);
        g_free(remote);
        g_free(dataset_prefix);
        g_free(dataset_str);
        return;
    }

    gchar **lines = g_strsplit(out ? out : "", "\n", -1);
    gint seen = 0;
    gint destroyed = 0;
    gint failed = 0;

    for (guint i = 0; lines && lines[i]; i++) {
        gchar *snap = g_strstrip(lines[i]);
        if (!snap || *snap == '\0')
            continue;

        if (!g_str_has_prefix(snap, dataset_prefix)) {
            PCV_LOG_WARN(BACKUP_LOG_DOM,
                         "Skipping unexpected remote snapshot outside %s: %s",
                         dataset_str, snap);
            continue;
        }

        seen++;
        if (seen <= remote_retention)
            continue;

        const gchar *destroy_argv[] = {
            "ssh",
            "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-i", "/etc/purecvisor/cluster_id_ed25519",
            remote,
            "sudo", "zfs", "destroy", snap,
            NULL
        };
        GError *destroy_err = nullptr;
        if (pcv_spawn_sync(destroy_argv, NULL, NULL, &destroy_err)) {
            destroyed++;
        } else {
            failed++;
            PCV_LOG_WARN(BACKUP_LOG_DOM,
                         "Failed to destroy old remote snapshot %s on %s: %s",
                         snap, peer_ssh,
                         destroy_err ? destroy_err->message : "unknown");
        }
        if (destroy_err) g_error_free(destroy_err);
    }

    if (failed == 0) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Enforced remote retention (%d) for %s/%s on %s, destroyed=%d",
                     remote_retention, pool, vm_name, peer_ssh, destroyed);
    }

    if (err) g_error_free(err);
    g_strfreev(lines);
    g_free(out);
    g_free(remote);
    g_free(dataset_prefix);
    g_free(dataset_str);
}

/**
 * pcv_backup_replicate:
 * @vm_name:     대상 VM 이름
 * @target_node: 대상 노드 IP/호스트명
 * @ssh_user:    SSH 사용자명 (NULL이면 daemon.conf 기본값)
 * @error:       GError 반환
 *
 * 최신 스냅샷을 원격 노드로 ZFS send/recv로 복제합니다.
 *   1. 최신 스냅샷 조회
 *   2. 원격에 이전 스냅샷 존재 여부 확인 → 증분/풀 결정
 *   3. pcv_spawn_pipe_sync()로 zfs send stdout을 ssh zfs recv stdin에 직접 연결
 *   4. 결과 로깅
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_backup_replicate(const gchar *vm_name,
                              const gchar *target_node,
                              const gchar *ssh_user,
                              GError     **error)
{
    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name for replication");
        return FALSE;
    }
    if (!pcv_validate_remote_host(target_node)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid replication target node");
        return FALSE;
    }

    const gchar *user = ssh_user;
    if (!user || *user == '\0') {
        user = pcv_config_get_ssh_user();
    }
    if (!pcv_validate_ssh_user(user)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid replication SSH user");
        return FALSE;
    }

    /* B8-C3: per-VM 백업 작업 락 */
    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }

    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *dataset = g_strdup_printf("%s/%s", pool, vm_name);

    /* 1. 스냅샷 목록 조회 */
    GPtrArray *snaps = _list_all_snapshots(vm_name);
    if (snaps->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No snapshots found for VM '%s'", vm_name);
        g_ptr_array_unref(snaps);
        g_free(dataset);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }

    const gchar *latest = g_ptr_array_index(snaps, snaps->len - 1);
    const gchar *base = (snaps->len >= 2)
                        ? (const gchar *)g_ptr_array_index(snaps, snaps->len - 2)
                        : NULL;

    gint bw_mbps = pcv_config_get_int("cluster", "repl_bandwidth_mbps", 0);
    if (bw_mbps > 0) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "Replication bandwidth limit (%d Mbps) ignored in shell-free path",
                     bw_mbps);
    }

    /* 3. 증분 또는 풀 전송 결정 */
    gboolean incremental = base && _remote_snapshot_exists(user, target_node, base);
    gchar *remote = g_strdup_printf("%s@%s", user, target_node);
    const gchar *producer_full_argv[] = {
        "zfs", "send", latest,
        NULL
    };
    const gchar *producer_incremental_argv[] = {
        "zfs", "send", "-i", base, latest,
        NULL
    };
    const gchar *consumer_argv[] = {
        "ssh",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-i", "/etc/purecvisor/cluster_id_ed25519",
        remote,
        "sudo", "zfs", "recv", "-F", dataset,
        NULL
    };
    const gchar * const *producer_argv = incremental
                                         ? producer_incremental_argv
                                         : producer_full_argv;

    GTimer *timer = g_timer_new();
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    gboolean ok = pcv_spawn_pipe_sync(producer_argv, consumer_argv,
                                      NULL, &stderr_buf, &local_err);
    gdouble elapsed = g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s replication failed for %s → %s: %s",
                    incremental ? "Incremental" : "Full",
                    vm_name, target_node,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        if (local_err) g_error_free(local_err);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "Replication complete: %s → %s@%s (%s, %.1fs)",
                     vm_name, user, target_node,
                     incremental ? "incremental" : "full",
                     elapsed);

        /* 복제 후 무결성 검증 — 원격에 최신 스냅샷 존재 확인 */
        _verify_replication(latest, target_node, user);

        /* R-5: 원격 사이트 독립 보존 정책 적용 */
        gint remote_retention = pcv_config_get_int("backup", "remote_retention", 0);
        if (remote_retention > 0) {
            _enforce_remote_retention(pool, vm_name, target_node, user, remote_retention);
        }
    }

    g_free(stderr_buf);
    g_free(remote);
    g_free(dataset);
    g_ptr_array_unref(snaps);
    _vm_backup_unlock(vm_name);
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * [16] S3 호환 외부 백업 — ZFS 스냅샷 → gzip → S3 멀티파트 업로드
 *
 * AWS CLI를 래핑하여 S3 호환 오브젝트 스토리지에 백업을 전송합니다.
 * libsoup3 기반 직접 구현 대신 aws-cli를 사용하는 이유:
 *   - SigV4 서명, 멀티파트 재시도, 청크 체크섬을 aws-cli가 처리
 *   - 코드 복잡도 절감 (~250 LOC vs ~500+ LOC for native HTTP)
 *   - 기존 pcv_spawn_sync() 패턴과 일관됨
 *
 * 자격증명 소스: daemon.conf [backup] 섹션
 *   s3_endpoint    = https://s3.amazonaws.com
 *   s3_bucket      = pcv-backup
 *   s3_region      = ap-northeast-2
 *   s3_access_key  = AKIA...
 *   s3_secret_key  = wJalrX...
 *
 * 업로드 구조:
 *   s3://<bucket>/<prefix><vm>/<timestamp>/backup.zfs.gz   — 백업 데이터
 *   s3://<bucket>/<prefix><vm>/<timestamp>/metadata.json   — 메타데이터
 * ══════════════════════════════════════════════════════════════ */

#define S3_SNAP_PREFIX "pcv-s3-"   /* STO-2/AF-S4: pcv- 시스템 예약 네임스페이스 */
#define S3_TEMP_DIR    "/tmp"

/**
 * _s3_build_env — S3 자격증명을 환경 변수 배열로 빌드
 *
 * aws-cli는 환경 변수 AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,
 * AWS_DEFAULT_REGION 을 인식한다.
 *
 * Returns: (transfer full): NULL-terminated 환경 변수 문자열 배열
 *   호출자가 g_strfreev()로 해제해야 한다.
 */
static gchar **
_s3_build_env(const gchar *region)
{
    gchar *ak = pcv_config_get_secret("backup", "s3_access_key", NULL);
    gchar *sk = pcv_config_get_secret("backup", "s3_secret_key", NULL);
    const gchar *reg = (region && *region) ? region
                       : pcv_config_get_string("backup", "s3_region", "ap-northeast-2");

    /* Inherit current environ and add AWS creds */
    GPtrArray *env = g_ptr_array_new();
    gchar **environ_snapshot = g_get_environ();
    for (gchar **e = environ_snapshot; e && *e; e++) {
        g_ptr_array_add(env, g_strdup(*e));
    }
    g_strfreev(environ_snapshot);
    if (ak) { g_ptr_array_add(env, g_strdup_printf("AWS_ACCESS_KEY_ID=%s", ak)); g_free(ak); }
    if (sk) { g_ptr_array_add(env, g_strdup_printf("AWS_SECRET_ACCESS_KEY=%s", sk)); g_free(sk); }
    g_ptr_array_add(env, g_strdup_printf("AWS_DEFAULT_REGION=%s", reg));
    g_ptr_array_add(env, NULL);
    return (gchar **)g_ptr_array_free(env, FALSE);
}

/* ══════════════════════════════════════════════════════════════════════
 * BE-A12: S3 멀티파트 업로드 (>100MB 파일)
 *
 * aws s3 cp에 --expected-size를 지정하여 대용량 파일의 멀티파트 업로드를
 * 최적화한다. AWS CLI는 기본 8MB 파트 크기를 사용하며, expected-size
 * 힌트로 최적 파트 크기를 자동 계산한다.
 * ══════════════════════════════════════════════════════════════��═══════ */

#define S3_MULTIPART_THRESHOLD  (100 * 1024 * 1024)  /* 100MB */

/**
 * _s3_upload_multipart — 멀티파트 업로드로 대용량 파일을 S3에 전송
 *
 * @param local_path  로컬 파일 경로
 * @param s3_path     S3 오브젝트 키
 * @param endpoint    S3 엔드포인트 URL
 * @param bucket      S3 버킷 이름
 * @param region      AWS 리전
 * @param envp        (nullable) 이 자식에만 적용할 "KEY=VALUE" 환경변수 배열
 *                    (AWS 자격증명). 데몬 전역 environ은 건드리지 않는다.
 * @param error       GError 반환
 *
 * Returns: TRUE 성공
 */
static gboolean
_s3_upload_multipart(const gchar *local_path, const gchar *s3_path,
                     const gchar *endpoint, const gchar *bucket,
                     const gchar *region, const gchar * const *envp,
                     GError **error)
{
    gchar *s3_url = g_strdup_printf("s3://%s/%s", bucket, s3_path);

    /* 파일 크기 조회 (expected-size 힌트용) */
    struct stat st;
    gchar size_str[32] = "5368709120";  /* 기본 5GB 힌트 */
    if (g_stat(local_path, &st) == 0) {
        g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT, (gint64)st.st_size);
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"aws");
    g_ptr_array_add(argv, (gchar *)"s3");
    g_ptr_array_add(argv, (gchar *)"cp");
    g_ptr_array_add(argv, (gchar *)local_path);
    g_ptr_array_add(argv, s3_url);
    if (endpoint && *endpoint) {
        g_ptr_array_add(argv, (gchar *)"--endpoint-url");
        g_ptr_array_add(argv, (gchar *)endpoint);
    }
    if (region && *region) {
        g_ptr_array_add(argv, (gchar *)"--region");
        g_ptr_array_add(argv, (gchar *)region);
    }
    g_ptr_array_add(argv, (gchar *)"--expected-size");
    g_ptr_array_add(argv, size_str);
    g_ptr_array_add(argv, (gchar *)"--no-progress");
    g_ptr_array_add(argv, NULL);

    gchar *std_err = nullptr;
    GError *local_err = nullptr;
    gboolean ok = pcv_spawn_sync_env((const gchar * const *)argv->pdata,
                                      envp, NULL, &std_err, &local_err);
    if (!ok) {
        PCV_LOG_WARN(BACKUP_LOG_DOM,
                     "S3 multipart upload failed for %s: %s",
                     s3_url,
                     local_err ? local_err->message
                               : (std_err ? std_err : "unknown"));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "S3 multipart upload failed: %s — %s",
                    s3_url,
                    local_err ? local_err->message
                              : (std_err ? std_err : "unknown"));
        if (local_err) g_error_free(local_err);
    } else {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "S3 multipart upload complete: %s (size=%s)",
                     s3_url, size_str);
    }

    g_free(std_err);
    g_free(s3_url);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

/**
 * _s3_upload_file — aws-cli로 단일 파일을 S3에 업로드
 *
 * @param endpoint     S3 엔드포인트 URL
 * @param bucket       S3 버킷
 * @param s3_key       S3 오브젝트 키 (경로)
 * @param local_path   로컬 파일 경로
 * @param content_type MIME 타입 (예: "application/gzip")
 * @param region       AWS 리전 (resolve된 문자열, 멀티파트 --region 힌트용)
 * @param envp         (nullable) 이 자식에만 적용할 "KEY=VALUE" 환경변수 배열
 *                     (AWS 자격증명). 데몬 전역 environ은 건드리지 않는다.
 * @param error        GError 반환
 *
 * Returns: TRUE 성공
 */
static gboolean
_s3_upload_file(const gchar *endpoint, const gchar *bucket,
                const gchar *s3_key, const gchar *local_path,
                const gchar *content_type, const gchar *region,
                const gchar * const *envp,
                GError **error)
{
    /* BE-A12: 100MB 초과 파일은 멀티파트 업로드로 자동 전환 */
    struct stat mp_st;
    if (g_stat(local_path, &mp_st) == 0 && mp_st.st_size > S3_MULTIPART_THRESHOLD) {
        PCV_LOG_INFO(BACKUP_LOG_DOM,
                     "File %s exceeds 100MB (%" G_GINT64_FORMAT "), using multipart upload",
                     local_path, (gint64)mp_st.st_size);
        /* region은 호출부에서 resolve해 명시 전달 (전역 environ 역참조 제거) */
        return _s3_upload_multipart(local_path, s3_key, endpoint, bucket,
                                     region, envp, error);
    }

    gchar *s3_uri = g_strdup_printf("s3://%s/%s", bucket, s3_key);

    /* aws s3 cp <local> <s3_uri> --endpoint-url <endpoint> [--content-type] */
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gchar *)"aws");
    g_ptr_array_add(argv, (gchar *)"s3");
    g_ptr_array_add(argv, (gchar *)"cp");
    g_ptr_array_add(argv, (gchar *)local_path);
    g_ptr_array_add(argv, s3_uri);
    if (endpoint && *endpoint) {
        g_ptr_array_add(argv, (gchar *)"--endpoint-url");
        g_ptr_array_add(argv, (gchar *)endpoint);
    }
    if (content_type && *content_type) {
        g_ptr_array_add(argv, (gchar *)"--content-type");
        g_ptr_array_add(argv, (gchar *)content_type);
    }
    g_ptr_array_add(argv, NULL);

    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;
    gboolean ok = pcv_spawn_sync_env((const gchar * const *)argv->pdata,
                                      envp, NULL, &stderr_buf, &local_err);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "S3 upload failed: %s — %s",
                    s3_uri,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        if (local_err) g_error_free(local_err);
    }

    g_free(stderr_buf);
    g_free(s3_uri);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

gboolean
pcv_backup_export_s3(const gchar *vm_name,
                      const gchar *s3_endpoint,
                      const gchar *s3_bucket,
                      const gchar *s3_key_prefix,
                      GError     **error)
{
    if (!vm_name || !*vm_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "vm_name is required");
        return FALSE;
    }

    /* B8-C3: per-VM 백업 작업 락 */
    if (!_vm_backup_try_lock(vm_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Another backup operation is in progress for VM '%s'",
                    vm_name);
        return FALSE;
    }

    /* ── 1. S3 설정 확인 ────────────────────────── */
    const gchar *endpoint = (s3_endpoint && *s3_endpoint) ? s3_endpoint
        : pcv_config_get_string("backup", "s3_endpoint", "");
    const gchar *bucket = (s3_bucket && *s3_bucket) ? s3_bucket
        : pcv_config_get_string("backup", "s3_bucket", "");
    const gchar *prefix = (s3_key_prefix && *s3_key_prefix) ? s3_key_prefix
        : pcv_config_get_string("backup", "s3_key_prefix", "pcv-backup/");
    /* region을 한 곳에서 resolve — envp의 AWS_DEFAULT_REGION과 멀티파트 --region이
     * 동일 값을 쓰도록 보장 (전역 environ 역참조 제거) */
    const gchar *region = pcv_config_get_string("backup", "s3_region", "ap-northeast-2");

    if (!bucket || !*bucket) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "S3 bucket not configured (daemon.conf [backup] s3_bucket)");
        _vm_backup_unlock(vm_name);
        return FALSE;
    }

    /* SSRF guard (A10/V4, Wave B Item 5-a) — S3 --endpoint-url이 설정된 경우
     * argv(_s3_upload_file) 구성 전에 endpoint host를 실주소로 resolve하여
     * 루프백/링크로컬(클라우드 메타데이터) 차단. endpoint 미설정 시 aws-cli 기본
     * 공용 엔드포인트(s3.amazonaws.com)를 쓰므로 검증 생략. */
    if (endpoint && *endpoint) {
        GError *ssrf_err = NULL;
        if (!pcv_url_target_allowed(endpoint, &ssrf_err)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "S3 endpoint blocked (SSRF guard): %s",
                        ssrf_err ? ssrf_err->message : "blocked");
            g_clear_error(&ssrf_err);
            _vm_backup_unlock(vm_name);
            return FALSE;
        }
    }

    /* ── 2. 타임스탬프 생성 ────────────────────── */
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    gchar ts[32];
    g_snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d",
               tm_now->tm_year + 1900, tm_now->tm_mon + 1,
               tm_now->tm_mday, tm_now->tm_hour,
               tm_now->tm_min, tm_now->tm_sec);

    /* B8-W7: S3 임시 파일 디스크 사용률 점검 */
    _check_backup_disk_usage(S3_TEMP_DIR);

    /* ── 3. ZFS 스냅샷 생성 ──────────────────── */
    const gchar *pool = pcv_config_get_zvol_pool();
    gchar *snap_name = g_strdup_printf("%s%s", S3_SNAP_PREFIX, ts);
    gchar *snap_full = g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);

    const gchar *snap_argv[] = {"zfs", "snapshot", snap_full, NULL};
    gchar *stderr_buf = nullptr;
    GError *local_err = nullptr;

    if (!pcv_spawn_sync(snap_argv, NULL, &stderr_buf, &local_err)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS snapshot failed: %s — %s",
                    snap_full,
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(snap_name);
        g_free(snap_full);
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: snapshot created %s", snap_full);

    /* AF-S4: pcv-s3- 스냅샷 리텐션 — 새 스냅샷 생성 성공 직후 prune (best-effort).
     * S3 export 는 full `zfs send`(증분 -i 아님)이라 오래된 pcv-s3- 스냅샷은 base 가
     * 아니므로 send 이전에 정리해도 안전하다. 또한 이후 send/upload 실패 경로에서는
     * pcv-s3- 스냅샷이 destroy 되지 않고 남으므로, 생성 직후 prune 해야 실패 반복 시에도
     * 무한 누적을 막는다. 최신 N개(방금 만든 것 포함) 보존, 초과분만 오래된 순 삭제. */
    _prune_snapshots_by_prefix(vm_name, S3_SNAP_PREFIX,
                               pcv_config_get_int("backup", "s3_retention_count", 7));

    /* ── 4. zfs send | gzip → 임시 파일 ─────── */
    gchar *tmp_file = g_strdup_printf("%s/pcv-s3-%s-%s.zfs.gz",
                                       S3_TEMP_DIR, vm_name, ts);

    /* 셸 파이프 사용 — zfs send | gzip > file */
    gchar *q_snap = g_shell_quote(snap_full);
    gchar *q_tmp  = g_shell_quote(tmp_file);
    gchar *send_cmd = g_strdup_printf("zfs send %s | gzip -1 > %s", q_snap, q_tmp);
    g_free(q_snap);
    g_free(q_tmp);

    const gchar *sh_argv[] = {"/bin/sh", "-c", send_cmd, NULL};
    stderr_buf = nullptr;
    local_err = nullptr;

    gboolean ok = pcv_spawn_sync(sh_argv, NULL, &stderr_buf, &local_err);
    g_free(send_cmd);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ZFS send+gzip failed: %s",
                    local_err ? local_err->message
                              : (stderr_buf ? stderr_buf : "unknown"));
        g_free(stderr_buf);
        if (local_err) g_error_free(local_err);
        /* cleanup */
        g_unlink(tmp_file);
        g_free(tmp_file);
        g_free(snap_name);
        g_free(snap_full);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }
    g_free(stderr_buf);
    if (local_err) { g_error_free(local_err); local_err = nullptr; }

    /* Get file size */
    struct stat st;
    gint64 file_size = 0;
    if (stat(tmp_file, &st) == 0) {
        file_size = (gint64)st.st_size;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: stream created %s (%" G_GINT64_FORMAT " bytes)",
                 tmp_file, file_size);

    /* ── 5. S3 자격증명 envp 빌드 (전역 environ 오염 없음) ── */
    /* M-9: g_setenv로 데몬 전역 environ에 주입하던 방식을 제거. envp를 빌드해
     * pcv_spawn_sync_env()로 각 aws-cli 자식에만 전달한다. */
    gchar **s3_env = _s3_build_env(region);

    /* ── 6. S3 업로드 (aws s3 cp — 자동 멀티파트) ── */
    gchar *s3_data_key = g_strdup_printf("%s%s/%s/backup.zfs.gz", prefix, vm_name, ts);
    ok = _s3_upload_file(endpoint, bucket, s3_data_key, tmp_file,
                          "application/gzip", region,
                          (const gchar * const *)s3_env, error);
    g_free(s3_data_key);

    if (!ok) {
        g_strfreev(s3_env);
        g_unlink(tmp_file);
        g_free(tmp_file);
        g_free(snap_name);
        g_free(snap_full);
        _vm_backup_unlock(vm_name);
        return FALSE;
    }

    PCV_LOG_INFO(BACKUP_LOG_DOM, "S3 backup: data uploaded for %s", vm_name);

    /* ── 7. 메타데이터 JSON 생성 + 업로드 ────── */
    gchar *meta_file = g_strdup_printf("%s/pcv-s3-%s-%s-meta.json",
                                        S3_TEMP_DIR, vm_name, ts);
    {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "vm_name");
        json_builder_add_string_value(b, vm_name);
        json_builder_set_member_name(b, "snapshot");
        json_builder_add_string_value(b, snap_name);
        json_builder_set_member_name(b, "timestamp");
        json_builder_add_string_value(b, ts);
        json_builder_set_member_name(b, "size_bytes");
        json_builder_add_int_value(b, file_size);
        json_builder_set_member_name(b, "compression");
        json_builder_add_string_value(b, "gzip");
        json_builder_set_member_name(b, "pool");
        json_builder_add_string_value(b, pool);
        json_builder_end_object(b);

        JsonGenerator *gen = json_generator_new();
        json_generator_set_pretty(gen, TRUE);
        JsonNode *root = json_builder_get_root(b);
        json_generator_set_root(gen, root);

        GError *write_err = nullptr;
        json_generator_to_file(gen, meta_file, &write_err);
        if (write_err) g_error_free(write_err);

        json_node_free(root);
        g_object_unref(gen);
        g_object_unref(b);
    }

    gchar *s3_meta_key = g_strdup_printf("%s%s/%s/metadata.json", prefix, vm_name, ts);
    GError *meta_err = nullptr;
    _s3_upload_file(endpoint, bucket, s3_meta_key, meta_file,
                     "application/json", region,
                     (const gchar * const *)s3_env, &meta_err);
    if (meta_err) {
        PCV_LOG_WARN(BACKUP_LOG_DOM, "S3 metadata upload warning: %s", meta_err->message);
        g_error_free(meta_err);
    }
    g_free(s3_meta_key);
    g_strfreev(s3_env);

    /* ── 8. 클린업 ───────────────────────────── */
    g_unlink(tmp_file);
    g_unlink(meta_file);

    PCV_LOG_INFO(BACKUP_LOG_DOM,
                 "S3 backup complete: vm=%s snap=%s bucket=%s size=%" G_GINT64_FORMAT,
                 vm_name, snap_name, bucket, file_size);

    g_free(tmp_file);
    g_free(meta_file);
    g_free(snap_name);
    g_free(snap_full);
    _vm_backup_unlock(vm_name);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * [14] 스냅샷 스케줄 상태 조회 (Feature: Snapshot Auto-Scheduling)
 *
 * daemon.conf [snapshot] 섹션 설정과 현재 등록된 정책 기반으로
 * 각 VM별 스냅샷 스케줄 상태를 반환합니다.
 * ══════════════════════════════════════════════════════════════ */

JsonObject *
pcv_snapshot_schedule_status(void)
{
    JsonObject *result = json_object_new();

    /* daemon.conf [snapshot] 섹션 읽기 (글로벌 기본값) */
    gboolean snap_enabled = g_strcmp0(
        pcv_config_get_string("snapshot", "enabled", "true"), "true") == 0;
    gint default_interval = pcv_config_get_int("snapshot", "interval_hours", 24);
    gint default_retention = pcv_config_get_int("snapshot", "retention_count", 7);
    const gchar *default_prefix = pcv_config_get_string("snapshot", "name_prefix", "pcv-auto-");

    json_object_set_boolean_member(result, "enabled", snap_enabled);
    json_object_set_int_member(result, "default_interval_hours", default_interval);
    json_object_set_int_member(result, "default_retention_count", default_retention);
    json_object_set_string_member(result, "name_prefix", default_prefix);
    json_object_set_int_member(result, "check_interval_sec", CHECK_INTERVAL);

    /* 현재 정책 목록 + VM별 스냅샷 상태 */
    JsonArray *arr = json_array_new();
    g_mutex_lock(&g_policy_mutex);
    guint policy_count = g_policies ? g_policies->len : 0;

    for (guint i = 0; i < policy_count; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(g_policies, i);

        JsonObject *po = json_object_new();
        json_object_set_string_member(po, "vm_name", p->vm_name);
        json_object_set_boolean_member(po, "enabled", p->enabled);
        json_object_set_int_member(po, "interval_hours", p->interval_hours);
        json_object_set_int_member(po, "retention_count", p->retention_count);

        /* 와일드카드가 아닌 개별 VM의 경우, 스냅샷 수/마지막/다음 예정 조회 */
        if (g_strcmp0(p->vm_name, "*") != 0) {
            GPtrArray *snaps = _list_auto_snapshots(p->vm_name);
            json_object_set_int_member(po, "snapshot_count", (gint64)snaps->len);

            if (snaps->len > 0) {
                const gchar *newest = g_ptr_array_index(snaps, snaps->len - 1);
                json_object_set_string_member(po, "last_snapshot", newest);

                time_t last_t = _parse_snap_time(newest);
                if (last_t > 0) {
                    time_t next_due = last_t + (time_t)p->interval_hours * 3600;
                    json_object_set_int_member(po, "next_due_epoch", (gint64)next_due);
                }
            } else {
                json_object_set_null_member(po, "last_snapshot");
                json_object_set_int_member(po, "next_due_epoch", 0);
            }

            g_ptr_array_unref(snaps);
        } else {
            json_object_set_string_member(po, "scope", "all_vms");
        }

        json_array_add_object_element(arr, po);
    }

    g_mutex_unlock(&g_policy_mutex);

    json_object_set_int_member(result, "policy_count", (gint64)policy_count);
    json_object_set_array_member(result, "policies", arr);

    return result;
}
