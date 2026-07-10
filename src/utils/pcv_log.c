/**
 * @file pcv_log.c
 * @brief 구조화된 JSON 로깅 시스템 — GLib 로그 핸들러 교체 + 감사 로그
 *
 * Sprint C-1에서 도입된 PureCVisor 전용 로깅 모듈입니다.
 * GLib 기본 로그 핸들러를 교체하여 모든 g_log/g_message/g_warning 호출을
 * 구조화된 JSON 한 줄 포맷으로 통일합니다.
 *
 * [아키텍처 위치]
 *   main.c → pcv_log_init() (가장 먼저 호출, pcv_config_init보다 앞)
 *          → 모든 모듈이 PCV_LOG_INFO/WARN/ERROR/DEBUG/AUDIT 매크로 사용
 *          → pcv_log_shutdown() (종료 시)
 *
 *   가장 먼저 초기화되는 이유:
 *     pcv_config_init()도 내부에서 PCV_LOG_INFO를 호출하므로,
 *     로깅 시스템이 먼저 준비되어야 합니다.
 *
 * [출력 형식]
 *   일반 로그 (stderr):
 *     {"t":"2025-03-07T14:30:00.123Z","lvl":"INFO","dom":"handler_vm","req":"abc-123","msg":"VM started"}
 *
 *   systemd journal 모드 (JOURNAL_STREAM 환경변수 감지 시):
 *     <6>{"t":"...","lvl":"INFO",...}    (syslog priority prefix 추가)
 *     → journalctl이 자동으로 우선순위를 파싱하여 색상/필터링 적용
 *
 *   감사 로그 (/var/log/purecvisor/audit.log):
 *     {"t":"...","lvl":"AUDIT","dom":"handler_vm","req":"...","op":"vm.create","target":"myvm","msg":"..."}
 *
 * [핵심 기능]
 *   1. req_id TLS (Thread Local Storage):
 *      GTask 워커 스레드에서도 요청 ID 자동 추적.
 *      dispatcher.c에서 요청 수신 시 pcv_log_req_id_set(rpc_id) 호출하면
 *      이후 해당 스레드의 모든 로그에 "req" 필드가 자동 포함됩니다.
 *      이를 통해 하나의 RPC 요청이 여러 모듈을 거치면서
 *      생성하는 모든 로그를 req_id로 연결하여 추적할 수 있습니다.
 *
 *   2. JSON 이스케이프:
 *      메시지 내 ", \, \n 등을 안전하게 이스케이프하여
 *      JSON 파싱 도구(jq, Logstash, Loki)에서 오류 없이 처리됩니다.
 *
 *   3. libvirt-gobject 노이즈 필터링:
 *      virEventAddHandle/Remove/Dispatch 등 이벤트루프 내부 디버그 메시지를
 *      프로덕션 로그에서 억제합니다. 5초마다 반복되는 스팸을 차단합니다.
 *      PURECVISOR_LIBVIRT_NOISE=1 환경변수로 억제 해제 가능.
 *
 *   4. 감사 로그 이중 출력:
 *      audit.log 파일(영속) + stderr(실시간) 동시 기록.
 *      보안 감사 추적과 실시간 모니터링을 모두 지원합니다.
 *
 * [다른 모듈과의 관계]
 *   - 모든 모듈: PCV_LOG_INFO/WARN/ERROR/DEBUG 매크로로 로그 출력
 *   - dispatcher.c:    pcv_log_req_id_set()으로 요청 ID 설정
 *   - handler_*.c:     PCV_LOG_AUDIT()으로 감사 이벤트 기록
 *   - audit_logger.c:  audit.log 파일과는 별도의 SQLite 감사 DB (pcv_audit.db)
 *
 * [스레드 안전]
 *   - req_id: GPrivate (스레드별 독립 저장소, 자동 g_free)
 *   - 감사 파일: audit_mutex로 write 동기화
 *   - 일반 로그: fprintf(stderr) 자체가 스레드 안전 (POSIX 보장)
 *
 * [주의사항]
 *   - G_LOG_LEVEL_ERROR 발생 시 g_abort() 호출 (프로세스 종료 — GLib 규약)
 *   - PCV_LOG_ERROR 매크로는 G_LOG_LEVEL_CRITICAL 사용 (abort 아님, 경고만)
 *   - audit.log 파일 열기 실패 시 stderr fallback (비치명적)
 *   - 로그 회전: logrotate(/etc/logrotate.d/purecvisor)가 관리 (30일 보존)
 */

#include "pcv_log.h"
#include "pcv_config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

/* ── 모듈 내부 상태 ───────────────────────────────────── */

/** 감사 로그 디렉터리 경로. pcv_log_init()에서 자동 생성 */
#define AUDIT_LOG_DIR   "/var/log/purecvisor"

/** 감사 로그 파일 경로. 보안 감사 추적용 (누가 무엇을 했는지 기록) */
#define AUDIT_LOG_PATH  "/var/log/purecvisor/audit.log"

/** pcv_log 자체의 로그 도메인 */
#define LOG_DOM "pcv_log"

/**
 * PcvLogState - 로깅 모듈 내부 상태 구조체
 *
 * 프로세스당 1개 싱글턴으로, pcv_log_init()에서 초기화됩니다.
 * 멀티스레드에서 접근되므로:
 *   - use_journal: 읽기 전용 (초기화 후 불변)
 *   - audit_fp: audit_mutex로 보호
 *   - initialized: 읽기 전용 (초기화 후 불변)
 *   - module_levels: module_levels_mu로 보호
 */
