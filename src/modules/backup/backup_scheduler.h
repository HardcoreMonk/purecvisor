/**
 * @file backup_scheduler.h
 * @brief ZFS 스냅샷 자동 백업 스케줄러 공개 헤더 — 정책 기반 주기적 스냅샷 생성/삭제
 *
 * [파일 역할]
 *   VM별 ZFS 자동 스냅샷의 정책 관리(CRUD)와 복원(rollback) API를 선언합니다.
 *   GLib 타이머(5분 간격)가 정책을 순회하며 스냅샷 생성/삭제를 자동 수행합니다.
 *
 * [아키텍처 위치]
 *   main.c -> pcv_backup_scheduler_init() / shutdown()
 *   handler_backup.c (backup.* RPC) -> 이 헤더의 정책 CRUD / 이력 / 복원 API
 *   내부 타이머 -> _backup_check_cb() -> zfs snapshot/destroy (자동 실행)
 *
 * [대응 RPC 메서드 (handler_backup.c에서 호출)]
 *   backup.policy.set    -> pcv_backup_policy_set()    (정책 추가/갱신, upsert)
 *   backup.policy.list   -> pcv_backup_policy_list()   (전체 정책 조회)
 *   backup.policy.delete -> pcv_backup_policy_delete() (정책 삭제, 스냅샷 유지)
 *   backup.history       -> pcv_backup_history()       (VM의 pcv-auto-* 스냅샷 이력)
 *   backup.restore       -> pcv_backup_restore()       (zfs rollback, fire-and-forget)
 *
 * [주요 자료구조]
 *   PcvBackupPolicy — 백업 정책 (vm_name, interval_hours, retention_count, enabled)
 *     vm_name="*"이면 전체 VM 대상 와일드카드 정책
 *
 * [사용법 (main.c)]
 *   pcv_backup_scheduler_init();      // 다른 모듈 초기화 이후 1회 호출
 *   // ... GMainLoop 실행 (타이머가 5분마다 자동 백업 수행) ...
 *   pcv_backup_scheduler_shutdown();  // 종료 시 타이머 해제 + 메모리 정리
 *
 * [메모리 관리]
 *   - PcvBackupPolicy: pcv_backup_policy_free()로 해제
 *   - pcv_backup_policy_list() 반환: g_ptr_array_unref()로 해제
 *   - pcv_backup_history() 반환: g_ptr_array_unref()로 해제
 *
 * [정책 파일]
 *   경로: /etc/purecvisor/backup_policies.json
 *   스냅샷 이름: pcv-auto-YYYYMMDD-HHMMSS
 *   검사 주기: 5분 (CHECK_INTERVAL=300초)
 */

#ifndef PCV_BACKUP_SCHEDULER_H
#define PCV_BACKUP_SCHEDULER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvBackupPolicy:
 * @vm_name: VM 이름. "*" 이면 전체 VM 대상.
 * @interval_hours: 스냅샷 생성 주기 (시간 단위).
 * @retention_count: 보존할 스냅샷 최대 개수. 초과 시 가장 오래된 것 삭제.
 * @enabled: 정책 활성 여부.
 */
typedef struct {
    gchar    *vm_name;
    gint      interval_hours;
    gint      retention_count;
    gboolean  enabled;
} PcvBackupPolicy;

/**
 * pcv_backup_scheduler_init:
 * 정책 파일 로드 + 5분 간격 타이머 등록.
 * main.c 에서 다른 모듈 초기화 이후 1회 호출.
 */
void pcv_backup_scheduler_init(void);

/**
 * pcv_backup_scheduler_shutdown:
 * 타이머 해제 + 메모리 정리.
 */
void pcv_backup_scheduler_shutdown(void);

/* ── 정책 CRUD ─────────────────────────────────────────── */

