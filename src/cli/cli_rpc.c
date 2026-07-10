/**
 * @file cli_rpc.c
 * @brief CLI RPC 통신 — UDS 소켓 연결 + JSON-RPC 전송/수신 + 전역 컨텍스트
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  pcvctl(CLI 도구)의 핵심 통신 계층입니다. purecvisorctl.c(166개 커맨드 파서)에서
 *  분리되어, UDS 소켓 연결과 JSON-RPC 직렬화/역직렬화만 담당합니다.
 *
 *    purecvisorctl.c ──(purectl_send_request 호출)──→ cli_rpc.c (이 파일)
 *         │                                               │
 *         │                                               ▼
 *    cli_output.c ──(응답 포맷팅)                  UDS 소켓 연결
 *                                            /var/run/purecvisor/daemon.sock
 *                                                         │
 *                                                         ▼
 *                                                   현재 에디션 데몬
 *
 * ============================================================================
 *  주니어 개발자 필독
 * ============================================================================
 *
 *  1. PcvCtx (g_ctx) — 전역 실행 컨텍스트
 *     CLI의 모든 상태를 하나의 전역 구조체에 모아놓은 것입니다.
 *     - fmt: 출력 형식 (TABLE/JSON/PLAIN/CSV, --json 플래그로 전환)
 *     - interactive: readline REPL 모드 여부
 *     - batch: 배치 모드 (에러 시 즉시 종료)
 *     - no_color: --no-color 플래그 (파이프 출력 시 자동 감지)
 *     - verbose: --verbose 플래그 (RPC 페이로드 디버그 출력)
 *     - socket_path: UDS 소켓 경로 (기본: DAEMON_SOCK_PATH)
 *
 *  2. cc() / ce() — 조건부 컬러 코드 헬퍼
 *     터미널이 아니거나 --no-color 모드이면 빈 문자열을 반환합니다.
 *     cc()는 stdout용, ce()는 stderr용입니다.
 *     사용 예: printf("%s[OK]%s\n", cc(CYBER_GREEN), cc(CYBER_RESET));
 *
 *  3. purectl_send_request() — 핵심 RPC 전송 함수
 *     4단계로 동작: 소켓 연결 → 페이로드 조립 → 전송 → 수신
 *     g_socket_shutdown(SHUT_WR)으로 쓰기 종료 신호를 보내야
 *     데몬이 EOF를 감지하고 응답을 보냅니다.
 *     -32601 에러 감지 시 "클러스터 빌드 전용" 메시지를 출력합니다.
 * ============================================================================
 */
#include "cli_rpc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

/* ── 전역 컨텍스트 인스턴스 ────────────────────────────────────────────
 * CLI 실행 전반에 걸쳐 참조되는 싱글턴 상태.
 * main()에서 커맨드라인 인자를 파싱하여 필드를 갱신합니다.
 * ──────────────────────────────────────────────────────────────────── */
PcvCtx g_ctx = {
    .fmt         = FMT_TABLE,
    .interactive = false,
    .batch       = false,
    .no_color    = false,
    .verbose     = false,
    .socket_path = DAEMON_SOCK_PATH,
};

/* ── 조건부 컬러 코드 ─────────────────────────────────────────────────
 * 터미널(tty)에 출력할 때만 ANSI 이스케이프 코드를 반환합니다.
 * 파이프(`pcvctl vm list | grep web`)나 --no-color 모드에서는 빈 문자열을 반환하여
 * 색상 코드가 출력에 섞이는 것을 방지합니다.
 * ──────────────────────────────────────────────────────────────────── */
/** cc — stdout용 조건부 컬러 (터미널이 아니거나 no_color이면 빈 문자열) */
const char *cc(const char *code) {
    if (g_ctx.no_color || !isatty(STDOUT_FILENO)) return "";
    return code;
}
/** ce — stderr용 조건부 컬러 */
const char *ce(const char *code) {
    if (g_ctx.no_color || !isatty(STDERR_FILENO)) return "";
    return code;
}