typedef struct {
    gboolean    use_journal;       /* systemd journal 모드 (JOURNAL_STREAM 감지) */
    FILE       *audit_fp;          /* 감사 로그 파일 핸들 (NULL이면 stderr fallback) */
    GMutex      audit_mutex;       /* 감사 파일 write 동기화 뮤텍스 */
    gboolean    initialized;       /* pcv_log_init() 호출 완료 여부 */
    PcvLogLevel global_level;      /* 전역 기본 로그 레벨 */
    GHashTable *module_levels;     /* "domain" → GINT_TO_POINTER(PcvLogLevel)
                                    * [주의: GINT_TO_POINTER(0)=NULL 함정]
                                    * DEBUG=0이므로 GINT_TO_POINTER(0)=NULL.
                                    * g_hash_table_lookup()이 NULL을 반환하면
                                    * "키 미존재"와 "DEBUG 레벨 설정"을 구분 불가.
                                    * → pcv_log_get_module_level()에서
                                    *   g_hash_table_lookup_extended() 사용으로 해결. */
    GMutex      module_levels_mu;  /* module_levels 해시 테이블 보호 */
} PcvLogState;

/** g_pcv_log_state - 전역 로깅 상태 싱글턴 */
static PcvLogState g_pcv_log_state = { 0 };

/**
 * g_req_id_key - 요청 ID Thread Local Storage (TLS) 키
 *
 * GPrivate: GLib의 스레드 로컬 저장소 구현.
 * 각 스레드가 독립적인 req_id 문자열을 가집니다.
 * G_PRIVATE_INIT(g_free): 스레드 종료 시 저장된 값을 g_free()로 자동 해제.
 *
 * [동작 흐름]
 *   1. dispatcher.c에서 RPC 수신: pcv_log_req_id_set("request-42")
 *   2. 핸들러 내 PCV_LOG_INFO 호출: _pcv_log_handler가 req_id를 자동 포함
 *   3. GTask 워커 스레드에서도 동일 (GPrivate는 스레드별 독립)
 *
 * [왜 GPrivate인가?]
 *   GTask 워커 스레드는 GLib 스레드 풀에서 관리됩니다.
 *   메인 스레드와 워커 스레드가 동시에 다른 요청을 처리하므로
 *   전역 변수로는 요청 ID가 섞입니다. TLS로 스레드별 격리합니다.
 */
static GPrivate g_req_id_key = G_PRIVATE_INIT(g_free);

/* ── 내부 유틸리티 ────────────────────────────────────── */

/**
 * _iso8601_now - RFC 3339 UTC 타임스탬프 생성
 *
 * @return: "2025-03-07T14:30:00.123Z" 형식 문자열 (호출자 g_free)
 *
 * [왜 UTC를 사용하는가?]
 *   3노드 클러스터의 로그를 시간순으로 정렬할 때
 *   각 노드의 로컬 시간대가 다르면 혼란이 발생합니다.
 *   UTC 통일로 시간 비교가 명확해집니다.
 *
 * [밀리초 정밀도]
 *   clock_gettime(CLOCK_REALTIME)은 나노초 정밀도를 제공하지만,
 *   로그에는 밀리초(3자리)로 충분합니다.
 *   나노초를 1,000,000으로 나누어 밀리초로 변환합니다.
 */
static gchar *
_iso8601_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);  /* 스레드 안전 UTC 변환 */

    /* "2025-03-07T14:30:00.123Z" — ISO 8601 형식 */
    return g_strdup_printf(
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
        tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
        tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
        ts.tv_nsec / 1000000L);   /* 나노초 → 밀리초 */
}

/**
 * _level_str - GLogLevelFlags를 문자열로 변환
 * @level: GLib 로그 레벨 플래그
 *
 * @return: 정적 문자열 포인터 (free 불필요)
 *
 * [레벨 매핑]
 *   G_LOG_LEVEL_ERROR    → "ERROR"  (g_abort 호출, 프로세스 종료)
 *   G_LOG_LEVEL_CRITICAL → "CRIT"   (PCV_LOG_ERROR 매크로에서 사용, abort 아님)
 *   G_LOG_LEVEL_WARNING  → "WARN"   (PCV_LOG_WARN 매크로)
 *   G_LOG_LEVEL_MESSAGE  → "INFO"   (PCV_LOG_INFO 매크로)
 *   G_LOG_LEVEL_INFO     → "INFO"   (GLib 내부)
 *   G_LOG_LEVEL_DEBUG    → "DEBUG"  (PCV_LOG_DEBUG 매크로)
 */
static const gchar *
_level_str(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_ERROR)    return "ERROR";
    if (level & G_LOG_LEVEL_CRITICAL) return "CRIT";
    if (level & G_LOG_LEVEL_WARNING)  return "WARN";
    if (level & G_LOG_LEVEL_MESSAGE)  return "INFO";
    if (level & G_LOG_LEVEL_INFO)     return "INFO";
    if (level & G_LOG_LEVEL_DEBUG)    return "DEBUG";
    return "UNKNOWN";
}

/**
 * _journal_prefix - systemd journal syslog 우선순위 prefix 생성
 * @level: GLib 로그 레벨 플래그
 *
 * @return: syslog priority prefix 문자열 (정적, free 불필요)
 *
 * [systemd journal 통합]
 *   systemd는 stderr에서 "<N>" 형식의 prefix를 자동 파싱합니다.
 *   N은 syslog 우선순위 번호:
 *     <3> = ERR     (journalctl에서 빨간색)
 *     <4> = WARNING (journalctl에서 노란색)
 *     <5> = NOTICE
 *     <6> = INFO    (journalctl 기본 표시)
 *     <7> = DEBUG   (journalctl --priority=debug에서만 표시)
 *
 *   이 prefix가 있으면 journalctl -p err 등으로 레벨 필터링이 가능합니다.
 */
static const gchar *
_journal_prefix(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_ERROR)    return "<3>";
    if (level & G_LOG_LEVEL_CRITICAL) return "<3>";
    if (level & G_LOG_LEVEL_WARNING)  return "<4>";
    if (level & G_LOG_LEVEL_MESSAGE)  return "<6>";
    if (level & G_LOG_LEVEL_INFO)     return "<6>";
    if (level & G_LOG_LEVEL_DEBUG)    return "<7>";
    return "<6>";
}

