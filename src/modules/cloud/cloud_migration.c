/**
 * @file cloud_migration.c
 * @brief AWS EC2 ↔ PureCVisor VM 마이그레이션 — 핵심 오케스트레이터
 *
 * [파일 역할]
 *   AWS EC2 VM을 PureCVisor 클러스터로 가져오거나(Import),
 *   PureCVisor VM을 AWS EC2 AMI로 내보내는(Export) 양방향 마이그레이션을 수행한다.
 *   모든 작업은 fire-and-forget 패턴으로 비동기 실행되며, RPC 호출자에게는
 *   job_id만 즉시 반환한다.
 *
 * [아키텍처 위치]
 *   handler_cloud.c (cloud.import / cloud.export RPC)
 *     → pcv_cloud_import_ec2() / pcv_cloud_export_ec2()   [이 파일, 공개 API]
 *       → GTask 워커 (_import_worker / _export_worker)    [백그라운드 스레드]
 *
 *   handler_cloud.c (cloud.status / cloud.jobs.list RPC)
 *     → pcv_cloud_get_status() / pcv_cloud_list_jobs()    [이 파일, 상태 조회]
 *
 *   handler_cloud.c (cloud.job.cancel RPC)
 *     → pcv_cloud_cancel_job()                            [이 파일, GCancellable 취소]
 *
 *   handler_cloud.c (cloud.import.finalize RPC)
 *     → pcv_cloud_finalize_import()                       [이 파일, Near-Live Phase 2]
 *
 * [Import 파이프라인 — 6단계]
 *   1. AWS 인증 확인 (pcv_aws_check_credentials)
 *   2. AMI → S3 Export 요청 (pcv_aws_export_image)
 *   3. Export 완료 폴링 (최대 1시간, 30초 간격)
 *   4. S3 다운로드 (pcv_aws_s3_download)
 *   5. RAW → qcow2 변환 + virtio 드라이버 주입
 *   6. VM 정의 (virt-install 우선, 실패 시 XML 폴백)
 *
 * [Export 파이프라인 — 5단계]
 *   1. AWS 인증 확인
 *   2. VM 디스크 탐색 + qcow2 → RAW 변환
 *   3. RAW 이미지 S3 업로드
 *   4. AMI 등록 (aws ec2 import-image)
 *   5. AMI 등록 완료 폴링 (최대 1시간)
 *
 * [Near-Live Import (C-4)]
 *   Phase 1: 기본 Import 6단계 실행 후 AWAITING_CUTOVER 상태에서 대기
 *   Phase 2: pcv_cloud_finalize_import() 호출 시 7단계 Finalize 워커 실행
 *     1. EC2 인스턴스 중지 → 2. EBS 델타 스냅샷 생성 → 3. 완료 대기
 *     4. 스냅샷 S3 Export + 다운로드 → 5. 델타 병합 → 6. VM 시작
 *   다운타임: Phase 1 = 0분, Phase 2 = 2~5분
 *
 * [상태 추적]
 *   g_jobs (GHashTable): VM 이름 → PcvCloudJobStatus 매핑 (인메모리)
 *   g_cancellables (GHashTable): VM 이름 → GCancellable 매핑 (취소 시그널)
 *   GMutex(g_jobs_mu)로 동시 접근 보호
 *
 * [안전장치]
 *   - AMI ID 형식 검증 (ami-[a-f0-9]{8,17})
 *   - VM 이름 검증 (pcv_validate_vm_name)
 *   - GCancellable로 사용자 취소 지원
 *   - 감사 로그 기록 (작업 시작/완료/실패)
 *   - 각 단계 실패 시 상세 에러 메시지와 함께 FAILED 상태로 전이
 *
 * [스레드 안전]
 *   g_jobs_mu (GMutex): g_jobs/g_cancellables 해시 테이블 동시 접근 보호
 *   GTask 워커: 각 Import/Export/Finalize 작업은 독립 GTask 스레드에서 실행
 *   _update_status(): 내부에서 g_jobs_mu 잠금 → 상태 갱신 → 해제
 */

#include "cloud_migration.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_validate.h"
#include "../audit/pcv_audit.h"
#include "../virt/vm_config_builder.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gconfig/libvirt-gconfig.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

/*
 * ============================================================================
 *  [주니어 개발자 필독] 클라우드 마이그레이션 핵심 개념 정리
 * ============================================================================
 *
 *  1. 13-상태 머신 (PcvCloudStatus)
 *     queued → validating → exporting → downloading → converting
 *     → defining → done (일반 Import 경로)
 *     queued → validating → exporting → downloading → uploading
 *     → importing → done (Export 경로)
 *     pre_syncing → awaiting_cutover → finalizing → done (Near-Live)
 *     어디서든 → failed (에러 발생 시)
 *     상태 전이는 _update_status()가 관리하며, 각 단계 실패 시
 *     상세 에러 메시지와 함께 FAILED로 전이됩니다.
 *
 *  2. Import 6단계 파이프라인 (_import_worker)
 *     1단계: AWS 인증 확인 (aws sts get-caller-identity)
 *     2단계: AMI → S3 Export (aws ec2 export-image → RAW 이미지)
 *     3단계: Export 완료 폴링 (30초 간격 × 최대 120회 = 1시간)
 *     4단계: S3 다운로드 (aws s3 cp)
 *     5단계: RAW → qcow2 변환 + virtio 드라이버 주입
 *     6단계: VM 정의 (virt-install 우선, 실패 시 XML 폴백)
 *
 *  3. Export 5단계 파이프라인 (_export_worker)
 *     1단계: AWS 인증 확인
 *     2단계: VM 디스크 탐색 + qcow2 → RAW 변환
 *     3단계: RAW 이미지 S3 업로드
 *     4단계: AMI 등록 (aws ec2 import-image)
 *     5단계: AMI 등록 완료 폴링
 *
 *  4. Near-Live 2-Phase 동작 (C-4)
 *     기존 Import는 EC2 인스턴스를 중지해야 하므로 다운타임이 수십 분 발생.
 *     Near-Live는 다운타임을 2~5분으로 줄이는 2단계 방식:
 *       Phase 1 (사전 동기화, 다운타임 0):
 *         - Import 6단계를 실행하되, EC2는 계속 실행 중
 *         - AWAITING_CUTOVER 상태에서 finalize 호출 대기
 *       Phase 2 (최종 전환, 다운타임 2~5분):
 *         1. EC2 인스턴스 중지 → 2. EBS 델타 스냅샷 생성
 *         3. 스냅샷 완료 대기 → 4. S3 Export + 다운로드
 *         5. 델타 병합 (qemu-img) → 6. VM 시작
 *
 *  5. SQLite job 영속화 (_cloud_db_*)
 *     인메모리 해시 테이블과 함께 /var/lib/purecvisor/cloud_jobs.db에
 *     작업 상태를 기록합니다. 데몬 재시작 시 DB에서 진행 중이었던
 *     작업을 FAILED로 마킹하여 일관성을 유지합니다.
 *
 *  6. AWAITING_CUTOVER 타임아웃
 *     Near-Live Phase 1 완료 후 관리자가 finalize를 호출하지 않으면
 *     CUTOVER_TIMEOUT_SEC(2시간) 후 자동으로 FAILED 처리됩니다.
 *     무기한 대기하면 EC2 인스턴스와 로컬 디스크 이미지가 불일치 상태로
 *     방치되므로, 타임아웃으로 강제 종료합니다.
 * ============================================================================
 */

#define CLOUD_LOG "cloud_migration"

