/**
 * @file hids_file_integrity.h
 * @brief HIDS 파일 무결성 — baseline 상태·refresh·drift 스캔 계약
 *
 * 호스트의 중요 파일 집합에 대한 SHA-256 baseline 을 관리하고, 현재 상태와의
 * 차이(drift)를 탐지한다. v1 은 탐지·보고만 한다: 스캔은 변경을 리포트할 뿐
 * baseline 을 절대 변형하지 않는다.
 *
 * [불변식 — ADR-0024]
 *   - baseline 은 admin 이 pcv_hids_baseline_refresh 로 명시 refresh 하기 전까지
 *     PCV_HIDS_BASELINE_UNKNOWN 이다. DB 부재·손상·판독 불가도 UNKNOWN 으로 남긴다
 *     (없는 baseline 을 조용히 trusted 로 신뢰하지 않는 fail-secure 기본값).
 *   - refresh 는 admin 행위이며 감사 로그를 남긴다. scan 은 부작용이 없다.
 *
 * [아키텍처 위치]
 *   security_guard 데몬/handler_security → 이 API → SQLite file_baseline 테이블.
 *   스캔 결과(JSON)는 상위에서 PcvSecurityEvent(file_changed) 로 승격될 수 있다.
 */
#ifndef PURECVISOR_HIDS_FILE_INTEGRITY_H
#define PURECVISOR_HIDS_FILE_INTEGRITY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Unknown is the safe default: no baseline exists until an admin refreshes one. */
typedef enum {
    PCV_HIDS_BASELINE_UNKNOWN,
    PCV_HIDS_BASELINE_TRUSTED,
    PCV_HIDS_BASELINE_STALE
} PcvHidsBaselineStatus;

PcvHidsBaselineStatus pcv_hids_baseline_status(const gchar *db_path);
gboolean pcv_hids_baseline_refresh(const gchar *db_path,
                                    const gchar * const *paths,
                                    gsize n_paths,
                                    const gchar *admin_user,
                                    GError **error);
GPtrArray *pcv_hids_file_integrity_scan(const gchar *db_path,
                                         const gchar * const *paths,
                                         gsize n_paths);

G_END_DECLS

#endif