/**
 * _json_escape - JSON 문자열 이스케이프
 * @s: 이스케이프할 원본 문자열 (NULL 안전)
 *
 * @return: 이스케이프된 문자열 (호출자 g_free)
 *
 * [이스케이프 규칙 (JSON 사양 RFC 8259)]
 *   "  → \"   (JSON 문자열 종료 방지)
 *   \  → \\   (이스케이프 시퀀스 충돌 방지)
 *   \n → \\n  (줄바꿈 → 한 줄 유지)
 *   \r → \\r  (캐리지 리턴)
 *   \t → \\t  (탭)
 *   제어 문자(0x00~0x1F) → \\uXXXX (유니코드 이스케이프)
 *
 * [왜 필요한가?]
 *   로그 메시지에 에러 경로(/path/to/"file")나 줄바꿈이 포함되면
 *   JSON 파싱이 깨집니다. 이스케이프로 모든 메시지가 안전한 JSON이 됩니다.
 */
static gchar *
_json_escape(const gchar *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_sized_new(strlen(s) + 16);  /* 여유 있게 할당 */
    for (const gchar *p = s; *p; p++) {
        switch (*p) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n");  break;
            case '\r': g_string_append(out, "\\r");  break;
            case '\t': g_string_append(out, "\\t");  break;
            default:
                if ((guchar)*p < 0x20)
                    /* ASCII 제어 문자 → \uXXXX 유니코드 이스케이프 */
                    g_string_append_printf(out, "\\u%04x", (guchar)*p);
                else
                    g_string_append_c(out, *p);
                break;
        }
    }
    return g_string_free(out, FALSE);  /* FALSE: 내부 문자열 반환 (GString 헤더만 해제) */
}

/* ── 로그 레벨 파싱 + 필터링 ─────────────────────────── */

/**
 * _parse_log_level_str - 문자열을 PcvLogLevel로 변환
 * @s: 레벨 문자열 ("debug", "info", "warn", "error", 대소문자 무시)
 *
 * @return: PcvLogLevel 값, 인식 불가 시 PCV_LOG_LEVEL_INFO (기본)
 */
static PcvLogLevel
_parse_log_level_str(const gchar *s)
{
    if (!s) return PCV_LOG_LEVEL_INFO;
    if (g_ascii_strcasecmp(s, "debug") == 0) return PCV_LOG_LEVEL_DEBUG;
    if (g_ascii_strcasecmp(s, "info")  == 0) return PCV_LOG_LEVEL_INFO;
    if (g_ascii_strcasecmp(s, "warn")  == 0) return PCV_LOG_LEVEL_WARN;
    if (g_ascii_strcasecmp(s, "warning") == 0) return PCV_LOG_LEVEL_WARN;
    if (g_ascii_strcasecmp(s, "error") == 0) return PCV_LOG_LEVEL_ERROR;
    if (g_ascii_strcasecmp(s, "crit")  == 0) return PCV_LOG_LEVEL_ERROR;
    if (g_ascii_strcasecmp(s, "none")  == 0) return PCV_LOG_LEVEL_NONE;
    return PCV_LOG_LEVEL_INFO;
}

/**
 * _level_name - PcvLogLevel을 문자열로 변환 (표시용)
 */
static const gchar *
_level_name(PcvLogLevel lvl)
{
    switch (lvl) {
        case PCV_LOG_LEVEL_DEBUG: return "DEBUG";
        case PCV_LOG_LEVEL_INFO:  return "INFO";
        case PCV_LOG_LEVEL_WARN:  return "WARN";
        case PCV_LOG_LEVEL_ERROR: return "ERROR";
        case PCV_LOG_LEVEL_NONE:  return "NONE";
        default:                  return "INFO";
    }
}

/**
 * _glevel_to_pcvlevel - GLogLevelFlags → PcvLogLevel 변환
 * @level: GLib 로그 레벨 플래그
 *
 * PCV_LOG_DEBUG/INFO/WARN/ERROR 매크로가 사용하는 GLogLevelFlags를
 * 정수 비교 가능한 PcvLogLevel로 변환합니다.
 */
static PcvLogLevel
_glevel_to_pcvlevel(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_DEBUG)    return PCV_LOG_LEVEL_DEBUG;
    if (level & G_LOG_LEVEL_MESSAGE)  return PCV_LOG_LEVEL_INFO;
    if (level & G_LOG_LEVEL_INFO)     return PCV_LOG_LEVEL_INFO;
    if (level & G_LOG_LEVEL_WARNING)  return PCV_LOG_LEVEL_WARN;
    if (level & G_LOG_LEVEL_CRITICAL) return PCV_LOG_LEVEL_ERROR;
    if (level & G_LOG_LEVEL_ERROR)    return PCV_LOG_LEVEL_ERROR;
    return PCV_LOG_LEVEL_INFO;
}

/**
 * _should_log - 도메인+레벨 기반 로그 출력 여부 판정
 * @domain:    로그 도메인 문자열 (모듈명)
 * @msg_level: GLib 로그 레벨 플래그
 *
 * 모듈별 레벨이 설정되어 있으면 그것을 사용하고,
 * 없으면 전역 레벨로 폴백합니다.
 *
 * @return: TRUE이면 로그 출력, FALSE이면 억제
 */
static gboolean
_should_log(const gchar *domain, GLogLevelFlags msg_level)
{
    PcvLogLevel pcv_level = _glevel_to_pcvlevel(msg_level);

    /* G_LOG_LEVEL_ERROR(g_error/g_abort)는 항상 출력 — GLib 규약 */
    if (msg_level & G_LOG_LEVEL_ERROR)
        return TRUE;

    /* 모듈별 레벨 확인 (O(1) 해시 테이블 조회) */
    if (g_pcv_log_state.module_levels && domain) {
        g_mutex_lock(&g_pcv_log_state.module_levels_mu);
        gpointer val = g_hash_table_lookup(g_pcv_log_state.module_levels, domain);
        g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
        if (val) {
            PcvLogLevel module_level = (PcvLogLevel)GPOINTER_TO_INT(val);
            return pcv_level >= module_level;
        }
    }

    /* 전역 레벨로 폴백 */
    return pcv_level >= g_pcv_log_state.global_level;
}

