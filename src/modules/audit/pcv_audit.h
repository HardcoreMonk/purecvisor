/* ==========================================================================
 * src/modules/audit/pcv_audit.h
 * PureCVisor — 감사 로그 모듈 공개 API
 *
 * [파일 역할]
 *   모든 RPC/REST 요청의 행위자(username), 대상(target), 결과(result)를
 *   SQLite WAL + 파일 이중 경로로 기록하는 감사 시스템의 공개 인터페이스.
 *   GAsyncQueue 기반 비동기 워커로 GMainLoop 블록을 방지합니다.
 *
 * [아키텍처 위치]
 *   main.c         -> pcv_audit_init() / shutdown()
 *   dispatcher.c   -> pcv_audit_log_rpc() (RPC 처리 완료 후)
 *   rest_server.c  -> pcv_audit_log()     (REST 요청 처리 후)
 *   ai_agent.c     -> pcv_audit_log()     (AI 합의 완료 시)
 *   self_healing.c -> pcv_audit_log()     (자가 치유 액션 실행 시)
 *
 * [동작 흐름]
 *   pcv_audit_log() → PcvAuditRecord 깊은 복사 → GAsyncQueue push (논블로킹)
 *   전용 워커 GThread → pop → SQLite INSERT + PCV_LOG_AUDIT 파일 기록
 *
 * [DB 스키마]
 *   경로: /var/lib/purecvisor/pcv_audit.db (WAL 모드)
 *   테이블: audit_log
 *     id INTEGER PK, ts TEXT, node TEXT, username TEXT, method TEXT,
 *     target TEXT, result TEXT, error_code INTEGER, duration_ms INTEGER, src_ip TEXT
 *   인덱스: idx_audit_ts(ts), idx_audit_method(method)
 *
 * [보존 정책]
 *   30일 초과 레코드 자동 삭제 (1시간 주기 정리)
 *   DB 크기 상한: ~1GB (PRAGMA max_page_count)
 *
 * [안전장치]
 *   큐 크기 제한: 10,000개 초과 시 레코드 드롭 + 경고 로그
 *   SQLite 열기 실패 시 파일 로그만으로 동작 (graceful degradation)
 *   종료 시 큐에 남은 레코드 모두 처리 후 종료 (graceful drain)
 *
 * [스레드 안전]
 *   GAsyncQueue: lock-free push/pop (호출자 스레드와 워커 스레드 분리)
 *   SQLite: WAL 모드 (동시 읽기 허용, 워커만 쓰기)
 *
 * [메모리 관리]
 *   PcvAuditRecord의 모든 gchar* 멤버는 g_strdup으로 깊은 복사됨.
 *   워커 스레드에서 처리 완료 후 _audit_record_free()로 해제.
 * ========================================================================== */

#ifndef PURECVISOR_AUDIT_H
#define PURECVISOR_AUDIT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvAuditRecord:
 * 감사 로그 단일 레코드 — GAsyncQueue를 통해 워커 스레드에 전달됩니다.
 * 모든 gchar* 멤버는 동적 할당이며 _audit_record_free()로 해제됩니다.
 *
 * @username:    사용자 이름 (JWT에서 추출, 미인증이면 "-")
 * @method:      RPC 메서드 또는 액션 이름 (예: "vm.create")
 * @target:      대상 리소스 (VM 이름, 네트워크 이름 등)
 * @result:      처리 결과 ("ok" | "fail" | "denied")
 * @error_code:  에러 코드 (성공 시 0, JSON-RPC 에러코드)
 * @duration_ms: 처리 시간 (밀리초)
 * @src_ip:      요청 소스 IP (REST=클라이언트 IP, UDS="local")
 */
typedef struct {
    gchar  *username;
    gchar  *method;
    gchar  *target;
    gchar  *result;
    gint    error_code;
    gint64  duration_ms;
    gchar  *src_ip;
} PcvAuditRecord;

/* ── 생명주기 ─────────────────────────────────────────────────── */

/**
 * pcv_audit_init:
 * @db_path: SQLite DB 경로 (예: "/var/lib/purecvisor/pcv_audit.db")
 *
 * 감사 로그 모듈 초기화: DB 열기 + WAL 모드 + 테이블 생성
 * + GAsyncQueue + 워커 스레드 시작.
 * main.c에서 다른 모듈 초기화 이후 호출.
 */
