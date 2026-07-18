/**
 * @file security_event.h
 * @brief Native Host HIDS/HIPS 보안 이벤트 값 모델 — enum·직렬화·coalesce 키 계약
 *
 * Security Guard(SG)가 탐지한 사건 하나를 표현하는 값 타입 PcvSecurityEvent 와,
 * 그 열거형을 프로세스 경계 밖에서 안정적으로 주고받기 위한 문자열 계약을 정의한다.
 *
 * [아키텍처 위치]
 *   HIDS 수집기(hids_file_integrity 등) → 이 모델로 정규화 → security_policy 가
 *   severity/action 판정 → security_store 가 SQLite 에 저장 → handler_security RPC
 *   → Web UI. 이 헤더는 파이프라인 전 구간이 공유하는 공통 어휘다.
 *
 * [불변식]
 *   - 직렬화는 enum 수치값이 아니라 문자열 이름만 사용한다(security_event.c 의 맵).
 *     RPC/DB/UI 는 수치에 의존하면 안 되며, enum 을 재정렬해도 저장 포맷은 안정적이다.
 *   - 모든 문자열 필드는 고정 크기 버퍼다 — RPC 핸들러가 부분 free 없이 struct 를
 *     통째로 소유·복사할 수 있게 하려는 의도적 선택(가변 할당 회피).
 *   - evidence_json 은 pcv_security_event_set_evidence 단일 가드로만 채운다: 버퍼
 *     초과 시 중간 절단된 invalid JSON 대신 유효한 fallback JSON 을 저장한다.
 *   관련: ADR-0024(탐지 우선, 운영자 승인 기반 대응).
 */
#ifndef PURECVISOR_SECURITY_EVENT_H
#define PURECVISOR_SECURITY_EVENT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * Keep enum order stable for C defaults, but serialize only the string names
 * from security_event.c. RPC/DB/UI callers must not depend on numeric values.
 */
typedef enum {
    PCV_SECURITY_SOURCE_FILE_INTEGRITY,
    PCV_SECURITY_SOURCE_RUNTIME,
    PCV_SECURITY_SOURCE_LOG,
    PCV_SECURITY_SOURCE_PCV_AUDIT
} PcvSecuritySource;

typedef enum {
    PCV_SECURITY_EVENT_FILE_CHANGED,
    PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS,
    PCV_SECURITY_EVENT_APPARMOR_DENIED,
    PCV_SECURITY_EVENT_AUTH_BRUTEFORCE,
    PCV_SECURITY_EVENT_AUDIT_PATTERN
} PcvSecurityEventType;

typedef enum {
    PCV_SECURITY_SEVERITY_INFO,
    PCV_SECURITY_SEVERITY_WARN,
    PCV_SECURITY_SEVERITY_CRIT
} PcvSecuritySeverity;

typedef enum {
    PCV_SECURITY_TARGET_FILE,
    PCV_SECURITY_TARGET_PROCESS,
    PCV_SECURITY_TARGET_IP,
    PCV_SECURITY_TARGET_USER,
    PCV_SECURITY_TARGET_API_KEY,
    PCV_SECURITY_TARGET_SERVICE,
    PCV_SECURITY_TARGET_VM,
    PCV_SECURITY_TARGET_HOST
} PcvSecurityTargetKind;

typedef enum {
    PCV_SECURITY_STATUS_OPEN,
    PCV_SECURITY_STATUS_SUPPRESSED,
    PCV_SECURITY_STATUS_ACTION_PENDING,
    PCV_SECURITY_STATUS_RESOLVED
} PcvSecurityStatus;

typedef struct {
    /* Fixed-size fields keep RPC handler ownership simple and avoid partial frees. */
    gchar event_id[64];
    gint64 timestamp;
    PcvSecuritySource source;
    PcvSecurityEventType type;
    PcvSecuritySeverity severity;
    gint confidence;
    PcvSecurityTargetKind target_kind;
    gchar target[256];
    gchar summary[256];
    gchar recommended_action[64];
    PcvSecurityStatus status;
    gchar evidence_json[2048];
} PcvSecurityEvent;

const gchar *pcv_security_source_to_string(PcvSecuritySource v);
const gchar *pcv_security_type_to_string(PcvSecurityEventType v);
const gchar *pcv_security_severity_to_string(PcvSecuritySeverity v);
const gchar *pcv_security_target_kind_to_string(PcvSecurityTargetKind v);
const gchar *pcv_security_status_to_string(PcvSecurityStatus v);

gboolean pcv_security_severity_from_string(const gchar *s, PcvSecuritySeverity *out);
gboolean pcv_security_status_from_string(const gchar *s, PcvSecurityStatus *out);
JsonObject *pcv_security_event_to_json(const PcvSecurityEvent *ev);
gboolean pcv_security_event_from_json(JsonObject *obj, PcvSecurityEvent *out);

/**
 * pcv_security_event_set_evidence — evidence_json 고정 버퍼 안전 기록 (M-10/B-2)
 * 초과 시 값 중간 절단(invalid JSON) 대신 유효 fallback JSON 을 저장한다.
 * 프로듀서(SG)와 역직렬화 site 가 공유하는 단일 가드.
 */
void pcv_security_event_set_evidence(gchar *dst, gsize dstsz, const gchar *ejstr);
void pcv_security_event_make_id(PcvSecurityEvent *ev, const gchar *prefix);
gchar *pcv_security_event_coalesce_key(const PcvSecurityEvent *ev);

G_END_DECLS

#endif
