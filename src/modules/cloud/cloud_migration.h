/**
 * @file cloud_migration.h
 * @brief AWS EC2 ↔ PureCVisor 양방향 VM 마이그레이션 공개 API
 *
 * [아키텍처 위치]
 *   dispatcher.c → vm.import.ec2 / vm.export.ec2 RPC
 *     → pcv_cloud_import_ec2() / pcv_cloud_export_ec2()  [이 헤더]
 *         → GTask 워커 스레드에서 AWS CLI + qemu-img 실행
 *   vm.import.status / vm.export.status → pcv_cloud_get_status()
 *
 * [설계 원칙]
 *   - fire-and-forget: 응답 즉시 전송 후 GTask 비동기 실행
 *   - 상태 추적: GHashTable(인메모리) + GMutex (vm.delete.status 패턴)
 *   - AWS CLI 래핑: pcv_spawn_sync argv 배열 (command injection 방지)
 *   - 설정: daemon.conf [aws] 섹션
 *
 * [Import 파이프라인 — 6단계]
 *   1. AWS 인증 확인 (aws sts get-caller-identity)
 *   2. AMI → EBS Snapshot → S3 Export (aws ec2 export-image)
 *   3. Export 완료 대기 (describe-export-image-tasks 폴링)
 *   4. S3 다운로드 (aws s3 cp)
 *   5. 포맷 변환 (qemu-img convert RAW → qcow2)
 *   6. VM 정의 + 시작 (vm_config_builder → virDomainDefineXML)
 *
 * [Export 파이프라인 — 5단계]
 *   1. VM 중지 (선택)
 *   2. 포맷 변환 (qemu-img convert qcow2 → RAW)
 *   3. S3 업로드 (aws s3 cp --expected-size)
 *   4. AMI 등록 (aws ec2 import-image)
 *   5. Import 완료 대기 (describe-import-image-tasks 폴링)
 */

#ifndef PURECVISOR_CLOUD_MIGRATION_H
#define PURECVISOR_CLOUD_MIGRATION_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ══════════════════════════════════════════════════════════════
 * 상수
 * ══════════════════════════════════════════════════════════════ */
#define PCV_CLOUD_IMPORT_DIR   "/pcvpool/import"
#define PCV_CLOUD_EXPORT_DIR   "/pcvpool/export"

/* ══════════════════════════════════════════════════════════════
 * 작업 상태 열거형
 * ══════════════════════════════════════════════════════════════ */
typedef enum {
    PCV_CLOUD_STATUS_QUEUED      = 0,
    PCV_CLOUD_STATUS_VALIDATING  = 1,
    PCV_CLOUD_STATUS_EXPORTING   = 2,   /* AWS export-image 진행 중 */
    PCV_CLOUD_STATUS_DOWNLOADING = 3,   /* S3 → 로컬 다운로드 */
    PCV_CLOUD_STATUS_UPLOADING   = 4,   /* 로컬 → S3 업로드 */
    PCV_CLOUD_STATUS_CONVERTING  = 5,   /* qemu-img convert */
    PCV_CLOUD_STATUS_IMPORTING   = 6,   /* AWS import-image 진행 중 */
    PCV_CLOUD_STATUS_DEFINING    = 7,   /* virDomainDefineXML */
    PCV_CLOUD_STATUS_DONE        = 8,
    PCV_CLOUD_STATUS_FAILED      = 9,
    PCV_CLOUD_STATUS_PRE_SYNCING     = 10,  /* Phase 1: 기본 이미지 다운로드 중 */
    PCV_CLOUD_STATUS_AWAITING_CUTOVER = 11, /* Phase 1 완료, finalize 대기 */
    PCV_CLOUD_STATUS_FINALIZING      = 12,  /* Phase 2: 델타 전송 + 적용 중 */
} PcvCloudStatus;

const gchar *pcv_cloud_status_str(PcvCloudStatus s);

/* ══════════════════════════════════════════════════════════════
 * 작업 진행 상태 (인메모리 추적)
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar          *name;           /**< VM 이름 */
    gchar          *job_id;         /**< 작업 ID (import-YYYYMMDD-NNN) */
    gchar          *direction;      /**< "import" | "export" */
    PcvCloudStatus  status;
    gint            progress;       /**< 0~100 */
    gchar          *detail;         /**< 상세 메시지 */
    gint64          started_at;     /**< 시작 시각 (Unix timestamp) */
    gint64          updated_at;     /**< 마지막 갱신 시각 */
    gchar          *base_image_path; /**< Phase 1 결과 qcow2 경로 */
    gchar          *instance_id;     /**< EC2 인스턴스 ID */
    gchar          *volume_id;       /**< EBS 볼륨 ID */
} PcvCloudJobStatus;