/* ── AWAITING_CUTOVER 타임아웃 — 2시간 후 자동 취소 ──────── */
constexpr int CUTOVER_TIMEOUT_SEC = 7200;
/* ── Proactive sweeper 주기 (5분) — get_status가 호출되지 않아도 만료 처리 ──── */
constexpr int CUTOVER_SWEEP_INTERVAL_SEC = 300;

/* B2-CRIT2: forward decl (정의는 list_jobs 직전) — import/export 진입점에서 호출 */
static void _ensure_cutover_sweeper(void);

/* ══════════════════════════════════════════════════════════════
 * 외부 함수 선언 (aws_client.c, disk_converter.c)
 *
 * 헤더 순환 의존을 방지하기 위해 extern 인라인 선언 사용.
 * aws_client.c: AWS CLI 래핑 (인증, S3, EC2 Export/Import)
 * disk_converter.c: qemu-img 기반 디스크 포맷 변환
 * ══════════════════════════════════════════════════════════════ */
extern gboolean pcv_aws_check_credentials(const gchar *region, GError **error);
extern gchar   *pcv_aws_export_image(const gchar *ami_id, const gchar *s3_bucket,
                                       const gchar *region, GError **error);
extern gint     pcv_aws_check_export_progress(const gchar *task_id, const gchar *region,
                                                gchar **out_status, gchar **out_s3_key,
                                                GError **error);
extern gboolean pcv_aws_s3_download(const gchar *s3_bucket, const gchar *s3_key,
                                      const gchar *local_dir, const gchar *region,
                                      GError **error);
extern gboolean pcv_aws_s3_upload(const gchar *local_path, const gchar *s3_bucket,
                                    const gchar *s3_prefix, const gchar *region,
                                    GError **error);
extern gchar   *pcv_aws_import_image(const gchar *s3_bucket, const gchar *s3_key,
                                       const gchar *region, const gchar *description,
                                       GError **error);
extern gint     pcv_aws_check_import_progress(const gchar *task_id, const gchar *region,
                                                gchar **out_status, gchar **out_ami_id,
                                                GError **error);
extern gchar   *pcv_disk_convert_raw_to_qcow2(const gchar *raw_path, const gchar *vm_name,
                                                 const gchar *output_dir, GError **error);
extern gchar   *pcv_disk_convert_qcow2_to_raw(const gchar *qcow2_path, const gchar *vm_name,
                                                 const gchar *output_dir, GError **error);
extern gchar   *pcv_disk_find_vm_disk(const gchar *vm_name, GError **error);
extern gboolean pcv_disk_inject_virtio(const gchar *disk_path, GError **error);
extern gboolean pcv_aws_stop_instance(const gchar *instance_id, const gchar *region, GError **error);
extern gchar   *pcv_aws_create_snapshot(const gchar *volume_id, const gchar *description,
                                          const gchar *region, GError **error);
extern gboolean pcv_aws_wait_snapshot(const gchar *snapshot_id, const gchar *region, GError **error);
extern gboolean pcv_disk_apply_delta(const gchar *base_qcow2, const gchar *delta_raw,
                                       GError **error);

/* ══════════════════════════════════════════════════════════════
 * 상태 문자열 변환
 *
 * PcvCloudStatus 열거형 → 사람이 읽을 수 있는 문자열 매핑.
 * REST/RPC 응답 JSON의 "status" 필드에 사용된다.
 * 인덱스 순서가 PcvCloudStatus enum 정의와 반드시 일치해야 한다.
 * ══════════════════════════════════════════════════════════════ */
static const gchar *_status_strs[] = {
    "queued",             /* 0: 작업 대기열에 추가됨 */
    "validating",         /* 1: AWS 인증 확인 중 */
    "exporting",          /* 2: AMI → S3 Export 진행 중 */
    "downloading",        /* 3: S3에서 이미지 다운로드 중 */
    "uploading",          /* 4: S3로 이미지 업로드 중 */
    "converting",         /* 5: RAW ↔ qcow2 디스크 변환 중 */
    "importing",          /* 6: AWS에 AMI 등록 중 */
    "defining",           /* 7: libvirt VM 정의 중 */
    "done",               /* 8: 작업 완료 */
    "failed",             /* 9: 작업 실패 */
    "pre_syncing",        /* 10: Near-Live Phase 1 사전 동기화 */
    "awaiting_cutover",   /* 11: Near-Live Phase 1 완료 — finalize 대기 */
    "finalizing"          /* 12: Near-Live Phase 2 최종 전환 진행 중 */
};

const gchar *
pcv_cloud_status_str(PcvCloudStatus s)
{
    if (s >= 0 && s <= PCV_CLOUD_STATUS_FINALIZING)
        return _status_strs[s];
    return "unknown";
}

/* ══════════════════════════════════════════════════════════════
 * 인메모리 상태 추적 (GHashTable + GMutex)
 *
 * 마이그레이션 작업 상태를 메모리에서 관리한다.
 * 데몬 재시작 시 상태가 초기화되므로, 영속화가 필요하면
 * SQLite 또는 etcd로 확장해야 한다.
 * ══════════════════════════════════════════════════════════════ */
static GHashTable *g_jobs = nullptr;         /* key=vm_name(gchar*), value=PcvCloudJobStatus* */
static GHashTable *g_cancellables = nullptr; /* key=vm_name(gchar*), value=GCancellable* — 취소 시그널 전송용 */
static GMutex      g_jobs_mu;             /* g_jobs, g_cancellables 동시 접근 보호 */
static gint        g_job_seq = 0;         /* 작업 ID 시퀀스 (원자적 증가, import-0001 형식) */

/* ══════════════════════════════════════════════════════════════
 * SQLite 영속화 — 작업 상태를 디스크에 기록하여 데몬 재시작 시 복구
 *
 * 인메모리 해시 테이블과 함께 SQLite에도 작업 상태를 기록한다.
 * 데몬 재시작 시 DB에서 진행 중이었던 작업을 FAILED로 마킹한다.
 * ══════════════════════════════════════════════════════════════ */
static sqlite3 *g_cloud_db = nullptr;
#define CLOUD_DB_PATH "/var/lib/purecvisor/cloud_jobs.db"

/**
 * _cloud_db_init — SQLite DB를 열고 cloud_jobs 테이블을 생성한다.
 * 데몬 시작 시 1회 호출된다 (lazy initialization).
 */
static void
_cloud_db_init(void)
{
    if (g_cloud_db) return;

    g_mkdir_with_parents("/var/lib/purecvisor", 0700);
    int rc = sqlite3_open(CLOUD_DB_PATH, &g_cloud_db);
    if (rc != SQLITE_OK) {
        PCV_LOG_WARN(CLOUD_LOG, "Failed to open cloud jobs DB: %s",
                     g_cloud_db ? sqlite3_errmsg(g_cloud_db) : "unknown");
        g_cloud_db = nullptr;
        return;
    }
    sqlite3_exec(g_cloud_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db, "PRAGMA busy_timeout=3000;", NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db,
        "CREATE TABLE IF NOT EXISTS cloud_jobs ("
        "  id TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  vm_name TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  progress INTEGER DEFAULT 0,"
        "  error TEXT,"
        "  created_at INTEGER,"
        "  updated_at INTEGER);",
        NULL, NULL, NULL);

    /* B2-MINOR2: 인덱스 — vm_name/status는 외부 분석/복구 경로에서 사용된다.
     * 인메모리 해시테이블이 주 경로지만 풀스캔 회피용. */
    sqlite3_exec(g_cloud_db,
        "CREATE INDEX IF NOT EXISTS idx_cloud_jobs_vm_name ON cloud_jobs(vm_name);",
        NULL, NULL, NULL);
    sqlite3_exec(g_cloud_db,
        "CREATE INDEX IF NOT EXISTS idx_cloud_jobs_status ON cloud_jobs(status);",
        NULL, NULL, NULL);

    /* 재시작 시: 완료되지 않은 작업을 FAILED로 마킹 (인메모리 상태가 소실되었으므로) */
    sqlite3_exec(g_cloud_db,
        "UPDATE cloud_jobs SET status='failed', error='Daemon restarted' "
        "WHERE status NOT IN ('done','failed');",
        NULL, NULL, NULL);

    PCV_LOG_INFO(CLOUD_LOG, "Cloud jobs DB initialized at %s", CLOUD_DB_PATH);
}

