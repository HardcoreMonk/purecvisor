/**
 * @file tui_rpc.h
 * @brief TUI RPC 통신 및 JSON 유틸 함수 선언
 *
 * purecvisortui.c에서 분리된 UDS 소켓 통신 계층.
 * JSON-RPC 2.0 요청 전송과 JSON 파싱 유틸을 제공한다.
 */
#ifndef TUI_RPC_H
#define TUI_RPC_H

#include <glib.h>
#include <json-glib/json-glib.h>

/* ── UDS 소켓 설정 ─────────────────────────────────────────────────── */
#define SOCKET_PATH     "/var/run/purecvisor/daemon.sock"
#define SOCK_TIMEOUT_S  10

/* ── JSON 유틸 ─────────────────────────────────────────────────────── */

const gchar *safe_str(JsonObject *obj, const gchar *key, const gchar *def);
double safe_double(JsonObject *obj, const gchar *key);
gint64 safe_int(JsonObject *obj, const gchar *key);
void format_bytes(unsigned long long b, char *out, int sz);

/* ── RPC 전송 ──────────────────────────────────────────────────────── */

gchar *tui_send_request(const gchar *method, JsonObject *params, GError **error);

#endif /* TUI_RPC_H */
