/**
 * @file tui_rpc.c
 * @brief TUI RPC 통신 — UDS 소켓 JSON-RPC 전송/수신 + JSON 안전 접근 헬퍼
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  purecvisortui.c(TUI 메인 루프)에서 분리된 UDS 소켓 통신 계층입니다.
 *  ncurses와 무관한 순수 I/O 및 JSON 파싱 로직만 포함합니다.
 *
 *    purecvisortui.c ──(tui_send_request 호출)──→ tui_rpc.c (이 파일)
 *                                                     │
 *                                                     ▼
 *                                               UDS 소켓 연결
 *                                          /var/run/purecvisor/daemon.sock
 *                                                     │
 *                                                     ▼
 *                                               현재 에디션 데몬
 *                                               (디스패처 → 핸들러)
 *
 * ============================================================================
 *  주니어 개발자 필독
 * ============================================================================
 *
 *  1. safe_str / safe_double / safe_int
 *     JSON 응답에서 값을 추출할 때, 키가 없거나 타입이 다르면 크래시합니다.
 *     이 헬퍼들은 NULL 안전 + 타입 안전 접근을 제공합니다.
 *     json_object_get_string_member() 대신 safe_str()을 사용하세요.
 *
 *  2. format_bytes
 *     바이트 수를 사람이 읽기 쉬운 단위(G/M/K/B)로 변환합니다.
 *     메모리 사용량, 디스크 크기 표시에 사용.
 *
 *  3. tui_send_request
 *     JSON-RPC 2.0 요청을 UDS 소켓으로 전송하고 응답 문자열을 받습니다.
 *     json_node_take_object()로 소유권을 이전하여 메모리 누수를 방지합니다.
 *     타임아웃은 SOCK_TIMEOUT_S(tui_rpc.h에 정의, 보통 5초)입니다.
 * ============================================================================
 */
#include "tui_rpc.h"

#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

/* ── JSON 유틸 구현 ────────────────────────────────────────────────── */

/**
 * safe_str — JsonObject에서 문자열 값을 안전하게 추출
 *
 * [호출 시점] TUI에서 RPC 응답 JSON을 파싱할 때
 * [동작] obj가 NULL이거나 key가 없으면 def(기본값) 반환.
 *        key가 존재하지만 타입이 문자열이 아니면 def 반환.
 *        반환된 포인터는 JsonObject 내부 문자열이므로 g_free 금지.
 *
 * @param obj JsonObject (NULL 안전)
 * @param key 조회할 키
 * @param def 키가 없을 때 반환할 기본값
 * @return 문자열 값 또는 def
 */
const gchar *safe_str(JsonObject *obj, const gchar *key, const gchar *def) {
    if (!obj || !key || !json_object_has_member(obj, key)) return def;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return def;
    const gchar *v = json_node_get_string(n);
    return v ? v : def;
}

/**
 * safe_double — JsonObject에서 double 값을 안전하게 추출 (없으면 0.0)
 */
double safe_double(JsonObject *obj, const gchar *key) {
    if (!obj || !json_object_has_member(obj, key)) return 0.0;
    return json_object_get_double_member(obj, key);
}

/**
 * safe_int — JsonObject에서 정수 값을 안전하게 추출 (없으면 0)
 */
gint64 safe_int(JsonObject *obj, const gchar *key) {
    if (!obj || !json_object_has_member(obj, key)) return 0;
    return json_object_get_int_member(obj, key);
}

/**
 * format_bytes — 바이트 수를 사람이 읽기 쉬운 단위로 변환 (예: 1073741824 → "1.0G")
 *
 * @param b   바이트 수
 * @param out 출력 버퍼
 * @param sz  버퍼 크기
 */
void format_bytes(unsigned long long b, char *out, int sz) {
    if      (b >= (1ULL<<30)) snprintf(out, sz, "%.1fG", (double)b/(1<<30));
    else if (b >= (1ULL<<20)) snprintf(out, sz, "%.1fM", (double)b/(1<<20));
    else if (b >= (1ULL<<10)) snprintf(out, sz, "%.1fK", (double)b/(1<<10));
    else                      snprintf(out, sz, "%lluB",  b);
}

/* ── RPC 전송 구현 ─────────────────────────────────────────────────── */

/**
 * tui_send_request — JSON-RPC 2.0 요청을 UDS 소켓으로 전송하고 응답을 수신
 *
 * [호출 시점] TUI에서 VM 목록 조회, 상태 변경 등 모든 RPC 호출 시
 * [동작] 1) GSocketClient로 UDS 소켓 연결 (SOCKET_PATH, 타임아웃 SOCK_TIMEOUT_S)
 *        2) JSON-RPC 2.0 페이로드 조립 (jsonrpc/method/params/id)
 *        3) 개행 문자(\n)를 구분자로 전송 (UDS 서버가 줄 단위로 읽음)
 *        4) GDataInputStream으로 응답 1줄 수신
 *        5) 모든 GObject 자원 해제 후 응답 문자열 반환
 *
 * [주의] params가 NULL이면 빈 JsonObject를 자동 생성합니다.
 *        json_node_take_object()로 req의 소유권이 root에 이전됩니다.
 *        호출자가 반환된 gchar*를 g_free()해야 합니다.
 *        에러 시 NULL 반환 + *error에 GError 설정.
 *
 * @param method RPC 메서드 이름 (예: "vm.list", "telemetry.host")
 * @param params 요청 파라미터 JsonObject (NULL이면 빈 객체)
 * @param error  에러 출력 포인터
 * @return 응답 JSON 문자열 (호출자가 g_free 필요) 또는 NULL
 */
gchar *tui_send_request(const gchar *method, JsonObject *params, GError **error) {
    GSocketClient  *client = g_socket_client_new();
    GSocketAddress *addr   = g_unix_socket_address_new(SOCKET_PATH);
    g_socket_client_set_timeout(client, SOCK_TIMEOUT_S);

    GSocketConnection *conn = g_socket_client_connect(
        client, G_SOCKET_CONNECTABLE(addr), NULL, error);
    if (!conn) {
        g_object_unref(client); g_object_unref(addr); return NULL;
    }

    JsonObject *req = json_object_new();
    json_object_set_string_member(req, "jsonrpc", "2.0");
    json_object_set_string_member(req, "method",  method);
    json_object_set_object_member(req, "params",  params ? params : json_object_new());
    json_object_set_string_member(req, "id",      "tui-req");
    JsonNode      *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, req);
    gchar *data = json_to_string(root, FALSE);
    gsize  len  = strlen(data);

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    /* B7-M6: write_all 실패 시 read 시도 없이 조기 반환 */
    gchar *resp = NULL;
    if (!g_output_stream_write_all(out, data, len, NULL, NULL, error) ||
        !g_output_stream_write_all(out, "\n",  1,  NULL, NULL, error)) {
        goto out_cleanup;
    }

    GInputStream     *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *din = g_data_input_stream_new(in);
    resp = g_data_input_stream_read_line(din, NULL, NULL, error);
    g_object_unref(din);
out_cleanup:

    g_free(data);
    json_node_free(root);
    g_object_unref(conn);
    g_object_unref(addr); g_object_unref(client);
    return resp;
}