/**
 * _cloud_db_save_job — 새 작업을 DB에 기록한다.
 */
static void
_cloud_db_save_job(const gchar *job_id, const gchar *type, const gchar *vm_name)
{
    if (!g_cloud_db) _cloud_db_init();
    if (!g_cloud_db) return;

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_cloud_db,
        "INSERT OR REPLACE INTO cloud_jobs (id, type, vm_name, status, progress, created_at, updated_at) "
        "VALUES (?, ?, ?, 'pending', 0, ?, ?);",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
        sqlite3_bind_text(stmt, 1, job_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, type, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, vm_name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_int64(stmt, 5, now);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_warning("[cloud] insert job %s failed: %s", job_id, sqlite3_errmsg(g_cloud_db));
        }
        sqlite3_finalize(stmt);
    }
}

/**
 * _cloud_db_update_status — 작업 상태를 DB에 갱신한다.
 */
static void
_cloud_db_update_status(const gchar *job_id, const gchar *status_str,
                        gint progress, const gchar *error_detail)
{
    if (!g_cloud_db) return;

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_cloud_db,
        "UPDATE cloud_jobs SET status=?, progress=?, error=?, updated_at=? "
        "WHERE id=?;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
        sqlite3_bind_text(stmt, 1, status_str, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, progress);
        sqlite3_bind_text(stmt, 3, error_detail, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_text(stmt, 5, job_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_warning("[cloud] update job %s failed: %s", job_id, sqlite3_errmsg(g_cloud_db));
        }
        sqlite3_finalize(stmt);
    }
}

/* ══════════════════════════════════════════════════════════════
 * AMI ID 형식 검증
 *
 * AWS AMI ID는 "ami-" 접두사 + 8~17자리 16진수로 구성된다.
 * 예: "ami-0abcdef1234567890"
 * 잘못된 형식의 AMI ID를 AWS API에 전달하면 의미 없는 에러가
 * 반환되므로, 사전 검증으로 개발자 실수를 조기에 차단한다.
 * ══════════════════════════════════════════════════════════════ */
static gboolean
_validate_ami_id(const gchar *ami_id)
{
    if (!ami_id || !g_str_has_prefix(ami_id, "ami-"))
        return FALSE;
    const gchar *hex = ami_id + 4;
    gsize len = strlen(hex);
    if (len < 8 || len > 17)
        return FALSE;
    for (gsize i = 0; i < len; i++) {
        if (!g_ascii_isxdigit(hex[i]))
            return FALSE;
    }
    return TRUE;
}

/**
 * _ensure_jobs — 해시 테이블 지연 초기화 (lazy initialization)
 *
 * 첫 번째 작업 생성/조회 시 호출되어 g_jobs와 g_cancellables를 생성한다.
 * 반드시 g_jobs_mu 잠금 상태에서 호출해야 한다.
 */
static void _ensure_jobs(void) {
    if (!g_jobs)
        g_jobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)pcv_cloud_job_status_free);
    if (!g_cancellables)
        g_cancellables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 g_object_unref);
}

/**
 * _update_status — 마이그레이션 작업 상태를 갱신한다.
 *
 * @param name      VM 이름 (해시 테이블 키)
 * @param job_id    작업 ID (예: "import-0001")
 * @param direction 방향 문자열 ("import" 또는 "export")
 * @param status    새 상태 (PcvCloudStatus enum)
 * @param progress  진행률 (0~100%)
 * @param detail    상세 설명 (사람이 읽을 수 있는 메시지)
 *
 * 내부에서 g_jobs_mu 잠금을 잡고 해시 테이블에 새 PcvCloudJobStatus를 삽입한다.
 * 기존 엔트리가 있으면 started_at과 near-live 메타데이터를 보존한다.
 * 작업 시작/완료/실패 시 감사 로그도 기록한다.
 */
static void
_update_status(const gchar *name, const gchar *job_id,
               const gchar *direction, PcvCloudStatus status,
               gint progress, const gchar *detail)
{
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();

    PcvCloudJobStatus *s = g_new0(PcvCloudJobStatus, 1);
    s->name       = g_strdup(name);
    s->job_id     = g_strdup(job_id);
    s->direction  = g_strdup(direction);
    s->status     = status;
    s->progress   = progress;
    s->detail     = g_strdup(detail);
    s->updated_at = g_get_real_time() / G_USEC_PER_SEC;

    /* started_at + near-live 필드 보존 */
    PcvCloudJobStatus *old = g_hash_table_lookup(g_jobs, name);
    s->started_at = old ? old->started_at : s->updated_at;
    if (old) {
        if (!s->base_image_path) s->base_image_path = g_strdup(old->base_image_path);
        if (!s->instance_id)     s->instance_id     = g_strdup(old->instance_id);
        if (!s->volume_id)       s->volume_id       = g_strdup(old->volume_id);
    }

    g_hash_table_replace(g_jobs, g_strdup(name), s);
    g_mutex_unlock(&g_jobs_mu);

    PCV_LOG_INFO(CLOUD_LOG, "[%s] %s — %s (%d%%) %s",
                 job_id, name, pcv_cloud_status_str(status), progress, detail ?: "");

    /* SQLite 영속화 */
    _cloud_db_update_status(job_id, pcv_cloud_status_str(status), progress, detail);

    /* 감사 로그: 작업 시작/완료/실패 시 기록 */
    /* ADR-0018-audit: cloud.import, cloud.export, cloud.import.finalize
     * (동적 메서드명 — cloud.<direction> 형태로 g_strdup_printf 합성, 정적 분석 annotation) */
    if (status == PCV_CLOUD_STATUS_QUEUED ||
        status == PCV_CLOUD_STATUS_DONE ||
        status == PCV_CLOUD_STATUS_FAILED) {
        gchar *method = g_strdup_printf("cloud.%s", direction ?: "unknown");
        pcv_audit_log("system", method, name,
                      pcv_cloud_status_str(status),
                      status == PCV_CLOUD_STATUS_FAILED ? -1 : 0,
                      (s->updated_at - s->started_at) * 1000, "local");
        g_free(method);
    }
}

/**
 * _update_job_metadata — Near-Live Import용 메타데이터를 기존 작업에 추가한다.
 *
 * Phase 1 완료 후 base_image_path, instance_id, volume_id를 작업 상태에 저장한다.
 * Phase 2(finalize)에서 이 메타데이터를 읽어 델타 동기화에 사용한다.
 */
static void _update_job_metadata(const gchar *name, const gchar *base_path,
                                  const gchar *instance_id, const gchar *volume_id)
{
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (st) {
        if (base_path) { g_free(st->base_image_path); st->base_image_path = g_strdup(base_path); }
        if (instance_id) { g_free(st->instance_id); st->instance_id = g_strdup(instance_id); }
        if (volume_id) { g_free(st->volume_id); st->volume_id = g_strdup(volume_id); }
    }
    g_mutex_unlock(&g_jobs_mu);
}