/**
 * pcv_backup_policy_set:
 * @vm_name:         VM 이름 ("*"=전체 VM 대상)
 * @interval_hours:  스냅샷 생성 주기 (시간, >= 1)
 * @retention_count: 보존할 최대 스냅샷 수 (>= 1, 초과 시 오래된 것 삭제)
 * @error:           GError 반환
 *
 * 동일 vm_name 정책이 있으면 갱신, 없으면 추가합니다 (upsert).
 * Returns: 성공 시 TRUE
 */
gboolean   pcv_backup_policy_set(const gchar *vm_name,
                                  gint         interval_hours,
                                  gint         retention_count,
                                  GError     **error);

/**
 * pcv_backup_policy_delete:
 * @vm_name: 삭제할 정책의 VM 이름
 * @error:   GError 반환
 *
 * 정책만 제거합니다 (기존 스냅샷은 유지).
 * Returns: 성공 시 TRUE, 정책 미존재 시 FALSE
 */
gboolean   pcv_backup_policy_delete(const gchar *vm_name,
                                     GError     **error);

/**
 * pcv_backup_policy_list:
 * Returns: (element-type PcvBackupPolicy) (transfer full):
 *   GPtrArray of PcvBackupPolicy*. 호출자가 g_ptr_array_unref() 해야 함.
 */
GPtrArray *pcv_backup_policy_list(void);

/* ── 이력/복원 ─────────────────────────────────────────── */

/**
 * pcv_backup_history:
 * @vm_name: 조회할 VM 이름.
 * Returns: (element-type gchar*) (transfer full):
 *   pcv-auto-* 스냅샷 이름 목록. 호출자가 g_ptr_array_unref() 해야 함.
 */
GPtrArray *pcv_backup_history(const gchar *vm_name);

/**
 * pcv_backup_history_paged:
 * @vm_name:   조회할 VM 이름
 * @offset:    건너뛸 항목 수 (0-based)
 * @limit:     반환할 최대 항목 수 (0이면 전체)
 * @total_out: (out) (nullable): 전체 스냅샷 개수
 *
 * 히스토리 페이지네이션 지원. offset/limit으로 잘라낸 결과를 반환합니다.
 * Returns: (element-type gchar*) (transfer full): g_ptr_array_unref() 해야 함.
 */
GPtrArray *pcv_backup_history_paged(const gchar *vm_name,
                                     guint        offset,
                                     guint        limit,
                                     guint       *total_out);

/**
 * pcv_backup_restore:
 * @vm_name: 복원 대상 VM.
 * @snapshot_name: 복원할 스냅샷 이름.
 * Returns: TRUE on success, FALSE on error.
 */
[[nodiscard]] gboolean   pcv_backup_restore(const gchar *vm_name,
                               const gchar *snapshot_name,
                               GError     **error);

/**
 * pcv_backup_policy_free:
 * PcvBackupPolicy 구조체 해제.
 */
void pcv_backup_policy_free(PcvBackupPolicy *p);

/* ── 증분 백업 ─────────────────────────────────────────── */

/**
 * pcv_backup_incremental:
 * @vm_name: 대상 VM 이름.
 * @error:   GError 반환.
 *
 * 최신 스냅샷 대비 증분 스냅샷을 생성하고 증분 스트림을 파일로 저장합니다.
 * 이전 스냅샷이 없으면 풀 스냅샷을 생성합니다.
 * 저장 경로: /var/lib/purecvisor/backups/<vm>_incr_<timestamp>.zfs
 *
 * Returns: (transfer full): 결과 JSON 객체 (snapshot, base_snapshot, file, size_bytes)
 *   호출자가 json_object_unref()로 해제해야 합니다. 실패 시 NULL.
 */
JsonObject *pcv_backup_incremental(const gchar *vm_name, GError **error);

/* ── 백업 검증 ─────────────────────────────────────────── */