/* ── GLib 로그 핸들러 교체 ────────────────────────────── */

/**
 * _is_libvirt_internal_noise - libvirt-gobject 내부 스팸 메시지 필터
 * @domain:  로그 도메인 (NULL이면 GLib 기본 도메인 → libvirt-gobject 의심)
 * @message: 로그 메시지 문자열
 *
 * @return: TRUE이면 억제할 노이즈 메시지
 *
 * libvirt-gobject 이벤트루프 내부 콜백 메시지 필터입니다.
 * virEventAddHandle / virEventUpdateHandle / virEventRemoveHandle,
 * virEventAddTimeout 등이 g_debug() (domain=NULL)로 쏟아내는
 * "Add handle", "Update handle", "Close GVirConnection" 류 스팸을
 * 프로덕션 로그에서 완전히 억제합니다.
 *
 * 이 메시지들은 5초마다 반복되어 로그 볼륨을 크게 증가시킵니다.
 *
 * [디버깅 시 억제 해제]
 *   환경변수 PURECVISOR_LIBVIRT_NOISE=1 설정 → 모든 메시지 출력
 *   예: PURECVISOR_LIBVIRT_NOISE=1 journalctl -u purecvisorsd -f
 *       PURECVISOR_LIBVIRT_NOISE=1 journalctl -u purecvisormd -f
 */
static gboolean
_is_libvirt_internal_noise(const gchar *domain, const gchar *message)
{
    /* domain이 NULL → GLib default domain (libvirt-gobject 내부 경로) */
    if (domain != NULL) return FALSE;
    if (!message)       return FALSE;

    /*
     * 정적 환경변수 플래그: 프로세스 수명 동안 1회만 조회
     * G_UNLIKELY: "거의 발생하지 않는 조건" 힌트 (초기화 최적화)
     */
    static gint show = -1;
    if (G_UNLIKELY(show < 0)) {
        const gchar *e = g_getenv("PURECVISOR_LIBVIRT_NOISE");
        show = (e && e[0] == '1') ? 1 : 0;
    }
    if (show) return FALSE;  /* 억제 해제 → 모든 메시지 출력 */

    /* libvirt-gobject 내부 이벤트루프 메시지 패턴 (prefix 매칭) */
    static const gchar * const NOISE_PREFIXES[] = {
        /* lifecycle: Add/Update/Remove handle/timeout */
        "Add handle ",      "Update handle ",      "Remove handle ",
        "Add timeout ",     "Update timeout ",     "Remove timeout ",
        /* dispatch (5초마다 반복되는 이벤트 디스패치 로그) */
        "Dispatch handler ","Dispatch timeout ",
        /* 연결 해제 (데몬 종료 시) */
        "Close GVirConnection",
        NULL
    };
    for (int i = 0; NOISE_PREFIXES[i]; i++) {
        if (g_str_has_prefix(message, NOISE_PREFIXES[i]))
            return TRUE;
    }
    return FALSE;
}

/**
 * _pcv_log_handler - GLib 기본 로그 핸들러 교체 함수
 * @log_domain: 로그 도메인 문자열 (모듈명 또는 NULL)
 * @log_level:  GLib 로그 레벨 플래그
 * @message:    로그 메시지
 * @user_data:  미사용 (NULL)
 *
 * g_log_set_default_handler()로 등록되어 모든 g_log/g_message/g_warning
 * 호출이 이 함수를 거칩니다.
 *
 * [출력 형식]
 *   {"t":"...", "lvl":"INFO", "dom":"module", "req":"req-42", "msg":"message"}
 *
 * [주의: G_LOG_LEVEL_ERROR]
 *   GLib 규약에 따라 G_LOG_LEVEL_ERROR 발생 시 g_abort()를 호출합니다.
 *   이는 GLib 내부에서 g_error() 매크로가 사용하는 레벨입니다.
 *   PCV_LOG_ERROR 매크로는 G_LOG_LEVEL_CRITICAL을 사용하므로 abort되지 않습니다.
 */
static void
_pcv_log_handler(const gchar    *log_domain,
                 GLogLevelFlags  log_level,
                 const gchar    *message,
                 gpointer        user_data __attribute__((unused)))
{
    /* libvirt-gobject 내부 이벤트루프 스팸 억제 */
    if (_is_libvirt_internal_noise(log_domain, message))
        return;

    /* 모듈별/전역 로그 레벨 필터링 */
    if (!_should_log(log_domain, log_level))
        return;

    gchar *ts      = _iso8601_now();
    const gchar *lvl = _level_str(log_level);
    const gchar *req = pcv_log_req_id_get();
    const gchar *dom = log_domain ? log_domain : "purecvisor";

    /* JSON 이스케이프: 메시지/도메인/req_id에 특수 문자가 포함될 수 있음 */
    gchar *msg_esc = _json_escape(message);
    gchar *dom_esc = _json_escape(dom);
    gchar *req_esc = _json_escape(req);

    /* JSON 한 줄 포맷 — Logstash/Loki/jq 호환 */
    gchar *line = g_strdup_printf(
        "{\"t\":\"%s\",\"lvl\":\"%s\",\"dom\":\"%s\",\"req\":\"%s\",\"msg\":\"%s\"}",
        ts, lvl, dom_esc, req_esc, msg_esc);

    if (g_pcv_log_state.use_journal) {
        /* systemd journal: syslog priority prefix + JSON */
        fprintf(stderr, "%s%s\n", _journal_prefix(log_level), line);
    } else {
        /* 비-systemd 환경: 순수 JSON 출력 */
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);   /* 즉시 flush — 크래시 직전 로그가 버퍼에 남는 것 방지 */

    g_free(line);
    g_free(msg_esc);
    g_free(dom_esc);
    g_free(req_esc);
    g_free(ts);

    /* [위험: g_abort] G_LOG_LEVEL_ERROR는 GLib 규약에 따라 프로세스를 강제 종료한다.
     * PCV_LOG_ERROR 매크로는 의도적으로 G_LOG_LEVEL_CRITICAL을 사용하여 abort를 피한다.
     * g_error() 호출이나 GLib 내부 assertion 실패만 여기에 도달한다.
     * 코드에서 g_error()를 직접 호출하지 말 것 — 대신 PCV_LOG_ERROR 사용. */
    if (log_level & G_LOG_LEVEL_ERROR)
        g_abort();
}