/* ══════════════════════════════════════════════════════════════
 * 메모리 해제 — 구조체 내부 g_strdup 필드를 모두 g_free 해야 한다.
 * GHashTable의 value_destroy_func로도 사용된다.
 * ══════════════════════════════════════════════════════════════ */
void pcv_cloud_job_status_free(PcvCloudJobStatus *s) {
    if (!s) return;
    g_free(s->name); g_free(s->job_id); g_free(s->direction);
    g_free(s->detail);
    g_free(s->base_image_path); g_free(s->instance_id); g_free(s->volume_id);
    g_free(s);
}

void pcv_cloud_import_params_free(PcvCloudImportParams *p) {
    if (!p) return;
    g_free(p->name); g_free(p->ami_id); g_free(p->aws_region);
    g_free(p->s3_bucket); g_free(p->network_bridge); g_free(p->disk_format);
    g_free(p->mode); g_free(p->instance_id); g_free(p->volume_id);
    g_free(p);
}

void pcv_cloud_export_params_free(PcvCloudExportParams *p) {
    if (!p) return;
    g_free(p->name); g_free(p->aws_region); g_free(p->s3_bucket);
    g_free(p->ami_name); g_free(p->ami_description); g_free(p);
}

/* ══════════════════════════════════════════════════════════════
 * Import 워커 스레드 (GTask)
 *
 * 6단계 파이프라인:
 *   1. AWS 인증 확인
 *   2. AMI → S3 Export 요청 (aws ec2 create-instance-export-task)
 *   3. Export 완료 폴링 (30초 간격, 최대 1시간)
 *   4. S3 다운로드 (aws s3 cp)
 *   5. RAW → qcow2 변환 (qemu-img convert) + virtio 드라이버 주입
 *   6. VM 정의 (virt-install 우선, 실패 시 libvirt XML 폴백)
 *
 * Near-Live 모드: 5단계 완료 후 AWAITING_CUTOVER에서 중단하고
 *   finalize 호출을 기다린다 (다운타임 0분 사전 동기화).
 *
 * 취소: GCancellable이 폴링 루프 내에서 주기적으로 확인되어,
 *   사용자가 cloud.job.cancel RPC로 작업을 중단할 수 있다.
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    PcvCloudImportParams *params;  /**< Import 파라미터 (deep copy, 워커 종료 시 해제) */
    gchar *job_id;                 /**< 작업 ID (예: "import-0001") */
} ImportTaskData;

static void _import_data_free(gpointer p) {
    ImportTaskData *d = p;
    if (!d) return;
    pcv_cloud_import_params_free(d->params);
    g_free(d->job_id);
    g_free(d);
}

static void
_import_worker(GTask *task, gpointer src __attribute__((unused)),
               gpointer task_data, GCancellable *cancel)
{
    ImportTaskData *d = task_data;
    PcvCloudImportParams *p = d->params;
    const gchar *job = d->job_id;
    const gchar *region = p->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = p->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;
    const gboolean near_live = p->mode && g_strcmp0(p->mode, "near-live") == 0;

    /* 1. AWS 인증 확인 */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_VALIDATING, 2,
                   "Checking AWS credentials");
    if (!pcv_aws_check_credentials(region, &err)) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       err ? err->message : "AWS credentials not configured");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 2. AMI → S3 Export 요청 */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING, 5,
                   "Requesting AWS image export");
    gchar *export_task_id = pcv_aws_export_image(p->ami_id, bucket, region, &err);
    if (!export_task_id) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 5,
                       err ? err->message : "export-image failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 3. Export 완료 대기 (폴링, 최대 1시간) */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING, 10,
                   "Waiting for AWS export...");
    gchar *s3_key = nullptr;
    for (int i = 0; i < 120; i++) {  /* 30초 × 120 = 1시간 */
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr;
        gint prog = pcv_aws_check_export_progress(export_task_id, region,
                                                     &status, &s3_key, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            g_free(status);
            break;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, prog,
                           status ?: "export failed");
            g_free(status); g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *detail = g_strdup_printf("AWS export: %s (%d%%)", status ?: "active", prog);
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_EXPORTING,
                       10 + prog / 5, detail);
        g_free(detail); g_free(status);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(export_task_id);

    if (!s3_key || !*s3_key) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 30,
                       "S3 key not found after export");
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 4. S3 다운로드 */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DOWNLOADING, 35,
                   "Downloading from S3");
    g_mkdir_with_parents(PCV_CLOUD_IMPORT_DIR, 0755);
    if (!pcv_aws_s3_download(bucket, s3_key, PCV_CLOUD_IMPORT_DIR, region, &err)) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 35,
                       err ? err->message : "S3 download failed");
        if (err) g_error_free(err);
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_filename = g_path_get_basename(s3_key);
    gchar *raw_path = g_strdup_printf("%s/%s", PCV_CLOUD_IMPORT_DIR, raw_filename);
    g_free(raw_filename); g_free(s3_key);

    /* 5. RAW → qcow2 변환 */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_CONVERTING, 70,
                   "Converting RAW to qcow2");
    gchar *disk_path = pcv_disk_convert_raw_to_qcow2(raw_path, p->name, NULL, &err);
    if (!disk_path) {
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 70,
                       err ? err->message : "Disk conversion failed");
        if (err) g_error_free(err);
        unlink(raw_path); g_free(raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* virtio 드라이버 주입 (선택적) */
    pcv_disk_inject_virtio(disk_path, NULL);

    /* 임시 RAW 파일 삭제 */
    unlink(raw_path);
    g_free(raw_path);

    /* Near-Live: Phase 1 완료 — base image 저장 후 finalize 대기 */
    if (near_live) {
        _update_job_metadata(p->name, disk_path, p->instance_id, p->volume_id);
        _update_status(p->name, job, "import", PCV_CLOUD_STATUS_AWAITING_CUTOVER, 80,
                       "Base image ready. Call finalize to complete import.");
        g_free(disk_path);
        g_task_return_boolean(task, TRUE);
        return;
    }

    /* 6. VM 정의 (libvirt) — virt-install 시도 후 실패 시 XML 폴백 */
    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DEFINING, 90,
                   "Creating VM definition");
    {
        gint vcpu = p->vcpu > 0 ? p->vcpu : 2;
        gint mem = p->memory_mb > 0 ? p->memory_mb : 2048;
        const gchar *bridge = p->network_bridge ?: "pcvbr0";
        gboolean vm_defined = FALSE;

        /* 방법 1: virt-install */
        gchar *mem_str = g_strdup_printf("%d", mem);
        gchar *vcpu_str = g_strdup_printf("%d", vcpu);
        gchar *net_arg = g_strdup_printf("bridge=%s,model=virtio", bridge);
        const gchar *argv[] = {
            "virt-install",
            "--name", p->name,
            "--memory", mem_str,
            "--vcpus", vcpu_str,
            "--disk", disk_path,
            "--network", net_arg,
            "--import",
            "--os-variant", "generic",
            "--graphics", "vnc,listen=0.0.0.0",
            "--noautoconsole",
            NULL
        };
        gchar *out = NULL, *verr = nullptr;
        if (pcv_spawn_sync(argv, &out, &verr, &err)) {
            vm_defined = TRUE;
        } else {
            PCV_LOG_WARN(CLOUD_LOG, "virt-install failed: %s — trying XML fallback",
                         verr ?: (err ? err->message : "unknown"));
            if (err) { g_error_free(err); err = nullptr; }
        }
        g_free(out); g_free(verr);
        g_free(mem_str); g_free(vcpu_str); g_free(net_arg);

        /* 방법 2: vm_config_builder XML 폴백 */
        if (!vm_defined) {
            PureCVisorVmConfig *cfg = purecvisor_vm_config_new(p->name, vcpu, mem);
            purecvisor_vm_config_set_disk(cfg, disk_path);
            purecvisor_vm_config_set_network_bridge(cfg, bridge);
            GVirConfigDomain *dom = purecvisor_vm_config_build(cfg);
            if (dom) {
                gchar *xml = gvir_config_object_to_xml(
                    GVIR_CONFIG_OBJECT(dom));
                /* B2-MINOR1: XML tmpfile은 0600 권한이어야 한다.
                 * Why: g_file_set_contents는 기본 umask (보통 0644) 사용 → 다른 사용자 읽기 가능.
                 *      VM XML에는 디스크 경로/맥주소 등 내부 정보가 포함되므로 격리 필요. */
                gchar *tmpfile = g_strdup_printf("/tmp/pcv-import-%s.xml", p->name);
                if (g_file_set_contents(tmpfile, xml, -1, NULL)) {
                    if (chmod(tmpfile, 0600) != 0) {
                        PCV_LOG_WARN(CLOUD_LOG, "chmod 0600 failed on %s: %s",
                                     tmpfile, g_strerror(errno));
                    }
                    const gchar *def_argv[] = {"virsh", "define", tmpfile, NULL};
                    gchar *dout = NULL, *derr = nullptr;
                    if (pcv_spawn_sync(def_argv, &dout, &derr, &err)) {
                        vm_defined = TRUE;
                        /* virsh start */
                        const gchar *start_argv[] = {"virsh", "start", p->name, NULL};
                        pcv_spawn_sync(start_argv, NULL, NULL, NULL);
                    } else {
                        PCV_LOG_ERROR(CLOUD_LOG, "virsh define fallback failed: %s",
                                      derr ?: (err ? err->message : "unknown"));
                        if (err) { g_error_free(err); err = nullptr; }
                    }
                    g_free(dout); g_free(derr);
                }
                unlink(tmpfile);
                g_free(tmpfile); g_free(xml);
                g_object_unref(dom);
            }
            purecvisor_vm_config_free(cfg);
        }

        if (!vm_defined) {
            /* B2-WARN1: VM define 실패 시 변환된 qcow2 파일이 고아로 남음.
             * 재시도에서 같은 이름을 쓰면 stale 파일과 충돌하므로 정리한다. */
            unlink(disk_path);
            _update_status(p->name, job, "import", PCV_CLOUD_STATUS_FAILED, 90,
                           "VM definition failed (virt-install + XML fallback)");
            g_free(disk_path);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    _update_status(p->name, job, "import", PCV_CLOUD_STATUS_DONE, 100,
                   "Import complete");
    g_free(disk_path);
    g_task_return_boolean(task, TRUE);
}

/* ══════════════════════════════════════════════════════════════
 * Export 워커 스레드 (GTask)
 *
 * PureCVisor VM → AWS AMI 내보내기 5단계 파이프라인:
 *   1. AWS 인증 확인
 *   2. VM 디스크 경로 탐색 + qcow2 → RAW 변환 (qemu-img convert)
 *   3. RAW 이미지 S3 업로드 (aws s3 cp)
 *   4. AMI 등록 요청 (aws ec2 import-image)
 *   5. AMI 등록 완료 폴링 (30초 간격, 최대 1시간)
 *
 * Import와 동일하게 GCancellable로 취소를 지원하며,
 * 임시 RAW 파일은 S3 업로드 완료 후 즉시 삭제한다.
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    PcvCloudExportParams *params;  /**< Export 파라미터 (deep copy) */
    gchar *job_id;                 /**< 작업 ID (예: "export-0001") */
} ExportTaskData;

