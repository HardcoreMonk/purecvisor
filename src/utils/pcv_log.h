/**
 * @file pcv_log.h
 * @brief 구조화된 JSON 로깅 시스템 — 공개 헤더 (매크로, API 선언)
 *
 * Sprint C-1에서 도입된 PureCVisor 전용 로깅 시스템의 공개 인터페이스입니다.
 * 모든 .c 파일에서 #include "pcv_log.h"로 포함하여 사용합니다.
 *
 * [설계 원칙]
 *   1. req_id TLS: GTask 워커 스레드에서도 요청 ID 자동 추적 (분산 추적 기반)
 *   2. JSON 구조화: 파싱 가능한 한 줄 JSON (Logstash/Loki/jq 호환)
 *   3. systemd journal 통합: JOURNAL_STREAM 자동 감지 → syslog priority prefix
 *   4. 감사 로그 분리: create/delete/start/stop 등 중요 작업을 별도 파일 기록
 *
 * [매크로 사용법]
 *   PCV_LOG_DEBUG("module_name", "debug message: %s", var);   // G_LOG_LEVEL_DEBUG
 *   PCV_LOG_INFO ("module_name", "info message: %s", var);    // G_LOG_LEVEL_MESSAGE
 *   PCV_LOG_WARN ("module_name", "warning: %s", var);         // G_LOG_LEVEL_WARNING
 *   PCV_LOG_ERROR("module_name", "error: %s", var);           // G_LOG_LEVEL_CRITICAL (abort 아님!)
 *   PCV_LOG_AUDIT("module", "vm.create", "myvm", "created OK"); // 감사 로그 (별도 파일)
 *
 * [출력 형식 예시]
 *   일반:  {"t":"2025-03-07T14:30:00.123Z","lvl":"INFO","dom":"handler_vm","req":"abc-123","msg":"..."}
 *   감사:  {"t":"...","lvl":"AUDIT","dom":"...","req":"...","op":"vm.create","target":"myvm","msg":"..."}
 *
 * [req_id 흐름]
 *   dispatcher.c 요청 수신 → pcv_log_req_id_set(rpc_id)
 *   → 이후 해당 스레드의 모든 PCV_LOG_* 호출에 req 필드 자동 포함
 *   → GTask 워커 스레드에서도 독립적으로 동작 (GPrivate TLS 기반)
 *   → 하나의 RPC 요청이 여러 모듈을 거치는 로그를 req_id로 연결하여 추적
 *
 * [초기화]
 *   main.c에서: pcv_log_init() (가장 먼저) → ... → pcv_log_shutdown() (가장 나중에)
 *
 * [주의사항]
 *   - PCV_LOG_ERROR는 G_LOG_LEVEL_CRITICAL 사용 (abort 아님!)
 *   - G_LOG_LEVEL_ERROR(GLib 내장)는 g_abort() 호출 — PCV_LOG 매크로에서는 미사용
 *   - dom(도메인) 파라미터: 모듈 식별 문자열 (예: "handler_vm", "virt_conn_pool")
 *   - 로그 확인: journalctl -u purecvisorsd -f 또는 journalctl -u purecvisormd -f (실시간)
 *   - 감사 로그: cat /var/log/purecvisor/audit.log | jq (JSON 파싱)
 */

#ifndef PURECVISOR_LOG_H
#define PURECVISOR_LOG_H

#include <glib.h>

G_BEGIN_DECLS

/* ── 로그 레벨 열거형 ────────────────────────────────── */

/**
 * PcvLogLevel - 로그 레벨 (낮은 값 = 더 상세)
 *
 * daemon.conf [logging] 섹션에서 모듈별 레벨을 설정할 수 있습니다:
 *   [logging]
 *   level=INFO              # 전역 기본 레벨
 *   rest_server=DEBUG       # rest_server 모듈만 DEBUG
 *   vm_manager=WARN         # vm_manager 모듈은 WARN 이상만
 *
 * GLogLevelFlags 순서(bitmask)와 달리 단순 정수 비교로 필터링합니다.
 */
typedef enum {
    PCV_LOG_LEVEL_DEBUG = 0,   /* 개발/디버깅 상세 */
    PCV_LOG_LEVEL_INFO  = 1,   /* 일반 정보 (기본) */
    PCV_LOG_LEVEL_WARN  = 2,   /* 경고 */
    PCV_LOG_LEVEL_ERROR = 3,   /* 에러 */
    PCV_LOG_LEVEL_NONE  = 99   /* 모든 로그 억제 (테스트용) */
} PcvLogLevel;

/* ── 초기화 ──────────────────────────────────────────── */