void pcv_audit_init(const gchar *db_path);

/**
 * pcv_audit_shutdown:
 * 워커 중지 + 큐 드레인(남은 레코드 처리) + SQLite 닫기.
 */
void pcv_audit_shutdown(void);

/* ── 감사 기록 (비동기, 논블로킹) ────────────────────────────── */

/**
 * pcv_audit_log:
 * @username:    사용자 이름 (NULL이면 "-")
 * @method:      메서드/액션 이름 (NULL이면 "?")
 * @target:      대상 리소스 (NULL이면 "")
 * @result:      결과 (NULL이면 "ok")
 * @error_code:  에러 코드 (성공=0)
 * @duration_ms: 처리 시간 (밀리초)
 * @src_ip:      소스 IP (NULL이면 "")
 *
 * 감사 레코드를 비동기 큐에 push. 큐 10,000 초과 시 드롭.
 */
void pcv_audit_log(const gchar *username, const gchar *method,
                    const gchar *target, const gchar *result,
                    gint error_code, gint64 duration_ms,
                    const gchar *src_ip);

/**
 * pcv_audit_log_rpc:
 * RPC 핸들러에서 간편 호출용 래퍼.
 * username=NULL, target=NULL, src_ip=NULL로 pcv_audit_log 호출.
 */
void pcv_audit_log_rpc(const gchar *method, const gchar *result,
                        gint error_code, gint64 duration_ms);

/**
 * pcv_audit_get_total_count:
 * 데몬 시작 이후 기록된 총 감사 레코드 수 반환.
 * Prometheus 메트릭이나 /health 엔드포인트에서 사용.
 */
gint64 pcv_audit_get_total_count(void);

/**
 * pcv_audit_get_queue_depth:
 * 현재 비동기 큐에 대기 중인 감사 레코드 수를 반환합니다.
 * Prometheus 메트릭(purecvisor_audit_queue_depth)으로 노출하여
 * 큐 백프레셔 모니터링에 사용합니다.
 *
 * @return 큐 깊이 (모듈 미초기화 시 0)
 */
gint pcv_audit_get_queue_depth(void);

/**
 * pcv_audit_get_dropped_count:
 * 큐 오버플로로 드롭된 총 감사 레코드 수를 반환합니다.
 * Prometheus 메트릭(purecvisor_audit_dropped_total)으로 노출 가능.
 */
gint64 pcv_audit_get_dropped_count(void);

/**
 * pcv_audit_search:
 * @from_ts:        검색 시작 시각 (ISO 형식, NULL이면 제한 없음)
 * @to_ts:          검색 종료 시각 (ISO 형식, NULL이면 제한 없음)
 * @username:       사용자 필터 (정확 매칭, NULL이면 전체)
 * @method_pattern: 메서드 패턴 (LIKE 패턴, NULL이면 전체)
 * @limit:          최대 결과 수 (0 이하이면 기본값 100)
 *
 * audit_log 테이블에서 조건에 맞는 감사 레코드를 검색합니다.
 * Returns: (transfer full): JsonArray* — 호출자가 json_array_unref() 필요
 */
JsonArray *pcv_audit_search(const gchar *from_ts, const gchar *to_ts,
                             const gchar *username, const gchar *method_pattern,
                             gint limit);

/**
 * pcv_audit_recent_failures:
 * @target_filter: 대상 리소스 이름(VM명 등) 정확 매칭, NULL이면 전체
 * @limit:         최대 결과 수 (0 이하이면 5)
 *
 * 최근 result='fail' 감사 레코드를 시간 역순으로 조회한다.
 * UI의 /health/recent-errors 엔드포인트가 호출하여 사용자에게 즉시
 * 워커 실패 사유를 노출하기 위해 사용된다 (ADR-0018).
 *
 * Returns: (transfer full): JsonArray* — 호출자가 json_array_unref() 필요
 */
JsonArray *pcv_audit_recent_failures(const gchar *target_filter, gint limit);

G_END_DECLS

#endif /* PURECVISOR_AUDIT_H */