/* ── 공개 API ─────────────────────────────────────────── */

/**
 * pcv_log_init - 로깅 시스템 초기화
 *
 * main.c에서 가장 먼저 호출합니다 (pcv_config_init보다 앞).
 *
 * [초기화 순서]
 *   1. JOURNAL_STREAM 환경변수 감지 (systemd 환경 자동 판별)
 *      systemd가 프로세스를 실행하면 JOURNAL_STREAM=<dev>:<inode>를 설정합니다.
 *      이 변수가 있으면 stderr이 journal에 연결되어 있으므로
 *      syslog priority prefix를 추가합니다.
 *
 *   2. /var/log/purecvisor/ 디렉터리 생성 (0750)
 *      audit.log 파일을 저장할 디렉터리. 0750 = root 읽기/쓰기/실행 + 그룹 읽기/실행
 *
 *   3. /var/log/purecvisor/audit.log 파일 열기 (append 모드)
 *      실패 시 stderr fallback (비치명적)
 *
 *   4. GLib 기본 로그 핸들러를 _pcv_log_handler로 교체
 *      이후 모든 g_log/g_message/g_warning 호출이 JSON 형식으로 출력됩니다.
 */
void
pcv_log_init(void)
{
    /* JOURNAL_STREAM 감지: systemd가 프로세스를 실행하면 자동 설정됨.
     * 왜 이 감지가 필요한가:
     * systemd 환경에서는 <6>prefix로 syslog priority를 전달해야 journalctl이
     * 레벨별 색상 표시와 -p 필터링을 할 수 있다.
     * 비-systemd 환경(수동 실행)에서는 prefix가 의미 없는 깨진 문자로 보임. */
    const gchar *js = g_getenv("JOURNAL_STREAM");
    g_pcv_log_state.use_journal = (js && js[0] != '\0');

    /* 감사 로그 파일 열기 (append 모드: 기존 내용 보존) */
    if (g_mkdir_with_parents(AUDIT_LOG_DIR, 0750) == 0) {
        g_pcv_log_state.audit_fp = fopen(AUDIT_LOG_PATH, "a");
        if (!g_pcv_log_state.audit_fp)
            fprintf(stderr, "[pcv_log] Cannot open audit log %s: %s\n",
                    AUDIT_LOG_PATH, g_strerror(errno));
    }
    g_mutex_init(&g_pcv_log_state.audit_mutex);

    /* 모듈별 로그 레벨 해시 테이블 + 뮤텍스 초기화 */
    g_pcv_log_state.global_level = PCV_LOG_LEVEL_INFO;  /* 기본: INFO */
    g_pcv_log_state.module_levels = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_init(&g_pcv_log_state.module_levels_mu);

    /* GLib 기본 로그 핸들러 교체 → 모든 g_log 호출이 JSON으로 출력 */
    g_log_set_default_handler(_pcv_log_handler, NULL);

    /*
     * libvirt raw stderr 억제:
     * main.c 최초 진입점에서 g_setenv("LIBVIRT_LOG_OUTPUTS", "1:file:/dev/null")
     * 를 설정하므로 여기서는 별도 조치 불필요합니다.
     * virLogSetOutputs()는 libvirt 내부(private) API라 공개 헤더에 없습니다.
     */

    g_pcv_log_state.initialized = TRUE;

    /* 초기화 완료 메시지 — 이 시점부터 JSON 로깅 활성 */
    _pcv_log(G_LOG_LEVEL_MESSAGE, "pcv_log",
             "Logging initialized (journal=%s, audit=%s)",
             g_pcv_log_state.use_journal ? "yes" : "no",
             g_pcv_log_state.audit_fp   ? AUDIT_LOG_PATH : "stderr-fallback");
}

/**
 * pcv_log_shutdown - 로깅 시스템 종료
 *
 * 감사 로그 파일을 flush 후 닫고, 뮤텍스를 정리합니다.
 * main.c 종료 시 가장 나중에 호출합니다 (다른 모듈이 종료 로그를 출력할 수 있으므로).
 */
void
pcv_log_shutdown(void)
{
    if (g_pcv_log_state.audit_fp) {
        fflush(g_pcv_log_state.audit_fp);
        fclose(g_pcv_log_state.audit_fp);
        g_pcv_log_state.audit_fp = NULL;
    }
    if (g_pcv_log_state.initialized) {
        g_mutex_clear(&g_pcv_log_state.audit_mutex);
        if (g_pcv_log_state.module_levels) {
            g_hash_table_destroy(g_pcv_log_state.module_levels);
            g_pcv_log_state.module_levels = NULL;
        }
        g_mutex_clear(&g_pcv_log_state.module_levels_mu);
        g_pcv_log_state.initialized = FALSE;
    }
}

/**
 * pcv_log_req_id_set - 현재 스레드의 요청 ID 설정
 * @req_id: JSON-RPC 요청 ID 문자열 (NULL이면 해제)
 *
 * dispatcher.c에서 RPC 요청 수신 시 호출하여, 이후 해당 스레드의
 * 모든 로그에 "req" 필드가 자동 포함되도록 합니다.
 *
 * [사용 패턴 (dispatcher.c)]
 *   pcv_log_req_id_set(rpc_id);       // 요청 수신 시 설정
 *   handle_vm_start(params, ...);      // 핸들러 내 모든 로그에 req 포함
 *   pcv_log_req_id_set(NULL);          // 요청 처리 완료 후 해제
 *
 * [GPrivate(TLS) 동작]
 *   g_private_set(): 현재 스레드의 TLS에 값 저장
 *   이전 값이 있으면 G_PRIVATE_INIT에서 지정한 g_free()로 자동 해제
 *   NULL을 설정하면 이전 값이 해제되고 TLS가 비워집니다
 */