static void _export_data_free(gpointer p) {
    ExportTaskData *d = p;
    if (!d) return;
    pcv_cloud_export_params_free(d->params);
    g_free(d->job_id);
    g_free(d);
}

static void
_export_worker(GTask *task, gpointer src __attribute__((unused)),
               gpointer task_data, GCancellable *cancel)
{
    ExportTaskData *d = task_data;
    PcvCloudExportParams *p = d->params;
    const gchar *job = d->job_id;
    const gchar *region = p->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = p->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;

    /* 1. AWS 인증 확인 */
    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_VALIDATING, 2,
                   "Checking AWS credentials");
    if (!pcv_aws_check_credentials(region, &err)) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 0,
                       err ? err->message : "AWS credentials not configured");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* [감사 AF-S3] 실행 중 VM의 디스크를 정지/quiesce 없이 변환하면 crash
     * inconsistent(손상) 이미지가 정상 AMI로 업로드된다(과거 stop_vm 플래그는
     * 선언만 되고 읽히지 않았다). VM이 shut off가 아니면 거부한다 — 일관된
     * export는 오프라인 상태를 요구한다. (자동 정지/재시작 배선은 후속.) */
    {
        const gchar *dsargv[] = {"virsh", "domstate", p->name, NULL};
        gchar *dstate = NULL;
        gboolean got = pcv_spawn_sync(dsargv, &dstate, NULL, NULL);
        gboolean running = got && dstate && (strstr(dstate, "shut off") == NULL);
        g_free(dstate);
        if (running) {
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 5,
                "VM must be shut off before export (running VM would produce a corrupt image)");
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    /* 2. VM 디스크 경로 탐색 + qcow2 → RAW 변환 */
    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_CONVERTING, 10,
                   "Finding VM disk and converting to RAW");
    gchar *disk_path = pcv_disk_find_vm_disk(p->name, &err);
    if (!disk_path) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 10,
                       err ? err->message : "VM disk not found");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_path = pcv_disk_convert_qcow2_to_raw(disk_path, p->name, NULL, &err);
    g_free(disk_path);
    if (!raw_path) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 20,
                       err ? err->message : "Disk conversion failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 3. S3 업로드 */
    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_UPLOADING, 30,
                   "Uploading to S3");
    if (!pcv_aws_s3_upload(raw_path, bucket, "pcv-export/", region, &err)) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 30,
                       err ? err->message : "S3 upload failed");
        if (err) g_error_free(err);
        unlink(raw_path); g_free(raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *raw_basename = g_path_get_basename(raw_path);
    gchar *s3_key = g_strdup_printf("pcv-export/%s", raw_basename);
    g_free(raw_basename);

    /* 임시 RAW 삭제 */
    unlink(raw_path); g_free(raw_path);

    /* 4. AMI 등록 (aws ec2 import-image) */
    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_IMPORTING, 60,
                   "Registering AMI in AWS");
    gchar *desc = p->ami_description
        ? g_strdup(p->ami_description)
        : g_strdup_printf("PureCVisor export: %s", p->name);
    gchar *import_task_id = pcv_aws_import_image(bucket, s3_key, region, desc, &err);
    g_free(desc); g_free(s3_key);

    if (!import_task_id) {
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 60,
                       err ? err->message : "import-image failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 5. AMI 등록 완료 대기 (폴링, 최대 1시간) */
    for (int i = 0; i < 120; i++) {
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(import_task_id);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr; gchar *ami_id = nullptr;
        gint prog = pcv_aws_check_import_progress(import_task_id, region,
                                                     &status, &ami_id, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            gchar *detail = g_strdup_printf("AMI created: %s", ami_id ?: "unknown");
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_DONE, 100, detail);
            g_free(detail); g_free(status); g_free(ami_id); g_free(import_task_id);
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, prog,
                           status ?: "import-image failed");
            g_free(status); g_free(ami_id); g_free(import_task_id);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *detail = g_strdup_printf("AMI import: %s (%d%%)", status ?: "active", prog);
        _update_status(p->name, job, "export", PCV_CLOUD_STATUS_IMPORTING,
                       60 + prog * 4 / 10, detail);
        g_free(detail); g_free(status); g_free(ami_id);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(import_task_id);

    _update_status(p->name, job, "export", PCV_CLOUD_STATUS_FAILED, 90,
                   "AMI import timed out (1 hour)");
    g_task_return_boolean(task, FALSE);
}