void pcv_cloud_job_status_free(PcvCloudJobStatus *s);

/* ══════════════════════════════════════════════════════════════
 * Import 파라미터
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar   *name;              /**< PureCVisor VM 이름 */
    gchar   *ami_id;            /**< AWS AMI ID (ami-xxx) */
    gchar   *aws_region;        /**< AWS 리전 (NULL = daemon.conf 기본값) */
    gchar   *s3_bucket;         /**< S3 버킷 (NULL = daemon.conf 기본값) */
    gint     vcpu;              /**< vCPU (0 = 기본 2) */
    gint     memory_mb;         /**< 메모리 MB (0 = 기본 2048) */
    gchar   *network_bridge;    /**< 브릿지 (NULL = pcvbr0) */
    gchar   *disk_format;       /**< "qcow2" (기본) | "zvol" */
    gchar   *mode;              /**< "standard" (기본) | "near-live" */
    gboolean finalize;          /**< Phase 2 finalize 플래그 */
    gchar   *instance_id;       /**< EC2 인스턴스 ID (near-live Phase 2) */
    gchar   *volume_id;         /**< EBS 볼륨 ID (near-live Phase 2) */
} PcvCloudImportParams;

void pcv_cloud_import_params_free(PcvCloudImportParams *p);

/* ══════════════════════════════════════════════════════════════
 * Export 파라미터
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    gchar   *name;              /**< PureCVisor VM 이름 */
    gchar   *aws_region;        /**< AWS 리전 */
    gchar   *s3_bucket;         /**< S3 버킷 */
    gchar   *ami_name;          /**< AMI 이름 (NULL = vm-name-exported) */
    gchar   *ami_description;   /**< AMI 설명 */
    gboolean stop_vm;           /**< 내보내기 전 VM 중지 여부 */
} PcvCloudExportParams;

void pcv_cloud_export_params_free(PcvCloudExportParams *p);

/* ══════════════════════════════════════════════════════════════
 * 공개 API
 * ══════════════════════════════════════════════════════════════ */

/**
 * @brief EC2 → PureCVisor Import (fire-and-forget)
 * GTask 워커 스레드에서 비동기 실행.
 * 호출 즉시 "accepted" 상태의 job_id를 반환.
 * @return job_id 문자열 (호출자 g_free), 실패 시 NULL + error 설정
 */
gchar *pcv_cloud_import_ec2(const PcvCloudImportParams *params, GError **error);

/**
 * @brief PureCVisor → EC2 Export (fire-and-forget)
 * @return job_id 문자열 (호출자 g_free), 실패 시 NULL + error 설정
 */
gchar *pcv_cloud_export_ec2(const PcvCloudExportParams *params, GError **error);

/**
 * @brief 작업 진행 상태 조회
 * @param name VM 이름
 * @return PcvCloudJobStatus* (호출자 pcv_cloud_job_status_free), 없으면 NULL
 */
PcvCloudJobStatus *pcv_cloud_get_status(const gchar *name);

/**
 * @brief 전체 작업 목록 조회
 * @return GPtrArray<PcvCloudJobStatus*> (호출자 g_ptr_array_unref)
 */
GPtrArray *pcv_cloud_list_jobs(void);

/**
 * @brief 진행 중인 작업 취소
 * @param name VM 이름
 * @return TRUE 성공, FALSE 실패 (error에 상세)
 */
gboolean pcv_cloud_cancel_job(const gchar *name, GError **error);

/**
 * @brief Near-Live Import Phase 2 — 최종 전환 (finalize)
 * Phase 1 완료 상태(AWAITING_CUTOVER)의 작업에 대해 EC2 중지 → 델타 전송 → VM 시작
 * @param name VM 이름
 * @return job_id (호출자 g_free), 실패 시 NULL
 */
gchar *pcv_cloud_finalize_import(const gchar *name, GError **error);

G_END_DECLS

#endif /* PURECVISOR_CLOUD_MIGRATION_H */