/* ── UDS JSON-RPC 전송 구현 ────────────────────────────────────────── */
/**
 * purectl_send_request — JSON-RPC 2.0 요청을 UDS 소켓으로 전송하고 응답을 수신
 *
 * [호출 시점] CLI의 모든 커맨드 핸들러에서 RPC 호출 시
 * [동작]
 *   1단계: GSocketClient로 UDS 소켓 연결 (g_ctx.socket_path)
 *   2단계: JSON-RPC 2.0 페이로드 조립 (jsonrpc/method/params/id)
 *          json_node_take_object()로 root_obj 소유권을 root_node에 이전
 *   3단계: 페이로드를 소켓에 write_all 전송
 *          g_socket_shutdown(FALSE, TRUE)로 쓰기 EOF 전송
 *          (데몬이 EOF를 받아야 응답을 보냄 — TUI와 다른 프로토콜)
 *   4단계: 8KB 청크로 동적 버퍼 수신 (GByteArray)
 *
 * [주의]
 *   - params_obj가 NULL이면 빈 JsonObject를 자동 생성
 *   - verbose 모드에서 송/수신 페이로드를 stderr에 출력
 *   - -32601 에러 감지 시 "cluster build required" 경고 후 NULL 반환
 *     (Single Edge 바이너리에서 클러스터 전용 RPC 호출 시 발생)
 *   - 호출자가 반환된 gchar*를 g_free()해야 합니다.
 *
 * @param method     RPC 메서드 이름 (예: "vm.list")
 * @param params_obj 요청 파라미터 (NULL이면 빈 객체)
 * @param error      에러 출력 포인터
 * @return 응답 JSON 문자열 또는 NULL (에러/빈 응답)
 */
gchar *purectl_send_request(const gchar *method,
                            JsonObject  *params_obj,
                            GError     **error) {
    /* 1단계: UDS 소켓 연결 */
    GSocketClient    *client = g_socket_client_new();
    GSocketAddress   *addr   = g_unix_socket_address_new(g_ctx.socket_path);
    GSocketConnection *conn  = g_socket_client_connect(
            client, G_SOCKET_CONNECTABLE(addr), NULL, error);

    g_object_unref(client);
    g_object_unref(addr);
    if (!conn) return NULL;

    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_timeout(sock, 10);

    /* 2단계: JSON-RPC 2.0 페이로드 조립 */
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");
    json_object_set_string_member(root_obj, "method",  method);
    json_object_set_object_member(root_obj, "params",
            params_obj ? params_obj : json_object_new());
    json_object_set_int_member(root_obj, "id", 1);

    JsonNode      *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);
    gchar *payload = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    if (g_ctx.verbose) {
        /* B7-M1 (Phase 4): 민감 필드 마스킹 — password/secret/token/api_key */
        gchar *masked = g_strdup(payload);
        static const char *sensitive_keys[] = {
            "password", "secret", "token", "api_key", "apikey",
            "jwt_secret", "refresh_token", "access_token", NULL
        };
        for (int i = 0; sensitive_keys[i]; i++) {
            gchar *needle = g_strdup_printf("\"%s\":\"", sensitive_keys[i]);
            gchar *found = strstr(masked, needle);
            if (found) {
                gchar *val_start = found + strlen(needle);
                gchar *val_end = strchr(val_start, '"');
                if (val_end) {
                    gsize val_len = (gsize)(val_end - val_start);
                    memset(val_start, '*', val_len > 4 ? 4 : val_len);
                    if (val_len > 4) memmove(val_start + 4, val_end, strlen(val_end) + 1);
                }
            }
            g_free(needle);
        }
        g_printerr("%s[→ %s]%s\n", ce(CYBER_DIM), masked, ce(CYBER_RESET));
        g_free(masked);
    }

    /* 3단계: 페이로드 전송 */
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gsize bytes_written;
    if (!g_output_stream_write_all(out, payload, strlen(payload),
                                   &bytes_written, NULL, error)) {
        g_free(payload);
        g_object_unref(conn);
        return NULL;
    }
    g_free(payload);

    if (!g_socket_shutdown(sock, FALSE, TRUE, error)) {
        g_object_unref(conn);
        return NULL;
    }

    /* 4단계: 동적 버퍼 수신 */
    GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GByteArray   *buf = g_byte_array_new();
    gchar         tmp[8192];
    gssize        n;
    while ((n = g_input_stream_read(in, tmp, sizeof(tmp), NULL, error)) > 0)
        g_byte_array_append(buf, (guint8 *)tmp, (guint)n);

    g_object_unref(conn);

    if (buf->len == 0) {
        g_byte_array_free(buf, TRUE);
        return NULL;
    }
    g_byte_array_append(buf, (guint8 *)"\0", 1);
    gchar *result = g_strdup((gchar *)buf->data);
    g_byte_array_free(buf, TRUE);

    if (g_ctx.verbose)
        g_printerr("%s[← %s]%s\n", ce(CYBER_DIM), result, ce(CYBER_RESET));

    /* Single Edge 공개 리포에는 포함되지 않는 RPC 감지 */
    if (result && strstr(result, "-32601")) {
        g_printerr("\n%s[!] This command is not included in Single Edge.%s\n"
                   "%s    Current daemon does not support '%s' RPC method.%s\n"
                   "%s    Use the appropriate edition repository for this feature.%s\n\n",
            ce(CYBER_YELLOW), ce(CYBER_RESET),
            ce(CYBER_DIM), method, ce(CYBER_RESET),
            ce(CYBER_DIM), ce(CYBER_RESET));
        g_free(result);
        return NULL;
    }

    return result;
}