void
pcv_log_req_id_set(const gchar *req_id)
{
    g_private_replace(&g_req_id_key,
                      req_id ? g_strdup(req_id) : NULL);
}

/**
 * pcv_log_req_id_get - 현재 스레드의 요청 ID 조회
 *
 * @return: 요청 ID 문자열 또는 "-" (미설정 시)
 *
 * "-"를 반환하는 이유: JSON 로그에서 빈 문자열보다 "-"가
 * 명시적으로 "요청 ID 없음"을 나타내어 가독성이 좋습니다.
 * 데몬 시작/종료 시 로그처럼 RPC 요청과 무관한 로그에서 사용됩니다.
 */
const gchar *
pcv_log_req_id_get(void)
{
    const gchar *id = g_private_get(&g_req_id_key);
    return id ? id : "-";
}

/**
 * _pcv_log - 내부 로깅 함수 (PCV_LOG_* 매크로에서 호출)
 * @level:  GLib 로그 레벨 (G_LOG_LEVEL_MESSAGE, WARNING, CRITICAL 등)
 * @domain: 로그 도메인 문자열 (모듈명, 예: "handler_vm", "pcv_config")
 * @fmt:    printf 형식 문자열
 *
 * PCV_LOG_INFO/WARN/ERROR/DEBUG 매크로가 이 함수를 호출합니다.
 * va_list로 가변인자를 처리하여 g_log()에 전달합니다.
 * g_log()는 _pcv_log_handler()를 거쳐 JSON 형식으로 stderr에 출력됩니다.
 *
 * [왜 g_log()를 거치는가?]
 *   1. GLib 내부 로그(g_warning 등)와 통일된 핸들러 사용
 *   2. GLib의 로그 레벨 필터링 인프라 활용
 *   3. 향후 GLib structured logging으로 전환 시 호환성 유지
 */
void
_pcv_log(GLogLevelFlags level,
         const gchar   *domain,
         const gchar   *fmt,
         ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *msg = g_strdup_vprintf(fmt, args);
    va_end(args);

    /* GLib 핸들러 경유 → _pcv_log_handler 호출 → JSON 출력 */
    g_log(domain ? domain : G_LOG_DOMAIN, level, "%s", msg);
    g_free(msg);
}

/**
 * _pcv_log_audit - 감사 로그 기록 (PCV_LOG_AUDIT 매크로에서 호출)
 * @domain:    로그 도메인 (모듈명, 예: "handler_vm")
 * @operation: 수행 작업 (예: "vm.create", "vm.delete", "container.start")
 * @target:    작업 대상 (예: VM 이름 "web-prod", 컨테이너 이름)
 * @fmt:       추가 메시지 형식 문자열
 *
 * [감사 로그의 목적]
 *   보안 감사 추적: 누가(req_id), 무엇을(operation), 어디에(target) 했는지 기록.
 *   컴플라이언스 요구사항(금융, 의료 등)에서 필수적인 기능입니다.
 *
 * [이중 출력]
 *   1. /var/log/purecvisor/audit.log — 영속 감사 기록 (logrotate로 30일 보존)
 *   2. stderr (일반 로그) — 실시간 모니터링
 *      (journalctl -u purecvisorsd -f 또는 journalctl -u purecvisormd -f)
 *
 * [스레드 안전]
 *   audit_mutex로 파일 write를 동기화합니다.
 *   여러 RPC 핸들러가 동시에 감사 이벤트를 기록해도 안전합니다.
 *
 * [JSON 형식]
 *   {"t":"...", "lvl":"AUDIT", "dom":"handler_vm", "req":"req-42",
 *    "op":"vm.create", "target":"web-prod", "msg":"created successfully"}
 */
void
_pcv_log_audit(const gchar *domain,
               const gchar *operation,
               const gchar *target,
               const gchar *fmt,
               ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *msg = g_strdup_vprintf(fmt, args);
    va_end(args);

    gchar *ts      = _iso8601_now();
    const gchar *req = pcv_log_req_id_get();

    /* 모든 필드를 JSON 이스케이프 */
    gchar *msg_esc = _json_escape(msg);
    gchar *dom_esc = _json_escape(domain   ? domain    : "purecvisor");
    gchar *op_esc  = _json_escape(operation ? operation : "unknown");
    gchar *tgt_esc = _json_escape(target   ? target    : "-");
    gchar *req_esc = _json_escape(req);

    /* 감사 JSON 한 줄 — op와 target 필드 추가 */
    gchar *line = g_strdup_printf(
        "{\"t\":\"%s\",\"lvl\":\"AUDIT\",\"dom\":\"%s\","
        "\"req\":\"%s\",\"op\":\"%s\",\"target\":\"%s\",\"msg\":\"%s\"}",
        ts, dom_esc, req_esc, op_esc, tgt_esc, msg_esc);

    /* 감사 로그 파일에 기록 (audit_fp가 NULL이면 stderr fallback) */
    FILE *dest = g_pcv_log_state.audit_fp ? g_pcv_log_state.audit_fp : stderr;

    /* 뮤텍스로 파일 write 동기화 (멀티스레드 안전) */
    g_mutex_lock(&g_pcv_log_state.audit_mutex);
    fprintf(dest, "%s\n", line);
    fflush(dest);   /* 즉시 flush: 크래시 시에도 감사 기록 보존 */
    g_mutex_unlock(&g_pcv_log_state.audit_mutex);

    /* 일반 로그에도 동시 출력 (실시간 모니터링용) */
    if (g_pcv_log_state.use_journal) {
        fprintf(stderr, "<5>%s\n", line); /* syslog NOTICE 레벨 */
    } else {
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);

    /* 모든 동적 할당 문자열 해제 */
    g_free(line);
    g_free(msg_esc); g_free(dom_esc); g_free(op_esc);
    g_free(tgt_esc); g_free(req_esc);
    g_free(ts);
    g_free(msg);
}