/**
 * pcv_log_init:
 * 로그 시스템 초기화. main.c에서 가장 먼저 1회 호출.
 *  - JOURNAL_STREAM 환경변수 감지 (systemd 자동 판별)
 *  - 감사 로그 파일 열기 (/var/log/purecvisor/audit.log)
 *  - GLib 기본 핸들러를 JSON 구조화 핸들러로 교체
 */
void pcv_log_init(void);

/**
 * pcv_log_shutdown:
 * 로그 파일 닫기 + 뮤텍스 정리. main.c cleanup에서 가장 나중에 호출.
 */
void pcv_log_shutdown(void);

/* ── req_id Thread Local Storage ────────────────────── */

/**
 * pcv_log_req_id_set:
 * 현재 스레드의 req_id를 설정합니다.
 * dispatcher.c에서 RPC 요청 수신 즉시 호출합니다.
 * 이후 해당 스레드의 모든 로그에 "req" 필드가 자동 포함됩니다.
 * NULL을 전달하면 req_id를 초기화(해제)합니다.
 */
void pcv_log_req_id_set(const gchar *req_id);

/**
 * pcv_log_req_id_get:
 * 현재 스레드의 req_id를 반환합니다.
 * 미설정 시 "-"를 반환합니다 (명시적 "요청 ID 없음" 표현).
 */
const gchar *pcv_log_req_id_get(void);

/* ── 내부 구현 함수 (매크로에서 호출) ───────────────── */

/**
 * _pcv_log:
 * PCV_LOG_* 매크로의 내부 구현. 가변인자를 처리하여 g_log()에 전달합니다.
 * G_GNUC_PRINTF(3, 4): GCC에게 3번째 인자가 printf 형식 문자열임을 알려
 *                        컴파일 시 형식 문자열 검증을 활성화합니다.
 */
void _pcv_log(GLogLevelFlags level,
              const gchar   *domain,
              const gchar   *fmt,
              ...) G_GNUC_PRINTF(3, 4);

/**
 * _pcv_log_audit:
 * PCV_LOG_AUDIT 매크로의 내부 구현. 감사 로그를 파일 + stderr에 이중 출력합니다.
 * G_GNUC_PRINTF(4, 5): 4번째 인자(fmt)가 printf 형식 문자열.
 *
 * @domain:    모듈 식별 (예: "handler_vm")
 * @operation: 작업 이름 (예: "vm.create", "container.stop")
 * @target:    작업 대상 (예: VM/컨테이너 이름)
 * @fmt:       추가 메시지 형식 문자열
 */
void _pcv_log_audit(const gchar *domain,
                    const gchar *operation,
                    const gchar *target,
                    const gchar *fmt,
                    ...) G_GNUC_PRINTF(4, 5);

/* ── 공개 매크로 ─────────────────────────────────────── */
/*
 * [레벨 매핑]
 *   PCV_LOG_DEBUG → G_LOG_LEVEL_DEBUG    (개발/디버깅 전용)
 *   PCV_LOG_INFO  → G_LOG_LEVEL_MESSAGE  (일반 정보)
 *   PCV_LOG_WARN  → G_LOG_LEVEL_WARNING  (경고, 동작에는 영향 없음)
 *   PCV_LOG_ERROR → G_LOG_LEVEL_CRITICAL (에러, abort 아님!)
 *
 * [왜 PCV_LOG_ERROR가 CRITICAL인가?]
 *   G_LOG_LEVEL_ERROR는 GLib 규약에 따라 g_abort()를 호출합니다.
 *   데몬이 에러 로그 때문에 종료되면 안 되므로
 *   CRITICAL(경고만 출력, abort 없음)을 사용합니다.
 *
 * [##__VA_ARGS__]
 *   GCC 확장: 가변인자가 없을 때 앞의 쉼표를 자동 제거합니다.
 *   PCV_LOG_INFO("dom", "no args") 같은 호출이 안전하게 동작합니다.
 */

#define PCV_LOG_DEBUG(dom, ...)  _pcv_log(G_LOG_LEVEL_DEBUG,    (dom), ##__VA_ARGS__)
#define PCV_LOG_INFO(dom, ...)   _pcv_log(G_LOG_LEVEL_MESSAGE,  (dom), ##__VA_ARGS__)
#define PCV_LOG_WARN(dom, ...)   _pcv_log(G_LOG_LEVEL_WARNING,  (dom), ##__VA_ARGS__)
#define PCV_LOG_ERROR(dom, ...)  _pcv_log(G_LOG_LEVEL_CRITICAL, (dom), ##__VA_ARGS__)

