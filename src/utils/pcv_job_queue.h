/**
 * @file pcv_job_queue.h
 * @brief 통합 작업 큐 — fire-and-forget 비동기 작업의 상태 추적
 *
 * [파일 역할]
 *   vm.export.ova, backup.replicate, cloud.import 등 장시간 비동기 작업의
 *   상태(대기/실행/완료/실패/취소)를 SQLite 기반으로 추적합니다.
 *   각 작업에 고유 job_id(job-XXXXXXXX)를 부여하고, 진행률/상세를 갱신합니다.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_job_queue_init() / shutdown()
 *   handler_*.c    -> pcv_job_create() / update_status() / set_result()
 *   dispatcher.c   -> jobs.list / jobs.get / jobs.cancel RPC
 *
 * [DB 스키마]
 *   경로: /var/lib/purecvisor/pcv_jobs.db (WAL 모드)
 *   테이블: jobs
 *     job_id TEXT PK, type TEXT, target TEXT, status INTEGER,
 *     progress INTEGER, detail TEXT, params TEXT, result TEXT,
 *     created_at INTEGER, updated_at INTEGER
 *
 * [스레드 안전]
 *   SQLite WAL 모드 + GMutex로 동시 접근 보호.
 *   어떤 스레드에서든 호출 가능합니다.
 */

#ifndef PCV_JOB_QUEUE_H
#define PCV_JOB_QUEUE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvJobStatus:
 * 작업 상태 열거형.
 */
typedef enum {
    PCV_JOB_PENDING   = 0,  /**< 대기 중 (생성 직후) */
    PCV_JOB_RUNNING   = 1,  /**< 실행 중 */
    PCV_JOB_COMPLETED = 2,  /**< 정상 완료 */
    PCV_JOB_FAILED    = 3,  /**< 실패 */
    PCV_JOB_CANCELLED = 4   /**< 취소됨 */
} PcvJobStatus;

/* ── 생명주기 ─────────────────────────────────────────────────── */

/**
 * pcv_job_queue_init:
 * 작업 큐 모듈 초기화: SQLite DB 열기 + WAL 모드 + 테이블 생성.
 * main.c에서 pcv_audit_init() 이후 호출.
 */
void pcv_job_queue_init(void);

/**
 * pcv_job_queue_shutdown:
 * SQLite DB 닫기 + GMutex 해제.
 */
void pcv_job_queue_shutdown(void);

/**
 * pcv_job_queue_cleanup_old:
 * 지정 시간(시) 이상 된 완료/실패/취소 작업을 삭제합니다.
 * 주기적으로 호출하여 DB 크기를 제한합니다.
 *
 * @param max_age_hours 보존 기간 (시간 단위, 0이면 전체 삭제)
 */
void pcv_job_queue_cleanup_old(gint max_age_hours);

/* ── 작업 생성/갱신 (핸들러에서 호출) ────────────────────────── */

/**
 * pcv_job_create:
 * 새 작업을 생성하고 job_id를 반환합니다.
 *
 * @param type        작업 유형 (예: "ova_export", "cloud_import")
 * @param target      대상 리소스 (예: VM 이름)
 * @param params_json 파라미터 JSON 문자열 (NULL 가능)
 * @return job_id 문자열 (호출자가 g_free() 필요)
 */
gchar *pcv_job_create(const gchar *type, const gchar *target,
                       const gchar *params_json);

/**
 * pcv_job_update_status:
 * 작업의 상태/진행률/상세를 갱신합니다.
 *
 * @param job_id       작업 ID
 * @param status       새 상태
 * @param progress_pct 진행률 (0-100)
 * @param detail       상태 상세 문자열 (NULL 가능)
 */
void pcv_job_update_status(const gchar *job_id, PcvJobStatus status,
                            gint progress_pct, const gchar *detail);

/**
 * pcv_job_set_result:
 * 작업 완료/실패 시 최종 결과를 설정합니다.
 *
 * @param job_id      작업 ID
 * @param status      최종 상태 (COMPLETED/FAILED)
 * @param result_json 결과 JSON 문자열 (NULL 가능)
 */
void pcv_job_set_result(const gchar *job_id, PcvJobStatus status,
                         const gchar *result_json);

/* ── 조회 (RPC 핸들러에서 호출) ──────────────────────────────── */

/**
 * pcv_job_list:
 * 최근 작업 목록을 반환합니다 (최신순).
 *
 * @param limit 최대 결과 수 (0이면 기본값 50)
 * @return JsonArray* (호출자가 json_array_unref() 필요)
 */
JsonArray *pcv_job_list(gint limit);

/**
 * pcv_job_get:
 * 단일 작업의 상세 정보를 반환합니다.
 *
 * @param job_id 작업 ID
 * @return JsonObject* (호출자가 json_object_unref() 필요, 미발견 시 NULL)
 */
JsonObject *pcv_job_get(const gchar *job_id);

/**
 * pcv_job_cancel:
 * 대기/실행 중인 작업을 취소 상태로 변경합니다.
 *
 * @param job_id 작업 ID
 * @return TRUE면 취소 성공, FALSE면 이미 완료/취소
 */
gboolean pcv_job_cancel(const gchar *job_id);

G_END_DECLS

#endif /* PCV_JOB_QUEUE_H */