/* ── 모듈별 로그 레벨 공개 API ───────────────────────── */

/**
 * pcv_log_set_global_level - 전역 기본 로그 레벨 설정
 */
void
pcv_log_set_global_level(PcvLogLevel level)
{
    g_pcv_log_state.global_level = level;
}

/**
 * pcv_log_get_global_level - 현재 전역 로그 레벨 조회
 */
PcvLogLevel
pcv_log_get_global_level(void)
{
    return g_pcv_log_state.global_level;
}

/**
 * pcv_log_set_module_level - 특정 모듈의 로그 레벨을 런타임에 설정
 * @domain: 모듈 도메인 문자열
 * @level:  PcvLogLevel 값
 */
void
pcv_log_set_module_level(const gchar *domain, PcvLogLevel level)
{
    if (!domain || !g_pcv_log_state.module_levels)
        return;

    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
    g_hash_table_insert(g_pcv_log_state.module_levels,
                        g_strdup(domain), GINT_TO_POINTER((gint)level));
    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
}

/**
 * pcv_log_get_module_level - 특정 모듈의 로그 레벨 조회
 * @domain: 모듈 도메인 문자열
 * @return: 모듈별 설정이 있으면 그 값, 없으면 전역 레벨
 */
PcvLogLevel
pcv_log_get_module_level(const gchar *domain)
{
    if (domain && g_pcv_log_state.module_levels) {
        g_mutex_lock(&g_pcv_log_state.module_levels_mu);
        /* [GINT_TO_POINTER(0)=NULL 버그 수정]
         * GLib의 GINT_TO_POINTER(0)은 NULL을 반환한다.
         * g_hash_table_lookup()은 키가 없어도 NULL을 반환한다.
         * 따라서 "DEBUG(0)가 설정됨"과 "설정 자체가 없음"을 구분할 수 없었다.
         *
         * lookup_extended는 found=TRUE/FALSE로 키 존재 여부를 명시적으로 알려주므로
         * val=NULL(=GINT_TO_POINTER(0)=DEBUG)이어도 "설정됨"으로 올바르게 처리된다.
         *
         * 이 버그가 있을 때의 증상: [logging] rest_server=DEBUG를 설정해도
         * rest_server 모듈이 전역 레벨(INFO)로 동작하여 디버그 로그가 안 나옴. */
        gpointer val = NULL;
        gboolean found = g_hash_table_lookup_extended(
            g_pcv_log_state.module_levels, domain, NULL, &val);
        g_mutex_unlock(&g_pcv_log_state.module_levels_mu);
        if (found)
            return (PcvLogLevel)GPOINTER_TO_INT(val);
    }
    return g_pcv_log_state.global_level;
}

/**
 * pcv_log_load_module_levels - daemon.conf [logging] 섹션에서 모듈별 레벨 로드
 *
 * pcv_config_init() 이후에 호출합니다. SIGHUP 재로드 시에도 호출 가능합니다.
 *
 * [설정 파일 형식]
 *   [logging]
 *   level=INFO              # 전역 기본 레벨 ([daemon] log_level 대비 우선)
 *   rest_server=DEBUG       # rest_server 도메인만 DEBUG
 *   vm_manager=WARN         # vm_manager 도메인은 WARN 이상만
 *   alert_engine=DEBUG      # alert_engine DEBUG
 *
 * [logging] 섹션이 없으면 [daemon] log_level을 전역 레벨로 사용합니다.
 */
void
pcv_log_load_module_levels(void)
{
    if (!g_pcv_log_state.module_levels)
        return;

    /* 1) 전역 레벨: [logging] level > [daemon] log_level > INFO 기본값 */
    const gchar *global_str = pcv_config_get_string("logging", "level", NULL);
    if (global_str) {
        g_pcv_log_state.global_level = _parse_log_level_str(global_str);
    } else {
        /* [daemon] log_level fallback (기존 설정 호환) */
        const gchar *daemon_level = pcv_config_get_log_level();
        g_pcv_log_state.global_level = _parse_log_level_str(daemon_level);
    }

    /* 2) 기존 모듈 오버라이드 초기화 */
    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
    g_hash_table_remove_all(g_pcv_log_state.module_levels);
    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);

    /* 3) [logging] 섹션의 모든 키를 읽어서 모듈별 레벨 등록 */
    const gchar *section = "logging";
    /* pcv_config_get_string는 GKeyFile 기반이므로 키를 직접 열거해야 한다.
     * pcv_config에 키 열거 API가 없으므로 잘 알려진 패턴으로 시도:
     * config에서 [logging] 섹션의 키 목록을 직접 읽는다. */
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, PCV_CONFIG_FILE_PATH,
                                  G_KEY_FILE_NONE, NULL)) {
        gsize n_keys = 0;
        gchar **keys = g_key_file_get_keys(kf, section, &n_keys, NULL);
        if (keys) {
            GString *overrides = g_string_new(NULL);
            for (gsize i = 0; i < n_keys; i++) {
                /* "level" 키는 전역 레벨 (이미 처리됨) */
                if (g_strcmp0(keys[i], "level") == 0)
                    continue;

                gchar *val = g_key_file_get_string(kf, section, keys[i], NULL);
                if (val) {
                    g_strstrip(val);
                    PcvLogLevel lvl = _parse_log_level_str(val);

                    g_mutex_lock(&g_pcv_log_state.module_levels_mu);
                    g_hash_table_insert(g_pcv_log_state.module_levels,
                                        g_strdup(keys[i]),
                                        GINT_TO_POINTER((gint)lvl));
                    g_mutex_unlock(&g_pcv_log_state.module_levels_mu);

                    if (overrides->len > 0)
                        g_string_append(overrides, ", ");
                    g_string_append_printf(overrides, "%s=%s",
                                           keys[i], _level_name(lvl));
                    g_free(val);
                }
            }

            /* 시작 시 모듈별 오버라이드 로그 출력 */
            _pcv_log(G_LOG_LEVEL_MESSAGE, LOG_DOM,
                     "Log levels: global=%s%s%s",
                     _level_name(g_pcv_log_state.global_level),
                     overrides->len > 0 ? ", overrides: " : "",
                     overrides->len > 0 ? overrides->str : " (no module overrides)");

            g_string_free(overrides, TRUE);
            g_strfreev(keys);
        }
    }
    g_key_file_free(kf);
}