/**
 * PCV_LOG_AUDIT:
 * 감사 로그 매크로. VM/컨테이너 생성/삭제/시작/중지 등 중요 작업에 사용.
 * audit.log 파일 + stderr에 이중 출력됩니다.
 *
 * @dom:  모듈 도메인 문자열 (e.g. "handler_vm")
 * @op:   작업 이름 문자열 (e.g. "vm.create")
 * @tgt:  작업 대상 이름 (e.g. VM/컨테이너 이름)
 * @...:  메시지 포맷 + 인자
 *
 * 사용 예시:
 *   PCV_LOG_AUDIT("handler_vm", "vm.create", vm_name, "vcpu=%d mem=%dMB", vcpu, mem);
 */
#define PCV_LOG_AUDIT(dom, op, tgt, ...) \
    _pcv_log_audit((dom), (op), (tgt), ##__VA_ARGS__)

/* ── 모듈별 로그 레벨 제어 ──────────────────────────── */

/**
 * pcv_log_load_module_levels:
 * daemon.conf [logging] 섹션을 읽어 모듈별 로그 레벨을 설정합니다.
 * pcv_config_init() 이후에 호출해야 합니다.
 * SIGHUP 재로드 시에도 호출하여 변경사항을 반영합니다.
 */
void pcv_log_load_module_levels(void);

/**
 * pcv_log_set_module_level:
 * 특정 모듈(도메인)의 로그 레벨을 런타임에 설정합니다.
 * @domain: 모듈 도메인 문자열 (예: "rest_server", "vm_manager")
 * @level:  PcvLogLevel 값
 */
void pcv_log_set_module_level(const gchar *domain, PcvLogLevel level);

/**
 * pcv_log_get_module_level:
 * 특정 모듈의 로그 레벨을 조회합니다.
 * 모듈별 설정이 없으면 전역 레벨을 반환합니다.
 * @domain: 모듈 도메인 문자열
 * @return: PcvLogLevel 값
 */
PcvLogLevel pcv_log_get_module_level(const gchar *domain);

/**
 * pcv_log_set_global_level:
 * 전역 기본 로그 레벨을 설정합니다.
 * @level: PcvLogLevel 값
 */
void pcv_log_set_global_level(PcvLogLevel level);

/**
 * pcv_log_get_global_level:
 * 현재 전역 로그 레벨을 반환합니다.
 * @return: PcvLogLevel 값
 */
PcvLogLevel pcv_log_get_global_level(void);

/* ── Request ID 생성 ────────────────────────────────── */

/**
 * pcv_generate_request_id:
 * 고유한 요청 ID 문자열을 생성합니다. "req-XXXXXXXX" 형식 (8자리 16진수).
 * g_random_int() 기반으로 빠르고 충분히 유니크합니다.
 * 반환값은 호출자가 g_free()로 해제해야 합니다.
 */
gchar *pcv_generate_request_id(void);

/* ── W3C Trace Context (C-7) ───────────────────────── */

/**
 * PcvTraceContext — W3C Trace Context 추적 구조체
 *
 * W3C Trace Context Level 1 표준에 따른 분산 추적 컨텍스트.
 * traceparent 헤더 형식: "00-<trace_id 32hex>-<span_id 16hex>-<flags 2hex>"
 *
 * REST 서버에서:
 *   1. 수신 traceparent 헤더 파싱 → 기존 trace_id 유지, 새 span_id 생성
 *   2. 수신 헤더 없으면 → 새 trace_id + span_id 생성
 *   3. 응답 헤더에 traceparent 설정
 *   4. 로그에 trace_id, span_id 포함하여 분산 추적 가능
 */
typedef struct {
    gchar   trace_id[33];    /**< 32 hex + NUL (128-bit trace identifier) */
    gchar   span_id[17];     /**< 16 hex + NUL (64-bit span identifier) */
    gchar   parent_id[17];   /**< 16 hex + NUL (incoming span_id, 추적 parent) */
    guint8  flags;            /**< trace flags (0x01 = sampled) */
} PcvTraceContext;

/** 새 trace context 생성 (새 trace_id + span_id, flags=sampled) */
PcvTraceContext *pcv_trace_context_new(void);

/**
 * 수신 traceparent 헤더 파싱
 * @traceparent: "00-<32hex>-<16hex>-<2hex>" 형식 문자열
 * @return: 파싱된 context (기존 trace_id 유지, 새 span_id 생성), 실패 시 NULL
 */
PcvTraceContext *pcv_trace_context_parse(const gchar *traceparent);

/**
 * traceparent 헤더 문자열 생성
 * @return: "00-<trace_id>-<span_id>-<flags>" (호출자 g_free)
 */
gchar *pcv_trace_context_format(const PcvTraceContext *ctx);

/** trace context 해제 */
void pcv_trace_context_free(PcvTraceContext *ctx);

G_END_DECLS

#endif /* PURECVISOR_LOG_H */