/* ══════════════════════════════════════════════════════════════
 * 공개 API — RPC 핸들러에서 호출하는 진입점
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_cloud_import_ec2 — AWS EC2 AMI를 PureCVisor VM으로 가져온다.
 *
 * @param params  Import 파라미터 (name, ami_id 필수)
 * @param error   에러 발생 시 설정됨
 *
 * 파라미터를 deep copy한 후 GTask 워커를 생성하여 비동기로 실행한다.
 * fire-and-forget 패턴: job_id만 즉시 반환하고, 실제 작업은 백그라운드에서 진행.
 * 진행 상태는 pcv_cloud_get_status(name)로 폴링하여 확인한다.
 *
 * Returns: (transfer full): job_id 문자열 (g_free 필요), 실패 시 NULL
 */
gchar *
pcv_cloud_import_ec2(const PcvCloudImportParams *params, GError **error)
{
    if (!params || !params->name || !params->ami_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name and ami_id are required");
        return NULL;
    }
    if (!pcv_validate_vm_name(params->name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name: %s", params->name);
        return NULL;
    }
    if (!_validate_ami_id(params->ami_id)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid AMI ID format (expected ami-[a-f0-9]{8,17}): %s",
                    params->ami_id);
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("import-%04d", seq);

    /* SQLite 영속화: 새 작업 기록 */
    _cloud_db_save_job(job_id, "import", params->name);
    _ensure_cutover_sweeper();

    _update_status(params->name, job_id, "import", PCV_CLOUD_STATUS_QUEUED, 0,
                   "Import job queued");

    /* 파라미터 deep copy */
    PcvCloudImportParams *copy = g_new0(PcvCloudImportParams, 1);
    copy->name           = g_strdup(params->name);
    copy->ami_id         = g_strdup(params->ami_id);
    copy->aws_region     = g_strdup(params->aws_region);
    copy->s3_bucket      = g_strdup(params->s3_bucket);
    copy->vcpu           = params->vcpu;
    copy->memory_mb      = params->memory_mb;
    copy->network_bridge = g_strdup(params->network_bridge);
    copy->disk_format    = g_strdup(params->disk_format);
    copy->mode           = g_strdup(params->mode);
    copy->finalize       = params->finalize;
    copy->instance_id    = g_strdup(params->instance_id);
    copy->volume_id      = g_strdup(params->volume_id);

    ImportTaskData *d = g_new0(ImportTaskData, 1);
    d->params = copy;
    d->job_id = g_strdup(job_id);

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    g_hash_table_replace(g_cancellables, g_strdup(params->name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _import_data_free);
    g_task_run_in_thread(task, _import_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}

/**
 * pcv_cloud_export_ec2 — PureCVisor VM을 AWS EC2 AMI로 내보낸다.
 *
 * @param params  Export 파라미터 (name 필수)
 * @param error   에러 발생 시 설정됨
 *
 * Import와 동일한 fire-and-forget 패턴. 디스크 변환 + S3 업로드 + AMI 등록을
 * 백그라운드에서 수행한다.
 *
 * Returns: (transfer full): job_id 문자열 (g_free 필요), 실패 시 NULL
 */
gchar *
pcv_cloud_export_ec2(const PcvCloudExportParams *params, GError **error)
{
    if (!params || !params->name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return NULL;
    }
    if (!pcv_validate_vm_name(params->name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid VM name: %s", params->name);
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("export-%04d", seq);

    /* SQLite 영속화: 새 작업 기록 */
    _cloud_db_save_job(job_id, "export", params->name);
    _ensure_cutover_sweeper();

    _update_status(params->name, job_id, "export", PCV_CLOUD_STATUS_QUEUED, 0,
                   "Export job queued");

    PcvCloudExportParams *copy = g_new0(PcvCloudExportParams, 1);
    copy->name            = g_strdup(params->name);
    copy->aws_region      = g_strdup(params->aws_region);
    copy->s3_bucket       = g_strdup(params->s3_bucket);
    copy->ami_name        = g_strdup(params->ami_name);
    copy->ami_description = g_strdup(params->ami_description);
    copy->stop_vm         = params->stop_vm;

    ExportTaskData *d = g_new0(ExportTaskData, 1);
    d->params = copy;
    d->job_id = g_strdup(job_id);

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    g_hash_table_replace(g_cancellables, g_strdup(params->name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _export_data_free);
    g_task_run_in_thread(task, _export_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}

/**
 * pcv_cloud_get_status — 특정 VM의 마이그레이션 작업 상태를 조회한다.
 *
 * @param name  VM 이름
 *
 * GMutex 잠금 하에 해시 테이블에서 조회하고, deep copy를 반환한다.
 * 반환된 구조체는 호출자가 pcv_cloud_job_status_free()로 해제해야 한다.
 *
 * Returns: (transfer full): 상태 구조체 deep copy, 미존재 시 NULL
 */
PcvCloudJobStatus *
pcv_cloud_get_status(const gchar *name)
{
    if (!name) return NULL;
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    PcvCloudJobStatus *orig = g_hash_table_lookup(g_jobs, name);

    /* AWAITING_CUTOVER 타임아웃 체크: 2시간 후 자동 실패 처리 */
    if (orig && orig->status == PCV_CLOUD_STATUS_AWAITING_CUTOVER) {
        gint64 now_sec = g_get_real_time() / G_USEC_PER_SEC;
        if ((now_sec - orig->updated_at) > CUTOVER_TIMEOUT_SEC) {
            PCV_LOG_WARN(CLOUD_LOG,
                "AWAITING_CUTOVER timeout for '%s' (age=%ld sec, limit=%d sec) — auto-cancelling",
                name, (long)(now_sec - orig->updated_at), CUTOVER_TIMEOUT_SEC);
            orig->status = PCV_CLOUD_STATUS_FAILED;
            g_free(orig->detail);
            orig->detail = g_strdup_printf(
                "AWAITING_CUTOVER timed out after %d seconds — finalize was not called",
                CUTOVER_TIMEOUT_SEC);
            orig->updated_at = now_sec;
            _cloud_db_update_status(orig->job_id, "failed", orig->progress, orig->detail);
        }
    }

    PcvCloudJobStatus *copy = nullptr;
    if (orig) {
        copy = g_new0(PcvCloudJobStatus, 1);
        copy->name       = g_strdup(orig->name);
        copy->job_id     = g_strdup(orig->job_id);
        copy->direction  = g_strdup(orig->direction);
        copy->status     = orig->status;
        copy->progress   = orig->progress;
        copy->detail     = g_strdup(orig->detail);
        copy->started_at = orig->started_at;
        copy->updated_at = orig->updated_at;
        copy->base_image_path = g_strdup(orig->base_image_path);
        copy->instance_id     = g_strdup(orig->instance_id);
        copy->volume_id       = g_strdup(orig->volume_id);
    }
    g_mutex_unlock(&g_jobs_mu);
    return copy;
}

/**
 * pcv_cloud_list_jobs — 모든 마이그레이션 작업 목록을 반환한다.
 *
 * 해시 테이블의 모든 엔트리를 deep copy하여 GPtrArray로 반환한다.
 * 각 항목은 PcvCloudJobStatus 포인터이며, 배열 소멸 시 자동 해제된다.
 *
 * Returns: (transfer full): GPtrArray of PcvCloudJobStatus*
 */
/* B2-CRIT2: AWAITING_CUTOVER 타임아웃을 g_jobs 전체에 적용한다.
 * Why: pcv_cloud_get_status는 단일 VM에만 적용되므로 list/sweeper에서도 수행해야
 *      finalize 미호출 작업이 무기한 잔류하지 않는다. */
static void
_sweep_cutover_timeouts_locked(void)
{
    if (!g_jobs) return;
    gint64 now_sec = g_get_real_time() / G_USEC_PER_SEC;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_jobs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        PcvCloudJobStatus *st = v;
        if (st->status == PCV_CLOUD_STATUS_AWAITING_CUTOVER &&
            (now_sec - st->updated_at) > CUTOVER_TIMEOUT_SEC) {
            PCV_LOG_WARN(CLOUD_LOG,
                "AWAITING_CUTOVER timeout for '%s' (age=%ld sec) — auto-cancelling",
                st->name ?: "?", (long)(now_sec - st->updated_at));
            st->status = PCV_CLOUD_STATUS_FAILED;
            g_free(st->detail);
            st->detail = g_strdup_printf(
                "AWAITING_CUTOVER timed out after %d seconds — finalize was not called",
                CUTOVER_TIMEOUT_SEC);
            st->updated_at = now_sec;
            _cloud_db_update_status(st->job_id, "failed", st->progress, st->detail);
        }
    }
}

/* GMainLoop 타이머 콜백: 5분마다 만료된 AWAITING_CUTOVER 작업을 일괄 처리 */
static gboolean
_cutover_sweeper_tick(gpointer user_data __attribute__((unused)))
{
    g_mutex_lock(&g_jobs_mu);
    _sweep_cutover_timeouts_locked();
    g_mutex_unlock(&g_jobs_mu);
    return G_SOURCE_CONTINUE;
}

static guint g_cutover_sweeper_id = 0;
static void
_ensure_cutover_sweeper(void)
{
    if (g_cutover_sweeper_id != 0) return;
    g_cutover_sweeper_id = g_timeout_add_seconds(
        CUTOVER_SWEEP_INTERVAL_SEC, _cutover_sweeper_tick, NULL);
}

GPtrArray *
pcv_cloud_list_jobs(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_cloud_job_status_free);
    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();
    _sweep_cutover_timeouts_locked();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, g_jobs);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        PcvCloudJobStatus *orig = val;
        PcvCloudJobStatus *copy = g_new0(PcvCloudJobStatus, 1);
        copy->name       = g_strdup(orig->name);
        copy->job_id     = g_strdup(orig->job_id);
        copy->direction  = g_strdup(orig->direction);
        copy->status     = orig->status;
        copy->progress   = orig->progress;
        copy->detail     = g_strdup(orig->detail);
        copy->started_at = orig->started_at;
        copy->updated_at = orig->updated_at;
        copy->base_image_path = g_strdup(orig->base_image_path);
        copy->instance_id     = g_strdup(orig->instance_id);
        copy->volume_id       = g_strdup(orig->volume_id);
        g_ptr_array_add(list, copy);
    }
    g_mutex_unlock(&g_jobs_mu);
    return list;
}

/**
 * pcv_cloud_cancel_job — 진행 중인 마이그레이션 작업을 취소한다.
 *
 * @param name   VM 이름
 * @param error  에러 발생 시 설정됨
 *
 * GCancellable에 취소 시그널을 보내면, Import/Export 워커의 폴링 루프에서
 * g_cancellable_is_cancelled()가 TRUE를 반환하여 작업이 중단된다.
 * 이미 완료/실패된 작업은 취소할 수 없다.
 *
 * Returns: 취소 성공 시 TRUE
 */
gboolean
pcv_cloud_cancel_job(const gchar *name, GError **error)
{
    if (!name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return FALSE;
    }

    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();

    /* 작업 존재 여부 확인 */
    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (!st) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No migration job for VM: %s", name);
        return FALSE;
    }

    /* 이미 완료/실패된 작업은 취소 불가 */
    if (st->status == PCV_CLOUD_STATUS_DONE ||
        st->status == PCV_CLOUD_STATUS_FAILED) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Job already finished (status: %s)",
                    pcv_cloud_status_str(st->status));
        return FALSE;
    }

    /* GCancellable 취소 시그널 전송 */
    GCancellable *cancel = g_hash_table_lookup(g_cancellables, name);
    if (cancel)
        g_cancellable_cancel(cancel);

    g_mutex_unlock(&g_jobs_mu);

    PCV_LOG_INFO(CLOUD_LOG, "Cancel requested for job: %s (VM: %s)",
                 st->job_id ?: "?", name);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * Near-Live Import Phase 2 — Finalize 워커
 *
 * Phase 1(사전 동기화)에서 base image를 이미 다운로드/변환해 두었으므로,
 * 이 단계에서는 EC2를 중지한 후 변경분(delta)만 동기화하여
 * 다운타임을 2~5분으로 최소화한다.
 *
 * 7단계 파이프라인:
 *   1. EC2 인스턴스 중지 (pcv_aws_stop_instance)
 *   2. EBS 볼륨 델타 스냅샷 생성 (pcv_aws_create_snapshot)
 *   3. 스냅샷 완료 대기 (pcv_aws_wait_snapshot)
 *   4. 스냅샷 → S3 Export + 다운로드
 *   5. 델타 RAW → base qcow2에 병합 (pcv_disk_apply_delta)
 *   6. VM 시작 (virsh start)
 *   7. 상태를 DONE으로 갱신
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar *name;
    gchar *job_id;
    gchar *instance_id;
    gchar *volume_id;
    gchar *base_image_path;
    gchar *aws_region;
    gchar *s3_bucket;
} FinalizeTaskData;