/* ── Request ID 생성 ─────────────────────────────────── */

/**
 * pcv_generate_request_id — "req-XXXXXXXX" 형식의 고유 요청 ID 생성
 *
 * g_random_int()로 32비트 랜덤 값을 생성하고 8자리 16진수로 포맷합니다.
 * 분산 추적에는 UUID가 이상적이지만, 단일 프로세스 로컬 추적에는
 * 32비트 랜덤으로 충분합니다 (40억 가지 조합, 초당 수천 요청에서도 충돌 희박).
 *
 * @return: "req-XXXXXXXX" 문자열 (호출자 g_free)
 */
gchar *
pcv_generate_request_id(void)
{
    guint32 r = g_random_int();
    return g_strdup_printf("req-%08x", r);
}

/* ══════════════════════════════════════════════════════════════════════════
 * W3C Trace Context (C-7)
 *
 * W3C Trace Context Level 1 표준 지원.
 * traceparent 형식: "00-<trace_id 32hex>-<span_id 16hex>-<flags 2hex>"
 *
 * REST 서버에서 수신 traceparent를 파싱하여 기존 trace chain에 참여하거나,
 * 없으면 새 trace를 시작합니다. 응답에 항상 traceparent를 설정하여
 * 클라이언트/프록시가 분산 추적을 연결할 수 있게 합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _trace_fill_random_hex - 랜덤 바이트를 hex 문자열로 채움
 * @buf:       출력 버퍼 (최소 byte_count*2 + 1)
 * @byte_count: 생성할 랜덤 바이트 수 (hex 문자수 = byte_count*2)
 */
static void
_trace_fill_random_hex(gchar *buf, gint byte_count)
{
    for (gint i = 0; i < byte_count; i++) {
        guint8 b = (guint8)g_random_int_range(0, 256);
        g_snprintf(buf + i * 2, 3, "%02x", b);
    }
}

/**
 * _trace_is_valid_hex - 문자열이 정확히 expected_len자의 hex인지 검증
 */
static gboolean
_trace_is_valid_hex(const gchar *s, gsize expected_len)
{
    if (!s) return FALSE;
    for (gsize i = 0; i < expected_len; i++) {
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    }
    return s[expected_len] == '-' || s[expected_len] == '\0';
}

PcvTraceContext *
pcv_trace_context_new(void)
{
    PcvTraceContext *ctx = g_new0(PcvTraceContext, 1);
    _trace_fill_random_hex(ctx->trace_id, 16);  /* 16 bytes → 32 hex */
    _trace_fill_random_hex(ctx->span_id, 8);    /* 8 bytes → 16 hex */
    memset(ctx->parent_id, '0', 16);
    ctx->parent_id[16] = '\0';
    ctx->flags = 0x01;  /* sampled */
    return ctx;
}

PcvTraceContext *
pcv_trace_context_parse(const gchar *traceparent)
{
    /* W3C Trace Context Level 1 traceparent 형식 파싱:
     *
     * "00-<trace_id 32hex>-<parent_id 16hex>-<flags 2hex>"
     *  VV-TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT-PPPPPPPPPPPPPPPP-FF
     *  0  3                                36               53
     *
     * [왜 W3C 표준을 사용하는가]
     * Jaeger, Zipkin, OpenTelemetry 같은 분산 추적 시스템과 호환.
     * REST 요청에 traceparent 헤더가 있으면 기존 trace chain에 참여하고,
     * 없으면 새 trace를 시작하여 응답 헤더에 반환.
     * 이를 통해 클라이언트→REST→디스패처→핸들러 전체 경로를 추적 가능. */
    if (!traceparent || strlen(traceparent) < 55)
        return NULL;

    /* version 필드 (00 only) */
    if (traceparent[0] != '0' || traceparent[1] != '0' || traceparent[2] != '-')
        return NULL;

    /* trace-id: 위치 3~34 (32 hex) */
    if (!_trace_is_valid_hex(traceparent + 3, 32))
        return NULL;

    /* parent-id: 위치 36~51 (16 hex) */
    if (traceparent[35] != '-')
        return NULL;
    if (!_trace_is_valid_hex(traceparent + 36, 16))
        return NULL;

    /* flags: 위치 53~54 (2 hex) */
    if (traceparent[52] != '-')
        return NULL;

    PcvTraceContext *ctx = g_new0(PcvTraceContext, 1);

    /* trace_id 유지 (동일 trace chain) */
    g_strlcpy(ctx->trace_id, traceparent + 3, 33);

    /* 수신 span_id → parent_id로 보존 */
    g_strlcpy(ctx->parent_id, traceparent + 36, 17);

    /* 이 서비스의 새 span_id 생성 */
    _trace_fill_random_hex(ctx->span_id, 8);

    /* flags 파싱 */
    gchar flags_str[3] = { traceparent[53], traceparent[54], '\0' };
    ctx->flags = (guint8)g_ascii_strtoull(flags_str, NULL, 16);

    return ctx;
}

gchar *
pcv_trace_context_format(const PcvTraceContext *ctx)
{
    if (!ctx) return g_strdup("00-00000000000000000000000000000000-0000000000000000-00");
    return g_strdup_printf("00-%s-%s-%02x", ctx->trace_id, ctx->span_id, ctx->flags);
}

void
pcv_trace_context_free(PcvTraceContext *ctx)
{
    g_free(ctx);
}
