/**
 * @file cli_rpc.h
 * @brief CLI RPC 통신 함수 선언
 *
 * purecvisorctl.c에서 분리된 UDS 소켓 통신 및 전역 컨텍스트.
 */
#ifndef CLI_RPC_H
#define CLI_RPC_H

#include <stdbool.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include "purecvisor/version.h"

#define DAEMON_SOCK_PATH "/var/run/purecvisor/daemon.sock"
#define PCVCTL_VERSION   PCV_PRODUCT_VERSION

/* ── ANSI 256-Color 네온 팔레트 ─────────────────────────────────── */
#define CYBER_CYAN    "\x1b[38;5;51m"
#define CYBER_PINK    "\x1b[38;5;198m"
#define CYBER_YELLOW  "\x1b[38;5;226m"
#define CYBER_GREEN   "\x1b[38;5;46m"
#define CYBER_RED     "\x1b[38;5;196m"
#define CYBER_BLUE    "\x1b[38;5;33m"
#define CYBER_DIM     "\x1b[38;5;240m"
#define CYBER_RESET   "\x1b[0m"
#define CYBER_BOLD    "\x1b[1m"

/* ── 출력 포맷 ─────────────────────────────────────────────────────── */
typedef enum {
    FMT_TABLE = 0,
    FMT_JSON,
    FMT_PLAIN,
    FMT_CSV,
} OutputFormat;

/* ── 전역 CLI 컨텍스트 ────────────────────────────────────────────── */
typedef struct {
    OutputFormat  fmt;
    bool          interactive;
    bool          batch;
    bool          no_color;
    bool          verbose;
    const char   *socket_path;
} PcvCtx;

/** 전역 CLI 컨텍스트 — 프로세스 수명 동안 유일한 인스턴스 */
extern PcvCtx g_ctx;

/** cc / ce — 조건부 ANSI 컬러 코드 반환 (stdout / stderr) */
const char *cc(const char *code);
const char *ce(const char *code);

/**
 * purectl_send_request - UDS를 통해 JSON-RPC 2.0 요청 전송 후 응답 반환
 *
 * @method:     RPC 메서드명
 * @params_obj: JSON-RPC params 객체 (소유권은 이 함수가 인수)
 * @error:      GError 출력 매개변수
 * @return:     응답 JSON 문자열 (호출자가 g_free()로 해제), 실패 시 NULL
 */
gchar *purectl_send_request(const gchar *method,
                            JsonObject  *params_obj,
                            GError     **error);

#endif /* CLI_RPC_H */