static void _finalize_data_free(gpointer p) {
    FinalizeTaskData *d = p;
    if (!d) return;
    g_free(d->name); g_free(d->job_id);
    g_free(d->instance_id); g_free(d->volume_id);
    g_free(d->base_image_path);
    g_free(d->aws_region); g_free(d->s3_bucket);
    g_free(d);
}

static void
_finalize_worker(GTask *task, gpointer src __attribute__((unused)),
                 gpointer task_data, GCancellable *cancel)
{
    FinalizeTaskData *d = task_data;
    const gchar *region = d->aws_region ?: pcv_config_get_string("aws", "default_region", "ap-northeast-2");
    const gchar *bucket = d->s3_bucket ?: pcv_config_get_string("aws", "s3_bucket", "pcv-migration");
    GError *err = nullptr;

    /* 1. EC2 인스턴스 중지 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 5,
                   "Stopping EC2 instance");
    if (!pcv_aws_stop_instance(d->instance_id, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 5,
                       err ? err->message : "Failed to stop EC2 instance");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (g_cancellable_is_cancelled(cancel)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       "Cancelled by user");
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 2. EBS 델타 스냅샷 생성 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 20,
                   "Creating delta snapshot");
    gchar *desc = g_strdup_printf("PureCVisor near-live delta: %s", d->name);
    gchar *snap_id = pcv_aws_create_snapshot(d->volume_id, desc, region, &err);
    g_free(desc);
    if (!snap_id) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 20,
                       err ? err->message : "Failed to create snapshot");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 3. 스냅샷 완료 대기 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 30,
                   "Waiting for snapshot completion");
    if (!pcv_aws_wait_snapshot(snap_id, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 30,
                       err ? err->message : "Snapshot wait failed");
        if (err) g_error_free(err);
        g_free(snap_id);
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (g_cancellable_is_cancelled(cancel)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                       "Cancelled by user");
        g_free(snap_id);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* 4. 스냅샷 → S3 Export + 다운로드 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 40,
                   "Exporting delta snapshot to S3");
    gchar *export_task_id = pcv_aws_export_image(snap_id, bucket, region, &err);
    g_free(snap_id);
    if (!export_task_id) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 40,
                       err ? err->message : "Delta export failed");
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* Export 완료 대기 (최대 30분) */
    gchar *s3_key = nullptr;
    for (int i = 0; i < 60; i++) {
        if (g_cancellable_is_cancelled(cancel)) {
            _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 0,
                           "Cancelled by user");
            g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        gchar *status = nullptr;
        gint prog = pcv_aws_check_export_progress(export_task_id, region,
                                                     &status, &s3_key, NULL);
        if (prog >= 100 || g_strcmp0(status, "completed") == 0) {
            g_free(status);
            break;
        }
        if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "cancelled") == 0) {
            _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 50,
                           status ?: "delta export failed");
            g_free(status); g_free(export_task_id); g_free(s3_key);
            g_task_return_boolean(task, FALSE);
            return;
        }
        g_free(status);
        g_usleep(30 * G_USEC_PER_SEC);
    }
    g_free(export_task_id);

    if (!s3_key || !*s3_key) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 55,
                       "Delta S3 key not found after export");
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* S3 다운로드 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 60,
                   "Downloading delta image");
    g_mkdir_with_parents(PCV_CLOUD_IMPORT_DIR, 0755);
    if (!pcv_aws_s3_download(bucket, s3_key, PCV_CLOUD_IMPORT_DIR, region, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 60,
                       err ? err->message : "Delta S3 download failed");
        if (err) g_error_free(err);
        g_free(s3_key);
        g_task_return_boolean(task, FALSE);
        return;
    }

    gchar *delta_filename = g_path_get_basename(s3_key);
    gchar *delta_raw_path = g_strdup_printf("%s/%s", PCV_CLOUD_IMPORT_DIR, delta_filename);
    g_free(delta_filename); g_free(s3_key);

    /* 5. 델타 병합 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 80,
                   "Applying delta to base image");
    if (!pcv_disk_apply_delta(d->base_image_path, delta_raw_path, &err)) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 80,
                       err ? err->message : "Delta apply failed");
        if (err) g_error_free(err);
        unlink(delta_raw_path); g_free(delta_raw_path);
        g_task_return_boolean(task, FALSE);
        return;
    }
    unlink(delta_raw_path); g_free(delta_raw_path);

    /* 6. VM 시작 */
    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 95,
                   "Starting VM");
    gboolean started = FALSE;
    {
        const gchar *start_argv[] = {"virsh", "start", d->name, NULL};
        gchar *out = NULL, *verr = nullptr;
        started = pcv_spawn_sync(start_argv, &out, &verr, &err);
        if (!started) {
            PCV_LOG_WARN(CLOUD_LOG, "virsh start failed during finalize: %s",
                         verr ?: (err ? err->message : "unknown"));
            if (err) { g_error_free(err); err = nullptr; }
        }
        g_free(out); g_free(verr);
    }

    /* [감사 AF-S2] 이전에는 start 실패 시 warning만 찍고 무조건 DONE(100%)을 보고해,
     * 정의된 적 없는 도메인 + 이미 정지된 원본 EC2로 "무-VM 아웃티지를 성공"으로
     * 표기했다. start가 실패하면 FAILED로 정직하게 보고한다. (near-live가 libvirt
     * 도메인을 define하지 않는 근본 결함의 완전 수정은 후속 — 여기서는 거짓 성공
     * 보고를 차단해 운영자가 개입할 수 있게 한다.) */
    if (!started) {
        _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_FAILED, 95,
            "VM failed to start after finalize (domain may not be defined) — no running VM");
        g_task_return_boolean(task, FALSE);
        return;
    }

    _update_status(d->name, d->job_id, "import", PCV_CLOUD_STATUS_DONE, 100,
                   "Near-live import finalized");
    g_task_return_boolean(task, TRUE);
}