/**
 * pcv_backup_verify:
 * @vm_name:       대상 VM 이름.
 * @snapshot_name: 검증할 스냅샷 이름.
 * @error:         GError 반환.
 *
 * ZFS 스냅샷의 존재 여부와 데이터 무결성을 검증합니다.
 * `zfs send -n` (dry-run)으로 무결성을 확인합니다.
 *
 * Returns: (transfer full): 결과 JSON 객체 (verified, snapshot, size_bytes, integrity)
 *   호출자가 json_object_unref()로 해제해야 합니다. 실패 시 NULL.
 */
JsonObject *pcv_backup_verify(const gchar *vm_name,
                              const gchar *snapshot_name,
                              GError     **error);

/* ── 크로스 노드 복제 ─────────────────────────────────── */

/**
 * pcv_backup_replicate:
 * @vm_name:     대상 VM 이름.
 * @target_node: 대상 노드 IP/호스트명.
 * @ssh_user:    SSH 사용자명 (NULL이면 daemon.conf 기본값 사용).
 * @error:       GError 반환.
 *
 * 최신 스냅샷을 원격 노드로 ZFS send/recv로 복제합니다.
 * 대상에 기존 스냅샷이 있으면 증분, 없으면 풀 전송합니다.
 * [cluster] repl_bandwidth_mbps 설정을 존중합니다.
 *
 * Returns: TRUE on success
 */
gboolean pcv_backup_replicate(const gchar *vm_name,
                              const gchar *target_node,
                              const gchar *ssh_user,
                              GError     **error);

/* ── S3 외부 백업 ──────────────────────────────────────── */

/**
 * pcv_backup_export_s3:
 * @vm_name:      대상 VM 이름
 * @s3_endpoint:  S3 엔드포인트 URL (예: "https://s3.amazonaws.com"), NULL이면 daemon.conf에서 읽음
 * @s3_bucket:    S3 버킷 이름, NULL이면 daemon.conf에서 읽음
 * @s3_key_prefix: S3 오브젝트 키 접두사 (예: "backups/"), NULL이면 "pcv-backup/"
 * @error:        GError 반환
 *
 * ZFS 스냅샷을 생성한 뒤 gzip 압축하여 S3 호환 스토리지에 멀티파트 업로드합니다.
 * 업로드 완료 후 메타데이터 JSON도 함께 저장합니다.
 *
 * 흐름:
 *   1. zfs snapshot pcvpool/vms/<vm>@pcv-s3-<timestamp>
 *   2. zfs send <snap> | gzip > /tmp/pcv-s3-<vm>-<ts>.zfs.gz
 *   3. S3 multipart upload (100MB 청크)
 *   4. 메타데이터 JSON 업로드 ({snapshot, size, timestamp, sha256})
 *   5. 로컬 임시 파일 삭제
 *
 * daemon.conf [backup] 섹션에서 S3 자격증명을 읽습니다:
 *   s3_endpoint, s3_bucket, s3_region, s3_access_key, s3_secret_key
 *
 * Returns: TRUE 성공, FALSE 실패
 */
gboolean pcv_backup_export_s3(const gchar *vm_name,
                               const gchar *s3_endpoint,
                               const gchar *s3_bucket,
                               const gchar *s3_key_prefix,
                               GError     **error);

/* ── 스냅샷 스케줄 상태 조회 ────────────────────────────── */

/**
 * pcv_snapshot_schedule_status:
 *
 * daemon.conf [snapshot] 섹션 + 현재 정책 기반 스냅샷 스케줄 상태를 반환합니다.
 * 각 활성 정책별로 다음 스냅샷 예정 시각과 현재 스냅샷 수를 포함합니다.
 *
 * Returns: (transfer full): JsonObject {enabled, interval_hours, retention_count,
 *   name_prefix, policy_count, policies: [{vm_name, enabled, interval_hours,
 *   retention_count, snapshot_count, last_snapshot, next_due}]}
 *   호출자가 json_object_unref()로 해제해야 합니다.
 */
JsonObject *pcv_snapshot_schedule_status(void);

G_END_DECLS

#endif /* PCV_BACKUP_SCHEDULER_H */