/**
 * pcv_cloud_finalize_import — Near-Live Import의 Phase 2(최종 전환)를 시작한다.
 *
 * @param name   VM 이름 (Phase 1이 AWAITING_CUTOVER 상태여야 함)
 * @param error  에러 발생 시 설정됨
 *
 * Phase 1에서 저장된 메타데이터(base_image_path, instance_id, volume_id)를
 * 읽어 FinalizeTaskData를 구성하고, _finalize_worker GTask를 실행한다.
 * 이 시점부터 EC2 인스턴스가 중지되므로 다운타임이 시작된다.
 *
 * Returns: (transfer full): job_id 문자열 (g_free 필요), 실패 시 NULL
 */
gchar *
pcv_cloud_finalize_import(const gchar *name, GError **error)
{
    if (!name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "name is required");
        return NULL;
    }

    g_mutex_lock(&g_jobs_mu);
    _ensure_jobs();

    PcvCloudJobStatus *st = g_hash_table_lookup(g_jobs, name);
    if (!st) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No migration job for VM: %s", name);
        return NULL;
    }
    if (st->status != PCV_CLOUD_STATUS_AWAITING_CUTOVER) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Job not in AWAITING_CUTOVER state (current: %s)",
                    pcv_cloud_status_str(st->status));
        return NULL;
    }
    if (!st->base_image_path || !st->instance_id || !st->volume_id) {
        g_mutex_unlock(&g_jobs_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Missing near-live metadata (base_image/instance_id/volume_id)");
        return NULL;
    }

    gint seq = g_atomic_int_add(&g_job_seq, 1);
    gchar *job_id = g_strdup_printf("finalize-%04d", seq);

    /* SQLite 영속화: finalize 작업 기록 */
    _cloud_db_save_job(job_id, "finalize", name);

    FinalizeTaskData *d = g_new0(FinalizeTaskData, 1);
    d->name            = g_strdup(name);
    d->job_id          = g_strdup(job_id);
    d->instance_id     = g_strdup(st->instance_id);
    d->volume_id       = g_strdup(st->volume_id);
    d->base_image_path = g_strdup(st->base_image_path);

    /* Region/bucket from config */
    d->aws_region = g_strdup(pcv_config_get_string("aws", "default_region", "ap-northeast-2"));
    d->s3_bucket  = g_strdup(pcv_config_get_string("aws", "s3_bucket", "pcv-migration"));

    g_mutex_unlock(&g_jobs_mu);

    _update_status(name, job_id, "import", PCV_CLOUD_STATUS_FINALIZING, 0,
                   "Finalize started — stopping EC2 instance");

    GCancellable *cancel = g_cancellable_new();
    g_mutex_lock(&g_jobs_mu);
    g_hash_table_replace(g_cancellables, g_strdup(name), g_object_ref(cancel));
    g_mutex_unlock(&g_jobs_mu);

    GTask *task = g_task_new(NULL, cancel, NULL, NULL);
    g_task_set_task_data(task, d, _finalize_data_free);
    g_task_run_in_thread(task, _finalize_worker);
    g_object_unref(task);
    g_object_unref(cancel);

    return job_id;
}
