/**
 * @file purecvisorctl.c
 * @brief PureCVisor 데몬 제어 CLI (pcvctl) -- readline REPL + 배치 CLI
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  pcvctl은 현재 에디션 데몬과 UDS(Unix Domain Socket) JSON-RPC 2.0으로
 *  통신하는 명령줄 클라이언트이다. 사용자가 입력한 명령을 JSON-RPC 요청으로
 *  변환하여 /var/run/purecvisor/daemon.sock 에 전송하고, 응답을 파싱하여
 *  사람이 읽기 좋은 테이블(또는 JSON/CSV/plain) 형태로 출력한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  클라이언트 계층에 속한다. 데몬 내부 코드와 직접 링크되지 않으며,
 *  오직 소켓 I/O만으로 데몬과 소통한다.
 *
 *    사용자 입력
 *        |
 *    purecvisorctl (이 파일)
 *        | UDS JSON-RPC 2.0
 *    purecvisorsd (Single Edge 데몬)
 *        | dispatcher.c
 *    핸들러 -> libvirt / ZFS / LXC / OVS ...
 *
 * ====================================================================
 *  주요 흐름
 * ====================================================================
 *  1. main() 에서 GOptionContext로 --format, --batch, --socket 등 옵션 파싱
 *  2. 인수 있으면 단발 실행(one-shot), 없으면 readline REPL 진입
 *  3. 명령 문자열을 Command Table(CmdEntry 배열)에서 접두사 매칭
 *  4. 매칭된 핸들러가 JSON-RPC params 객체를 조립
 *  5. send_rpc_request()로 UDS 소켓에 전송, 응답 JSON 수신
 *  6. OutputFormat(TABLE/JSON/PLAIN/CSV)에 따라 결과 포맷팅 출력
 *
 * ====================================================================
 *  핵심 패턴
 * ====================================================================
 *  - Command Table Routing: CmdEntry 구조체 배열로 명령어-핸들러 매핑.
 *    새 명령 추가 시 배열에 항목 하나 추가하면 자동 라우팅된다.
 *  - fire-and-forget 불필요: CLI는 동기 요청-응답이므로 send 후 recv 대기.
 *  - ANSI 256-Color: CYBER_* 매크로로 네온 팔레트 정의.
 *    --no-color 또는 비-tty 환경에서는 컬러 비활성화.
 *  - GIO GSocketClient: UDS 연결에 GLib GIO 소켓 API 사용.
 *
 * ====================================================================
 *  지원 명령 범주 (데몬 253 RPC 중 CLI 노출 172개 — routes[] 표 기준, 대표 범주만 열거)
 * ====================================================================
 *  - VM: create/start/stop/delete/list/metrics/vnc/snapshot 등
 *  - Network: create/delete/list/mode_set/bind_phys/dhcp_toggle
 *  - Storage: pool.list/zvol.list/zvol.create/zvol.delete
 *  - Container: create/destroy/start/stop/list/metrics/exec/snapshot
 *  - OVN: switch/router/acl/nat/dhcp/status (6개; Cluster/migrate 명령은 single edition 미제공)
 *  - RBAC: auth create/list/delete/role
 *  - Template: list/get/create/delete
 *  - Backup: set/list/delete/history/restore
 *  - DPDK/SR-IOV: status/bind/unbind/list/bridge/hugepage/enable 등
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - 소켓 경로 기본값: /var/run/purecvisor/daemon.sock
 *    --socket 옵션으로 오버라이드 가능.
 *  - readline이 없는 환경(HAVE_READLINE 미정의)에서는 fgets 폴백.
 *  - 파이프 입력(--batch 또는 stdin이 비-tty)일 때 프롬프트/컬러 비활성화.
 *  - 이 파일은 ~6,800+ LOC 단일 파일 구조이다.
 *
 * ====================================================================
 *  새 커맨드 추가 방법 (주니어 개발자 필독)
 * ====================================================================
 *  예: "pcvctl foo bar --baz 123" 명령을 추가한다고 가정.
 *
 *  1단계: 핸들러 함수 작성 (이 파일 내, cmd_xxx 섹션에)
 *    void cmd_foo_bar(int argc, char *argv[]) {
 *        if (argc < 3) { printf("Usage: ...\n"); return; }
 *        JsonObject *params = json_object_new();
 *        json_object_set_string_member(params, "baz", argv[3]);
 *        GError *error = NULL;
 *        gchar *resp = purectl_send_request("foo.bar", params, &error);
 *        if (error) { g_printerr("[!] %s\n", error->message); g_error_free(error); return; }
 *        print_action_response(resp, "FOO_BAR");
 *        g_free(resp);
 *    }
 *
 *  2단계: routes[] 배열에 항목 추가
 *    {"foo", "bar", cmd_foo_bar, "설명 텍스트"},
 *
 *  3단계: 빌드 확인
 *    make clean && make cli  (경고 0 확인)
 *
 *  그러면 자동으로:
 *    - route_exec()가 "foo bar" 입력을 cmd_foo_bar로 라우팅
 *    - readline 자동완성에 "foo bar" 등록
 *    - print_help()에 도움말 표시
 *    - --format=json/plain/csv 출력 자동 지원 (print_action_response 사용 시)
 *
 *  핵심 함수 (cli_rpc.c에서 제공):
 *    purectl_send_request(method, params, &error) → JSON 응답 문자열
 *    cc(CYBER_*) / ce(CYBER_*) → 조건부 컬러 코드
 *
 *  핵심 함수 (cli_output.c에서 제공):
 *    print_action_response(resp, "LABEL") → 성공/에러 포맷팅 출력
 *    print_raw_response(resp) → JSON 원본 출력
 *    ptbl_new / ptbl_row / ptbl_print_xxx / ptbl_free -- 테이블 출력
 *
 * ====================================================================
 *  커맨드 디스패치 패턴 (CommandRoute 구조체)
 * ====================================================================
 *  typedef struct {
 *      const char *object;      // "vm", "network", "storage" 등
 *      const char *action;      // "list", "create", "delete" 등
 *      CmdHandler  handler;     // void (*)(int argc, char *argv[])
 *      const char *help_text;   // 도움말 문자열
 *  } CommandRoute;
 *
 *  routes[] 배열을 순차 순회하여 object+action이 일치하는 첫 항목의
 *  handler를 호출한다. O(n) 선형 탐색이지만 172개 항목이라 충분히 빠르다.
 *
 * ====================================================================
 *  버전 이력
 * ====================================================================
 *  v1.0: --format 전환, REPL, Batch, 자동완성(completion/)
 *  Sprint I~N: OVN/RBAC/Template/Backup/DPDK/SR-IOV 명령 추가
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wordexp.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

#ifdef HAVE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

/* ── 분리된 모듈 include ──────────────────────────────────────────── */
#include "cli_rpc.h"       /* RPC 통신 + 전역 컨텍스트 (g_ctx, cc, ce, purectl_send_request) */
#include "cli_output.h"    /* 출력 포맷팅 (배너, 메트릭 바, PcvTable) */

// CYBER_* 컬러 상수 → cli_rpc.h로 이동

// OutputFormat, PcvCtx, g_ctx, cc(), ce() → cli_rpc.h/cli_rpc.c로 이동

/*
 * print_metrics_bar, purectl_send_request, print_raw_response,
 * print_action_response, PcvTable → cli_rpc.c / cli_output.c로 이동
 */
// [REMOVED BLOCK START — will be cleaned up below]
// The following comment replaces ~330 lines of extracted functions.
// They now reside in cli_rpc.c and cli_output.c.
static inline void _cli_extracted_stub(void) {
    (void)0; // This function exists only to keep the old code syntactically valid
              // while we incrementally remove the remaining duplicate definitions below.
}
// We cannot remove the entire block in one Edit call due to uniqueness constraints,
// so we mark the old function bodies below as unreachable.
#if 0 /* --- EXTRACTED CODE BEGIN (print_metrics_bar) --- */
static void _cli_removed_print_metrics_bar(const char *label, int percent, const char *color) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        printf("%s\t%d\n", label, percent);
        return;
    }
    printf("%s[ %-8s ] %s%s", cc(CYBER_CYAN), label, cc(CYBER_RESET), color);
    for (int i = 0; i < 20; i++) {
        if (i < percent / 5) printf("▰");
        else printf("%s▱%s%s", cc(CYBER_DIM), cc(CYBER_RESET), color);
    }
    printf(" %3d%%%s\n", percent, cc(CYBER_RESET));
}

/* ── UDS JSON-RPC 전송 ──────────────────────────────────────────── */

/**
 * purectl_send_request - UDS를 통해 JSON-RPC 2.0 요청 전송 후 응답 반환
 *
 * 교착 방지: write_all() 완료 즉시 g_socket_shutdown(SHUT_WR) 호출.
 * 64KB 초과 대응: GByteArray 동적 확장 수신 (Sprint H 개선)
 */
/**
 * purectl_send_request - UDS를 통해 JSON-RPC 2.0 요청 전송 후 응답 반환
 *
 * @method:     RPC 메서드명 (예: "vm.list", "vm.create")
 * @params_obj: JSON-RPC params 객체 (NULL이면 빈 객체 {}로 대체)
 *              이 함수에 전달된 params_obj의 소유권은 json_object_set_object_member()를
 *              통해 root_obj로 이전되므로, 호출자가 별도로 해제할 필요 없다.
 * @error:      GError 출력 매개변수 (연결/송수신 실패 시 설정)
 *
 * @return: 서버 응답 JSON 문자열 (호출자가 g_free()로 해제), 실패 시 NULL
 *
 * 동작 흐름:
 *   1. GSocketClient로 UDS 연결 (g_ctx.socket_path)
 *   2. JSON-RPC 2.0 페이로드 직렬화 (json-glib JsonGenerator)
 *   3. write_all()로 전송 → 즉시 SHUT_WR (교착 방지)
 *   4. GByteArray 동적 버퍼로 응답 수신 (64KB 초과 대응)
 *   5. NULL 종료 문자열로 변환하여 반환
 *
 * 교착 방지 메커니즘:
 *   서버(현재 에디션 데몬)는 클라이언트가 EOF를 보낼 때까지 read()를 반복한다.
 *   g_socket_shutdown(SHUT_WR)로 쓰기 종료 신호를 보내야 서버가 응답을 전송한다.
 *   이 호출이 없으면 클라이언트와 서버 모두 서로의 데이터를 기다리며 교착된다.
 */
gchar *purectl_send_request(const gchar *method,
                             JsonObject  *params_obj,
                             GError     **error) {
    /* 1단계: UDS 소켓 연결 */
    GSocketClient    *client = g_socket_client_new();
    GSocketAddress   *addr   = g_unix_socket_address_new(g_ctx.socket_path);
    GSocketConnection *conn  = g_socket_client_connect(
            client, G_SOCKET_CONNECTABLE(addr), NULL, error);

    g_object_unref(client);  /* 연결 후 클라이언트 객체는 불필요 */
    g_object_unref(addr);
    if (!conn) return NULL;  /* 연결 실패 시 error에 원인이 설정됨 */

    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_timeout(sock, 10);  /* 10초 타임아웃 — 데몬 무응답 방지 */

    /* 2단계: JSON-RPC 2.0 페이로드 조립 */
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");
    json_object_set_string_member(root_obj, "method",  method);
    json_object_set_object_member(root_obj, "params",
            params_obj ? params_obj : json_object_new());
    json_object_set_int_member(root_obj, "id", 1);

    /* JsonGenerator로 JSON 문자열 직렬화 */
    JsonNode      *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);  /* root_obj 소유권 이전 */
    gchar *payload = json_to_string(root_node, FALSE);
    json_node_free(root_node);  /* root_obj도 함께 해제됨 */

    /* --verbose 모드: 송신 페이로드를 stderr에 덤프 */
    if (g_ctx.verbose)
        g_printerr("%s[→ %s]%s\n", ce(CYBER_DIM), payload, ce(CYBER_RESET));

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

    /* SHUT_WR: 쓰기 방향 종료 → 서버에 EOF 전달 → 교착 방지 핵심 */
    if (!g_socket_shutdown(sock, FALSE, TRUE, error)) {
        g_object_unref(conn);
        return NULL;
    }

    /* 4단계: 동적 버퍼 수신 — GByteArray로 가변 길이 응답 처리 */
    GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GByteArray   *buf = g_byte_array_new();
    gchar         tmp[8192];
    gssize        n;
    while ((n = g_input_stream_read(in, tmp, sizeof(tmp), NULL, error)) > 0)
        g_byte_array_append(buf, (guint8 *)tmp, (guint)n);

    g_object_unref(conn);

    /* 빈 응답 처리 (데몬 비정상 종료 등) */
    if (buf->len == 0) {
        g_byte_array_free(buf, TRUE);
        return NULL;
    }
    /* NULL 종료 문자 추가 → C 문자열 변환 */
    g_byte_array_append(buf, (guint8 *)"\0", 1);
    gchar *result = g_strdup((gchar *)buf->data);
    g_byte_array_free(buf, TRUE);

    /* --verbose 모드: 수신 응답을 stderr에 덤프 */
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

/* ════════════════════════════════════════════════════════════════════
 *  출력 포맷 시스템 (Sprint H 신규)
 * ════════════════════════════════════════════════════════════════════
 *
 *  FMT_JSON  → print_raw_response() 로 원본 JSON 출력
 *  FMT_TABLE → 기존 CYBER 컬러 테이블 (변경 없음)
 *  FMT_PLAIN → pcv_table_plain()
 *  FMT_CSV   → pcv_table_csv()
 */

/* JSON 응답 원본 출력 (FMT_JSON 전용) */
static void print_raw_response(const gchar *json_string) {
    if (json_string) printf("%s\n", json_string);
}

/* 공통 응답 출력 — 성공/실패 메시지 */
void print_action_response(const gchar *json_string, const gchar *action_name) {
    if (!json_string) {
        g_printerr("%s[!] NULL RESPONSE%s\n", ce(CYBER_RED), ce(CYBER_RESET));
        return;
    }
    /* FMT_JSON: 파싱 없이 raw 출력 */
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(json_string);
        return;
    }
    /* FMT_PLAIN / FMT_CSV: 간결한 상태 출력 */
    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        /* 성공 여부만 탭 구분으로 출력 */
        GError     *err    = NULL;
        JsonParser *parser = json_parser_new();
        bool        ok     = false;
        if (json_parser_load_from_data(parser, json_string, -1, &err)) {
            JsonObject *root = json_node_get_object(json_parser_get_root(parser));
            ok = json_object_has_member(root, "result");
        }
        g_clear_error(&err);
        g_object_unref(parser);
        printf("%s\t%s\n", action_name, ok ? "OK" : "ERROR");
        return;
    }

    /* FMT_TABLE: 기존 CYBER 출력 */
    GError     *error  = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_string, -1, &error)) {
        g_printerr("%s[!] SYS_FAULT: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        g_object_unref(parser);
        return;
    }
    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));

    if (json_object_has_member(root_obj, "error")) {
        JsonObject *err_obj = json_object_get_object_member(root_obj, "error");
        g_printerr("%s[!] COMMAND REJECTED [%lld]: %s%s\n",
            ce(CYBER_RED),
            (long long)json_object_get_int_member(err_obj, "code"),
            json_object_get_string_member(err_obj, "message"),
            ce(CYBER_RESET));
    } else if (json_object_has_member(root_obj, "result")) {
        JsonNode *res_node = json_object_get_member(root_obj, "result");
        if (res_node && JSON_NODE_HOLDS_OBJECT(res_node)) {
            JsonObject  *res_obj = json_node_get_object(res_node);
            const gchar *status  = json_object_has_member(res_obj, "status")
                    ? json_object_get_string_member(res_obj, "status") : "SUCCESS";
            printf("%s%s[+] %s COMMAND ACCEPTED: %s",
                cc(CYBER_GREEN), cc(CYBER_BOLD), action_name, cc(CYBER_RESET));
            printf("%sEntity state transitioned to %s",
                cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s[ %s ]%s\n",
                cc(CYBER_YELLOW), status, cc(CYBER_RESET));
            if (json_object_has_member(res_obj, "dhcp_warning"))
                printf("%s[!] DHCP_WARN: %s%s\n",
                    cc(CYBER_YELLOW),
                    json_object_get_string_member(res_obj, "dhcp_warning"),
                    cc(CYBER_RESET));
        } else {
            printf("%s%s[+] %s SEQUENCE INITIATED SUCCESSFULLY.%s\n",
                cc(CYBER_GREEN), cc(CYBER_BOLD), action_name, cc(CYBER_RESET));
        }
    }
    g_object_unref(parser);
}

/* ── 간단 테이블 렌더러 (PLAIN / CSV 공용) ──────────────────────── */
/*
 * PcvTable: 경량 인메모리 테이블 구조체.
 * ptbl_new()로 생성, ptbl_row()로 행 추가,
 * ptbl_print_plain()/ptbl_print_csv()로 출력, ptbl_free()로 해제.
 *
 * FMT_PLAIN: 탭 구분, 헤더 없음 (awk/cut 파이프 호환)
 * FMT_CSV:   CSV RFC 4180 (콤마 구분, 특수문자 이스케이프)
 */

/**
 * PcvTable - 경량 인메모리 테이블 (FMT_PLAIN / FMT_CSV 출력용)
 *
 * FMT_TABLE은 각 cmd_* 함수에서 직접 printf로 출력하지만,
 * FMT_PLAIN/FMT_CSV는 이 구조체를 통해 구조화된 출력을 생성한다.
 *
 * 사용 패턴:
 *   PcvTable *t = ptbl_new(hdrs, ncols);  // 테이블 생성
 *   ptbl_row(t, "val1", "val2", NULL);    // 행 추가 (가변인자, NULL 종료 아님)
 *   ptbl_print_plain(t);                  // 또는 ptbl_print_csv(t)
 *   ptbl_free(t);                         // 해제
 *
 * 메모리 관리:
 *   - headers 포인터는 호출자의 const 배열을 참조만 한다 (복사하지 않음).
 *   - rows 내부의 gchar** 행은 g_strdup()으로 복사되며,
 *     GPtrArray의 free_func(g_strfreev)가 자동 해제한다.
 */
typedef struct {
    const char **headers;   /* 열 제목 배열 (호출자 소유, 복사 안 함) */
    size_t       ncols;     /* 열 개수 */
    GPtrArray   *rows;      /* GPtrArray of gchar** (NULL-terminated 행 배열) */
} PcvTable;

/** ptbl_new - 테이블 생성. hdrs=열 제목 배열, n=열 개수 */
static PcvTable *ptbl_new(const char **hdrs, size_t n) {
    PcvTable *t = g_new0(PcvTable, 1);
    t->headers = hdrs;
    t->ncols   = n;
    t->rows    = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    return t;
}

/**
 * ptbl_row - 테이블에 행 추가 (가변인자)
 *
 * ncols개의 const char* 인수를 받아 행을 추가한다.
 * 각 값은 g_strdup()으로 복사되므로 원본 수명과 무관하다.
 * NULL 값은 빈 문자열("")로 대체된다.
 *
 * 주의: va_arg로 ncols개만 읽으므로, 인수 개수를 정확히 맞춰야 한다.
 *       컴파일러가 가변인자 개수를 검증하지 않으므로 주의할 것.
 */
static void ptbl_row(PcvTable *t, ...) {
    va_list ap; va_start(ap, t);
    gchar **row = g_new0(gchar *, t->ncols + 1);
    for (size_t i = 0; i < t->ncols; i++) {
        const char *v = va_arg(ap, const char *);
        row[i] = g_strdup(v ? v : "");
    }
    va_end(ap);
    g_ptr_array_add(t->rows, row);
}

static void ptbl_print_plain(PcvTable *t) {
    for (guint r = 0; r < t->rows->len; r++) {
        gchar **row = g_ptr_array_index(t->rows, r);
        for (size_t c = 0; c < t->ncols; c++) {
            if (c) putchar('\t');
            fputs(row[c], stdout);
        }
        putchar('\n');
    }
}

static void ptbl_print_csv(PcvTable *t) {
    /* 헤더 */
    for (size_t c = 0; c < t->ncols; c++) {
        if (c) putchar(',');
        fputs(t->headers[c], stdout);
    }
    putchar('\n');
    for (guint r = 0; r < t->rows->len; r++) {
        gchar **row = g_ptr_array_index(t->rows, r);
        for (size_t c = 0; c < t->ncols; c++) {
            if (c) putchar(',');
            bool needs_quote = (strchr(row[c], ',') || strchr(row[c], '"')
                             || strchr(row[c], '\n'));
            if (needs_quote) {
                putchar('"');
                for (char *p = row[c]; *p; p++) {
                    if (*p == '"') putchar('"');
                    putchar(*p);
                }
                putchar('"');
            } else {
                fputs(row[c], stdout);
            }
        }
        putchar('\n');
    }
}

static void ptbl_free(PcvTable *t) {
    g_ptr_array_free(t->rows, TRUE);
    g_free(t);
}
#endif /* --- EXTRACTED CODE END (ptbl_free) --- */

/* ════════════════════════════════════════════════════════════════════
 *  VM 명령
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_vm_create - VM 생성 명령 핸들러
 *
 * Usage: pcvctl vm create <name> [--vcpu N] [--memory_mb N]
 *        [--disk_size_gb N] [--iso_path P] [--network_bridge B|none]
 *        (B 미지정 → 관리형 기본 NAT 네트워크, "none" → NIC 미부착 — VP-1)
 *
 * JSON-RPC "vm.create" 호출. 옵션 플래그를 파싱하여 params에 추가.
 * 파라미터 검증은 서버 측 dispatcher에서 pcv_validate_vm_create_params()로 수행.
 */
void cmd_vm_create(int argc, char *argv[]) {
    /* [#1] --help/-h 는 도움말이지 VM 이름이 아니다. 또한 '-'로 시작하는
     * positional 은 플래그 오타이므로 거부한다 (pcv_validate_vm_name 은
     * charset 상 '--help' 를 유효 이름으로 통과시킨다 — web-prod 같은 정상
     * 이름 때문에 '-' 를 허용하므로, 플래그/이름 구분은 CLI 책임). */
    gboolean want_help = (argc < 3)
        || g_strcmp0(argv[2], "--help") == 0
        || g_strcmp0(argv[2], "-h") == 0;
    if (want_help) {
        printf("%sUsage: pcvctl vm create <name>"
               " [--vcpu <n>] [--memory_mb <mb>] [--disk_size_gb <gb>]"
               " [--iso_path <path>] [--network_bridge <br>|none]"
               " [--storage_type zvol|qcow2|raw]"
               " [--storage_pool <dataset>] [--image_dir <path>]%s\n"
               "%s  network_bridge 미지정 시 관리형 기본 NAT 네트워크(pcvnat0)에 부착,"
               " 'none'은 NIC 미부착%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET),
            cc(CYBER_DIM), cc(CYBER_RESET));
        return;
    }
    if (argv[2][0] == '-') {
        g_printerr("%s[!] '%s' 는 VM 이름이 아닙니다 (플래그 형태). "
                   "이름을 먼저 지정하세요: pcvctl vm create <name> ...%s\n",
                   ce(CYBER_RED), argv[2], ce(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if      (g_strcmp0(argv[i],"--vcpu")           == 0 && i+1 < argc)
            json_object_set_int_member   (params,"vcpu",           atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--memory_mb")      == 0 && i+1 < argc)
            json_object_set_int_member   (params,"memory_mb",      atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--disk_size_gb")   == 0 && i+1 < argc)
            json_object_set_int_member   (params,"disk_size_gb",   atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--iso_path")       == 0 && i+1 < argc)
            json_object_set_string_member(params,"iso_path",        argv[++i]);
        else if (g_strcmp0(argv[i],"--network_bridge") == 0 && i+1 < argc)
            json_object_set_string_member(params,"network_bridge",  argv[++i]);
        else if (g_strcmp0(argv[i],"--storage_type")   == 0 && i+1 < argc)
            json_object_set_string_member(params,"storage_type",    argv[++i]);
        else if (g_strcmp0(argv[i],"--storage_pool")   == 0 && i+1 < argc)
            json_object_set_string_member(params,"storage_pool",    argv[++i]);
        else if (g_strcmp0(argv[i],"--image_dir")      == 0 && i+1 < argc)
            json_object_set_string_member(params,"image_dir",       argv[++i]);
    }
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.create", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "VM_CREATE");
    g_free(resp);
}

/**
 * cmd_vm_list - VM 목록 조회 핸들러
 *
 * JSON-RPC "vm.list" → UUID/NAME/STATE 테이블 출력.
 * FMT_TABLE: CYBER 네온 컬러 테이블
 * FMT_JSON:  raw JSON 그대로 출력
 * FMT_PLAIN/CSV: PcvTable 기반 구조화 출력
 */
void cmd_vm_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;

    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) ||
        !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root_obj   = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *result_arr = json_object_has_member(root_obj, "result")
            ? json_object_get_array_member(root_obj, "result") : NULL;

    /* PLAIN / CSV 경로 */
    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdrs[] = {"UUID","NAME","STATE"};
        PcvTable   *t      = ptbl_new(hdrs, 3);
        if (result_arr) {
            for (guint i = 0; i < json_array_get_length(result_arr); i++) {
                JsonObject  *vm    = json_array_get_object_element(result_arr, i);
                const gchar *uuid  = json_object_has_member(vm,"uuid")
                        ? json_object_get_string_member(vm,"uuid")  : "-";
                const gchar *name  = json_object_has_member(vm,"name")
                        ? json_object_get_string_member(vm,"name")  : "-";
                const gchar *state = json_object_has_member(vm,"state")
                        ? json_object_get_string_member(vm,"state") : "unknown";
                ptbl_row(t, uuid, name, state, NULL);
            }
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        g_object_unref(parser); g_free(resp); return;
    }

    /* FMT_TABLE: 기존 CYBER 출력 */
    print_cyber_banner();
    printf("%s%s %-38s │ %-18s │ %-10s%s\n",
        cc(CYBER_CYAN), cc(CYBER_BOLD),
        "SYS_UUID", "ENTITY_ID", "LIFELINE",
        cc(CYBER_RESET));
    printf("%s%s%s\n", cc(CYBER_CYAN),
        "────────────────────────────────────────"
        "┼────────────────────┼────────────",
        cc(CYBER_RESET));

    if (!result_arr || json_array_get_length(result_arr) == 0) {
        printf("%s [ NO ACTIVE ENTITIES FOUND IN MAINFRAME ]\n%s",
            cc(CYBER_DIM), cc(CYBER_RESET));
    } else {
        for (guint i = 0; i < json_array_get_length(result_arr); i++) {
            JsonObject  *vm    = json_array_get_object_element(result_arr, i);
            const gchar *uuid  = json_object_has_member(vm,"uuid")
                    ? json_object_get_string_member(vm,"uuid")  : "-";
            const gchar *name  = json_object_has_member(vm,"name")
                    ? json_object_get_string_member(vm,"name")  : "-";
            const gchar *state = json_object_has_member(vm,"state")
                    ? json_object_get_string_member(vm,"state") : "unknown";
            const gchar *sc =
                g_strcmp0(state,"running") == 0 ? cc(CYBER_GREEN) :
                g_strcmp0(state,"shutoff") == 0 ? cc(CYBER_RED)   :
                g_strcmp0(state,"paused")  == 0 ? cc(CYBER_YELLOW): cc(CYBER_DIM);
            printf("%s %-38s%s │ %s%-18s%s │ %s%-10s%s\n",
                cc(CYBER_DIM),  uuid,  cc(CYBER_RESET),
                cc(CYBER_YELLOW), name, cc(CYBER_RESET),
                sc, state, cc(CYBER_RESET));
        }
    }
    printf("%s%s%s\n\n", cc(CYBER_CYAN),
        "────────────────────────────────────────"
        "┴────────────────────┴────────────",
        cc(CYBER_RESET));
    g_object_unref(parser);
    g_free(resp);
}

/**
 * cmd_vm_action - VM 단일 액션 공통 핸들러 (start/stop/pause/resume)
 * @method: RPC 메서드명 (예: "vm.start", "vm.stop")
 * @label:  출력 레이블 (예: "START", "STOP")
 *
 * argv[2]를 vm_id로 사용. 동일 패턴의 4개 명령을 공통화.
 */
static void cmd_vm_action(int argc, char *argv[],
                          const gchar *method, const gchar *label) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm %s <vm_id>%s\n",
            cc(CYBER_YELLOW), argv[2], cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request(method, params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, label);
    g_free(resp);
}

void cmd_vm_start  (int argc, char *argv[]) { cmd_vm_action(argc,argv,"vm.start", "START" ); }
void cmd_vm_stop   (int argc, char *argv[]) { cmd_vm_action(argc,argv,"vm.stop",  "STOP"  ); }
void cmd_vm_pause  (int argc, char *argv[]) { cmd_vm_action(argc,argv,"vm.pause", "PAUSE" ); }
void cmd_vm_resume (int argc, char *argv[]) { cmd_vm_action(argc,argv,"vm.resume","RESUME"); }

/**
 * cmd_vm_delete - VM 삭제 명령 핸들러 (파괴적 작업, 확인 프롬프트 포함)
 *
 * tty 환경에서는 사용자에게 VM 이름 재입력을 요구하여 오삭제를 방지.
 * --batch 모드 또는 비-tty 환경에서는 확인 없이 즉시 실행.
 * VM XML, ZFS zvol, 모든 스냅샷이 영구 삭제된다.
 */
void cmd_vm_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm delete <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const char *target = argv[2];

    /* batch / non-tty: 확인 프롬프트 생략 */
    if (!g_ctx.batch && isatty(STDIN_FILENO)) {
        print_cyber_banner();
        printf("%s [!] WARNING: DESTRUCTIVE OPERATION INITIATED [!]%s\n",
            cc(CYBER_RED), cc(CYBER_RESET));
        printf("%s VM to delete: %s%s%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RED), target, cc(CYBER_RESET));
        printf(" This will permanently destroy:\n");
        printf("   1. VM XML configuration\n");
        printf("   2. ZFS zvol and all data\n");
        printf("   3. All ZFS snapshots\n");
        printf("%s This action CANNOT be undone.\n\n%s", cc(CYBER_RED), cc(CYBER_RESET));
        printf(" Confirm by typing the exact VM ID ('%s'): ", target);

        char buf[256];
        if (!fgets(buf, sizeof(buf), stdin)) return;
        buf[strcspn(buf, "\n")] = '\0';
        if (strcmp(buf, target) != 0) {
            printf("%s\n [!] ABORTED: Name mismatch. Deletion cancelled.\n%s",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        printf("%s\n [!] AUTHORIZATION ACCEPTED. COMMENCING ANNIHILATION...\n%s",
            cc(CYBER_CYAN), cc(CYBER_RESET));
    }
    cmd_vm_action(argc, argv, "vm.delete", "VM_DELETE");
}

/**
 * cmd_vm_rename - 정지된 VM 이름 변경
 *
 * Usage: pcvctl vm rename <old_name> <new_name>
 * RPC: vm.rename {name, new_name}
 */
void cmd_vm_rename(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm rename <old_name> <new_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "new_name", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.rename", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "VM_RENAME");
    g_free(resp);
}

void cmd_vm_limit(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl vm limit <vm_id>"
               " [--cpu <percent>] [--mem <mb>]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    for (int i = 3; i < argc; i++) {
        if      (g_strcmp0(argv[i],"--cpu") == 0 && i+1 < argc)
            json_object_set_int_member(params,"cpu",atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--mem") == 0 && i+1 < argc)
            json_object_set_int_member(params,"mem",atoi(argv[++i]));
    }
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.limit", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "RESOURCE_LIMIT");
    g_free(resp);
}

void cmd_vm_set_memory(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm set-memory <vm_id> <mb>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",    argv[2]);
    json_object_set_int_member   (params,"memory_mb",atoi(argv[3]));
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.set_memory", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "SET_MEMORY");
    g_free(resp);
}

void cmd_vm_set_vcpu(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm set-vcpu <vm_id> <count>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",     argv[2]);
    json_object_set_int_member   (params,"vcpu_count",atoi(argv[3]));
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.set_vcpu", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "SET_VCPU");
    g_free(resp);
}

void cmd_vm_vnc(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm vnc <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.vnc", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root,"result")) {
        JsonObject  *res  = json_object_get_object_member(root,"result");
        const gchar *port = json_object_has_member(res,"vnc_port")
                ? json_object_get_string_member(res,"vnc_port") : "N/A";

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("vnc_port\t%s\n", port);
        } else {
            print_cyber_banner();
            printf("%s [ OPTIC NERVE CONNECTED ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s VNC DISPLAY PORT : %s%s\n", cc(CYBER_GREEN), port, cc(CYBER_RESET));
            printf("%s BIND ADDRESS     : 0.0.0.0\n\n%s", cc(CYBER_DIM), cc(CYBER_RESET));
            printf("%s💡 HOW TO CONNECT:%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
            printf("   Open VNC viewer → connect to [Host_IP]:%s\n", port);
            printf("%s────────────────────────────────────────%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
        }
    } else {
        print_action_response(resp, "VNC_QUERY");
    }
    g_object_unref(parser);
    g_free(resp);
}

void cmd_vm_eject(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm eject <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.eject", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }
    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        printf("eject\t%s\tOK\n", argv[3]);
        g_free(resp); return;
    }
    print_cyber_banner();
    printf("%s [ OPTICAL DRIVE PURGED ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
    printf("%s TARGET VM : %s%s\n", cc(CYBER_GREEN), argv[3], cc(CYBER_RESET));
    printf("%s ISO media ejected from virtual cdrom.\n\n%s", cc(CYBER_DIM), cc(CYBER_RESET));
    printf("%s────────────────────────────────────────%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  스냅샷 명령
 * ════════════════════════════════════════════════════════════════════ */

void cmd_snapshot_create(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl snapshot create <vm_id> <snap_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p,"vm_id",    argv[2]);
    json_object_set_string_member(p,"snap_name",argv[3]);
    GError *e = NULL;
    gchar  *r = purectl_send_request("vm.snapshot.create", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "SNAPSHOT_CREATE");
    g_free(r);
}

void cmd_snapshot_rollback(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl snapshot rollback <vm_id> <snap_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    if (g_ctx.fmt == FMT_TABLE)
        printf("%s[!] WARNING: Running VMs will be auto-stopped before rollback.%s\n",
            cc(CYBER_RED), cc(CYBER_RESET));
    JsonObject *p = json_object_new();
    json_object_set_string_member(p,"vm_id",    argv[2]);
    json_object_set_string_member(p,"snap_name",argv[3]);
    GError *e = NULL;
    gchar  *r = purectl_send_request("vm.snapshot.rollback", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "SNAPSHOT_ROLLBACK");
    g_free(r);
}

void cmd_snapshot_list(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl snapshot list <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p,"vm_id",argv[2]);
    GError *e = NULL;
    gchar  *r = purectl_send_request("vm.snapshot.list", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser,r,-1,NULL) && json_parser_get_root(parser)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root,"result")) {
            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                printf("%s\n",
                    json_node_get_string(json_object_get_member(root,"result")));
            } else {
                print_cyber_banner();
                printf("%s [ ZFS TIMELINES: %s ]\n\n%s", cc(CYBER_CYAN), argv[3], cc(CYBER_RESET));
                printf("%s%s%s\n", cc(CYBER_GREEN),
                    json_node_get_string(json_object_get_member(root,"result")),
                    cc(CYBER_RESET));
                printf("%s────────────────────────────────────%s\n",
                    cc(CYBER_CYAN), cc(CYBER_RESET));
            }
        } else {
            print_action_response(r, "SNAPSHOT_LIST");
        }
    }
    g_object_unref(parser);
    g_free(r);
}

void cmd_snapshot_delete(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl snapshot delete <vm_id> <snap_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p,"vm_id",    argv[2]);
    json_object_set_string_member(p,"snap_name",argv[3]);
    GError *e = NULL;
    gchar  *r = purectl_send_request("vm.snapshot.delete", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "SNAPSHOT_DELETE");
    g_free(r);
}

/** cmd_snapshot_verify - ZFS 스냅샷 존재/무결성 검증. backup.snapshot.verify RPC. */
void cmd_snapshot_verify(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl snapshot verify <snap_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "snapshot", argv[2]);
    GError *e = NULL;
    gchar  *r = purectl_send_request("backup.snapshot.verify", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) { g_printerr("%s[!] NULL RESPONSE%s\n", ce(CYBER_RED), ce(CYBER_RESET)); return; }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, r, -1, NULL) && json_parser_get_root(parser)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *err_obj = json_object_get_object_member(root, "error");
            g_printerr("%s[!] COMMAND REJECTED [%lld]: %s%s\n",
                ce(CYBER_RED),
                (long long)json_object_get_int_member(err_obj, "code"),
                json_object_get_string_member(err_obj, "message"),
                ce(CYBER_RESET));
        } else if (json_object_has_member(root, "result")) {
            JsonObject  *res       = json_object_get_object_member(root, "result");
            gboolean     exists    = json_object_get_boolean_member(res, "exists");
            const gchar *integrity = json_object_has_member(res, "integrity")
                    ? json_object_get_string_member(res, "integrity") : "unknown";
            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                printf("%s\t%s\t%s\n", argv[2], exists ? "true" : "false", integrity);
            } else {
                printf("%s%s[%s] SNAPSHOT %s: exists=%s integrity=%s%s\n",
                    cc(exists ? CYBER_GREEN : CYBER_RED), cc(CYBER_BOLD),
                    exists ? "+" : "!", argv[2],
                    exists ? "true" : "false", integrity,
                    cc(CYBER_RESET));
            }
        }
    }
    g_object_unref(parser);
    g_free(r);
}

/* ════════════════════════════════════════════════════════════════════
 *  모니터링 명령
 * ════════════════════════════════════════════════════════════════════ */

void cmd_monitor_metrics(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl monitor metrics <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.metrics", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root,"result")) {
        JsonObject *res     = json_object_get_object_member(root,"result");
        int         cpu_pct = (int)json_object_get_int_member(res,"cpu");
        int         mem_pct = (int)json_object_get_int_member(res,"mem");

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("vm\tcpu\tmem\n%s\t%d\t%d\n", argv[3], cpu_pct, mem_pct);
        } else {
            print_cyber_banner();
            printf("%s%s>>> REALTIME TELEMETRY: %s <<<\n\n%s",
                cc(CYBER_YELLOW), cc(CYBER_BOLD), argv[3], cc(CYBER_RESET));
            print_metrics_bar("CPU",    cpu_pct, cc(CYBER_GREEN));
            print_metrics_bar("MEMORY", mem_pct, cc(CYBER_RED));
            printf("\n");
        }
    }
    g_object_unref(parser);
    g_free(resp);
}

void cmd_monitor_fleet(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("monitor.fleet", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));

    if (!json_object_has_member(root,"result")) {
        print_action_response(resp, "MONITOR_FLEET");
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *res  = json_object_get_object_member(root,"result");
    JsonNode   *node = json_object_get_member(res,"fleet");

    if (node && JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *arr = json_node_get_array(node);

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"NAME","STATE","CPU","MEM"};
            PcvTable   *t      = ptbl_new(hdrs, 4);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject  *vm = json_array_get_object_element(arr, i);
                char cpu_s[16], mem_s[16];
                snprintf(cpu_s, sizeof(cpu_s), "%d",
                    (int)json_object_get_int_member(vm,"cpu"));
                snprintf(mem_s, sizeof(mem_s), "%d",
                    (int)json_object_get_int_member(vm,"mem"));
                ptbl_row(t,
                    json_object_get_string_member(vm,"name"),
                    json_object_get_string_member(vm,"state"),
                    cpu_s, mem_s, NULL);
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else {
            print_cyber_banner();
            printf("%s%s [ GLOBAL FLEET STATUS ]\n\n%s",
                cc(CYBER_CYAN), cc(CYBER_BOLD), cc(CYBER_RESET));
            printf("%s%s %-20s │ %-10s │ %6s │ %6s%s\n",
                cc(CYBER_CYAN), cc(CYBER_BOLD),
                "ENTITY_ID","LIFELINE","CPU%","MEM%", cc(CYBER_RESET));
            printf("%s──────────────────────┼────────────┼────────┼────────%s\n",
                cc(CYBER_CYAN), cc(CYBER_RESET));
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject  *vm    = json_array_get_object_element(arr, i);
                const gchar *name  = json_object_get_string_member(vm,"name");
                const gchar *state = json_object_get_string_member(vm,"state");
                int          cpu   = (int)json_object_get_int_member(vm,"cpu");
                int          mem   = (int)json_object_get_int_member(vm,"mem");
                const gchar *sc    = g_strcmp0(state,"running")==0
                        ? cc(CYBER_GREEN) : cc(CYBER_RED);
                printf(" %-20s │ %s%-10s%s │ %5d%% │ %5d%%\n",
                    name, sc, state, cc(CYBER_RESET), cpu, mem);
            }
            printf("%s──────────────────────┴────────────┴────────┴────────%s\n\n",
                cc(CYBER_CYAN), cc(CYBER_RESET));
        }
    } else {
        /* 구조 미확정: raw 출력 */
        gchar *raw = json_to_string(json_object_get_member(root,"result"), FALSE);
        printf("%s\n", raw);
        g_free(raw);
    }
    g_object_unref(parser);
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  네트워크 명령
 * ════════════════════════════════════════════════════════════════════ */

void cmd_net_create(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl network create <bridge_name>"
               " [--mode nat|isolated|routed] [--cidr IP/PREFIX] [--iface eth0]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"bridge_name",argv[2]);
    for (int i = 3; i < argc; i++) {
        if      (g_strcmp0(argv[i],"--mode")  == 0 && i+1 < argc)
            json_object_set_string_member(params,"mode",       argv[++i]);
        else if (g_strcmp0(argv[i],"--cidr")  == 0 && i+1 < argc)
            json_object_set_string_member(params,"cidr",       argv[++i]);
        else if (g_strcmp0(argv[i],"--iface") == 0 && i+1 < argc)
            json_object_set_string_member(params,"physical_if",argv[++i]);
    }
    GError *err = NULL;
    gchar  *res = purectl_send_request("network.create", params, &err);
    if (err) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), err->message, ce(CYBER_RESET));
        g_error_free(err); return;
    }
    print_action_response(res, "NET_CREATE");
    g_free(res);
}

void cmd_net_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl network delete <bridge_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"bridge_name",argv[2]);
    GError *err = NULL;
    gchar  *res = purectl_send_request("network.delete", params, &err);
    if (err) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), err->message, ce(CYBER_RESET));
        g_error_free(err); return;
    }
    print_action_response(res, "NET_DELETE");
    g_free(res);
}

void cmd_net_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *err = NULL;
    gchar  *res = purectl_send_request("network.list", NULL, &err);
    if (err) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), err->message, ce(CYBER_RESET));
        g_error_free(err); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(res); g_free(res); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,res,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(res); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root,"result")
            ? json_object_get_array_member(root,"result") : NULL;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdrs[] = {"BRIDGE","MODE","CIDR","STATE"};
        PcvTable   *t      = ptbl_new(hdrs, 4);
        if (arr) {
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *n = json_array_get_object_element(arr, i);
                ptbl_row(t,
                    /* VP-5: 데몬 직렬화 키는 name/ip_cidr (bridge_name/cidr 아님) */
                    json_object_has_member(n,"name")
                        ? json_object_get_string_member(n,"name")        : "-",
                    json_object_has_member(n,"mode")
                        ? json_object_get_string_member(n,"mode")        : "-",
                    json_object_has_member(n,"ip_cidr")
                        ? json_object_get_string_member(n,"ip_cidr")     : "-",
                    json_object_has_member(n,"state")
                        ? json_object_get_string_member(n,"state")       : "up",
                    NULL);
            }
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        g_object_unref(parser); g_free(res); return;
    }

    print_cyber_banner();
    printf("%s%s %-20s │ %-12s │ %-18s │ %-10s%s\n",
        cc(CYBER_CYAN), cc(CYBER_BOLD),
        "BRIDGE","MODE","CIDR","STATE", cc(CYBER_RESET));
    printf("%s──────────────────────┼──────────────┼────────────────────┼────────────%s\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    if (!arr || json_array_get_length(arr) == 0) {
        printf("%s [ NO NETWORKS FOUND ]%s\n", cc(CYBER_DIM), cc(CYBER_RESET));
    } else {
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject  *n     = json_array_get_object_element(arr, i);
            /* VP-5: 데몬 직렬화 키는 name/ip_cidr (bridge_name/cidr 아님) */
            const gchar *br    = json_object_has_member(n,"name")
                    ? json_object_get_string_member(n,"name")        : "-";
            const gchar *mode  = json_object_has_member(n,"mode")
                    ? json_object_get_string_member(n,"mode")        : "-";
            const gchar *cidr  = json_object_has_member(n,"ip_cidr")
                    ? json_object_get_string_member(n,"ip_cidr")     : "-";
            const gchar *state = json_object_has_member(n,"state")
                    ? json_object_get_string_member(n,"state")       : "up";
            printf(" %-20s │ %s%-12s%s │ %-18s │ %s%-10s%s\n",
                br,
                cc(CYBER_YELLOW), mode, cc(CYBER_RESET),
                cidr,
                cc(CYBER_GREEN), state, cc(CYBER_RESET));
        }
    }
    printf("%s──────────────────────┴──────────────┴────────────────────┴────────────%s\n\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    g_object_unref(parser);
    g_free(res);
}

void cmd_net_mode(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl network mode <bridge_name>"
               " <nat|isolated|routed> <CIDR>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"bridge_name",argv[2]);
    json_object_set_string_member(params,"mode",       argv[3]);
    json_object_set_string_member(params,"cidr",       argv[4]);
    GError *err = NULL;
    gchar  *res = purectl_send_request("network.mode_set", params, &err);
    if (err) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), err->message, ce(CYBER_RESET));
        g_error_free(err); return;
    }
    print_action_response(res, "NET_MODE_SET");
    g_free(res);
}

/* ════════════════════════════════════════════════════════════════════
 *  스토리지 명령
 * ════════════════════════════════════════════════════════════════════ */

void cmd_storage_pool(int argc, char *argv[]) {
    if (argc < 4 || g_strcmp0(argv[2],"list") != 0) {
        printf("%sUsage: pcvctl storage pool list%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("storage.pool.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root,"result")
            ? json_object_get_array_member(root,"result") : NULL;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdrs[] = {"POOL","TOTAL","ALLOC","FREE","HEALTH"};
        PcvTable   *t      = ptbl_new(hdrs, 5);
        if (arr) {
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *row = json_array_get_object_element(arr, i);
                ptbl_row(t,
                    json_object_get_string_member(row,"name"),
                    json_object_get_string_member(row,"size"),
                    json_object_get_string_member(row,"alloc"),
                    json_object_get_string_member(row,"free"),
                    json_object_get_string_member(row,"health"),
                    NULL);
            }
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        g_object_unref(parser); g_free(resp); return;
    }

    print_cyber_banner();
    printf("%s%s %-15s │ %-10s │ %-10s │ %-10s │ %-10s%s\n",
        cc(CYBER_CYAN), cc(CYBER_BOLD),
        "POOL_NAME","TOTAL","ALLOC","FREE","HEALTH", cc(CYBER_RESET));
    printf("%s─────────────────┼────────────┼────────────┼────────────┼────────────%s\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    if (!arr || json_array_get_length(arr) == 0) {
        printf("%s [ NO ZFS POOLS DETECTED ]%s\n", cc(CYBER_DIM), cc(CYBER_RESET));
    } else {
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject  *row    = json_array_get_object_element(arr, i);
            const gchar *health = json_object_get_string_member(row,"health");
            const gchar *hc     = g_strcmp0(health,"ONLINE") == 0
                    ? cc(CYBER_GREEN) : cc(CYBER_RED);
            printf("%s %-15s%s │ %-10s │ %s%-10s%s │ %-10s │ %s%-10s%s\n",
                cc(CYBER_DIM), json_object_get_string_member(row,"name"), cc(CYBER_RESET),
                json_object_get_string_member(row,"size"),
                cc(CYBER_YELLOW), json_object_get_string_member(row,"alloc"), cc(CYBER_RESET),
                json_object_get_string_member(row,"free"),
                hc, health, cc(CYBER_RESET));
        }
    }
    printf("%s─────────────────┴────────────┴────────────┴────────────┴────────────%s\n\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    g_object_unref(parser);
    g_free(resp);
}

void cmd_storage_zvol(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl storage zvol list\n"
               "  pcvctl storage zvol create <pool/path> --size <size>\n"
               "  pcvctl storage zvol delete <pool/path>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action,"list") == 0) {
        GError *err = NULL;
        gchar  *res = purectl_send_request("storage.zvol.list", NULL, &err);
        if (err) {
            g_printerr("%s[!] LINK_SEVERED: %s%s\n",
                ce(CYBER_RED), err->message, ce(CYBER_RESET));
            g_error_free(err); return;
        }
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(res); g_free(res); return; }

        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser,res,-1,NULL)||!json_parser_get_root(parser)) {
            g_object_unref(parser); g_free(res); return;
        }
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonArray  *arr  = json_object_has_member(root,"result")
                ? json_object_get_array_member(root,"result") : NULL;

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"ZVOL_PATH","VOL_SIZE","USED"};
            PcvTable   *t      = ptbl_new(hdrs, 3);
            if (arr) {
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *row = json_array_get_object_element(arr, i);
                    ptbl_row(t,
                        json_object_get_string_member(row,"name"),
                        json_object_get_string_member(row,"volsize"),
                        json_object_get_string_member(row,"used"),
                        NULL);
                }
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
            g_object_unref(parser); g_free(res); return;
        }

        print_cyber_banner();
        printf("%s%s %-40s │ %-10s │ %-10s%s\n",
            cc(CYBER_CYAN), cc(CYBER_BOLD), "ZVOL_PATH","VOL_SIZE","USED", cc(CYBER_RESET));
        printf("%s──────────────────────────────────────────┼────────────┼────────────%s\n",
            cc(CYBER_CYAN), cc(CYBER_RESET));
        if (!arr || json_array_get_length(arr) == 0) {
            printf("%s [ NO ZVOL DEVICES ]%s\n", cc(CYBER_DIM), cc(CYBER_RESET));
        } else {
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *row = json_array_get_object_element(arr, i);
                printf("%s %-40s%s │ %s%-10s%s │ %s%-10s%s\n",
                    cc(CYBER_DIM), json_object_get_string_member(row,"name"), cc(CYBER_RESET),
                    cc(CYBER_GREEN), json_object_get_string_member(row,"volsize"), cc(CYBER_RESET),
                    cc(CYBER_YELLOW), json_object_get_string_member(row,"used"), cc(CYBER_RESET));
            }
        }
        printf("%s──────────────────────────────────────────┴────────────┴────────────%s\n\n",
            cc(CYBER_CYAN), cc(CYBER_RESET));
        g_object_unref(parser);
        g_free(res);

    } else if (g_strcmp0(action,"create") == 0) {
        if (argc < 6 || g_strcmp0(argv[4],"--size") != 0) {
            printf("%sUsage: pcvctl storage zvol create <pool/path> --size <size>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p,"zvol_path",argv[3]);
        json_object_set_string_member(p,"size",     argv[6]);
        GError *err = NULL;
        gchar  *res = purectl_send_request("storage.zvol.create", p, &err);
        if (err) {
            g_printerr("%s[!] LINK_SEVERED: %s%s\n",
                ce(CYBER_RED), err->message, ce(CYBER_RESET));
            g_error_free(err); return;
        }
        print_action_response(res, "ZVOL_CREATE");
        g_free(res);

    } else if (g_strcmp0(action,"delete") == 0) {
        if (argc < 4) {
            printf("%sUsage: pcvctl storage zvol delete <pool/path>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p,"zvol_path",argv[3]);
        GError *err = NULL;
        gchar  *res = purectl_send_request("storage.zvol.delete", p, &err);
        if (err) {
            g_printerr("%s[!] LINK_SEVERED: %s%s\n",
                ce(CYBER_RED), err->message, ce(CYBER_RESET));
            g_error_free(err); return;
        }
        print_action_response(res, "ZVOL_DELETE");
        g_free(res);
    } else {
        printf("%s[!] UNKNOWN ZVOL ACTION: %s%s\n",
            cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  디바이스 명령
 * ════════════════════════════════════════════════════════════════════ */

void cmd_device_disk(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage:\n"
               "  pcvctl device disk attach <vm_id>"
               " --source <zvol_path> --target <vdb> [--bus virtio]\n"
               "  pcvctl device disk detach <vm_id> --target <vdb>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];
    JsonObject  *params = json_object_new();
    json_object_set_string_member(params,"vm_id",argv[3]);
    for (int i = 5; i < argc; i++) {
        if      (g_strcmp0(argv[i],"--source") == 0 && i+1 < argc)
            json_object_set_string_member(params,"source",argv[++i]);
        else if (g_strcmp0(argv[i],"--target") == 0 && i+1 < argc)
            json_object_set_string_member(params,"target",argv[++i]);
        else if (g_strcmp0(argv[i],"--bus")    == 0 && i+1 < argc)
            json_object_set_string_member(params,"bus",   argv[++i]);
    }
    const gchar *method = NULL, *label = NULL;
    if      (g_strcmp0(action,"attach") == 0) { method="device.disk.attach"; label="DISK_ATTACH"; }
    else if (g_strcmp0(action,"detach") == 0) { method="device.disk.detach"; label="DISK_DETACH"; }
    else {
        printf("%s[!] UNKNOWN DISK ACTION: %s%s\n",cc(CYBER_RED),action,cc(CYBER_RESET));
        json_object_unref(params); return;
    }
    GError *err = NULL;
    gchar  *res = purectl_send_request(method, params, &err);
    if (err) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), err->message, ce(CYBER_RESET));
        g_error_free(err); return;
    }
    print_action_response(res, label);
    g_free(res);
}

/* ════════════════════════════════════════════════════════════════════
 *  컨테이너 명령 (12개)
 * ════════════════════════════════════════════════════════════════════ */

void cmd_container_create(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl container create <name>"
               " [--image ubuntu:22.04] [--memory_mb 512]"
               " [--vcpu_count 1] [--network_bridge virbr0]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"name",argv[2]);
    for (int i = 3; i < argc; i++) {
        if      (g_strcmp0(argv[i],"--image")          == 0 && i+1 < argc)
            json_object_set_string_member(params,"image",         argv[++i]);
        else if (g_strcmp0(argv[i],"--memory_mb")      == 0 && i+1 < argc)
            json_object_set_int_member   (params,"memory_mb",     atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--vcpu_count")     == 0 && i+1 < argc)
            json_object_set_int_member   (params,"vcpu_count",    atoi(argv[++i]));
        else if (g_strcmp0(argv[i],"--network_bridge") == 0 && i+1 < argc)
            json_object_set_string_member(params,"network_bridge",argv[++i]);
    }
    GError *e = NULL;
    gchar  *r = purectl_send_request("container.create", params, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_CREATE");
    g_free(r);
}

void cmd_container_destroy(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container destroy <name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new(); json_object_set_string_member(p,"name",argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.destroy",p,&e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r,"CONTAINER_DESTROY"); g_free(r);
}

void cmd_container_start(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container start <name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new(); json_object_set_string_member(p,"name",argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.start",p,&e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r,"CONTAINER_START"); g_free(r);
}

void cmd_container_stop(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container stop <name> [--force]%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new(); json_object_set_string_member(p,"name",argv[2]);
    for (int i = 3; i < argc; i++)
        if (g_strcmp0(argv[i],"--force")==0) json_object_set_boolean_member(p,"force",TRUE);
    GError *e = NULL; gchar *r = purectl_send_request("container.stop",p,&e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r,"CONTAINER_STOP"); g_free(r);
}

void cmd_container_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("container.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root,"result")
            ? json_object_get_array_member(root,"result") : NULL;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdrs[] = {"NAME","STATE","IP","IMAGE"};
        PcvTable   *t      = ptbl_new(hdrs, 4);
        if (arr) {
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *obj = json_array_get_object_element(arr, i);
                ptbl_row(t,
                    json_object_get_string_member(obj,"name"),
                    json_object_get_string_member(obj,"state"),
                    json_object_has_member(obj,"ip_addr")
                        ? json_object_get_string_member(obj,"ip_addr") : "-",
                    json_object_has_member(obj,"image")
                        ? json_object_get_string_member(obj,"image")   : "-",
                    NULL);
            }
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        g_object_unref(parser); g_free(resp); return;
    }

    print_cyber_banner();
    printf("%s%s %-30s │ %-12s │ %-18s │ %-20s%s\n",
        cc(CYBER_CYAN), cc(CYBER_BOLD),
        "CONTAINER_ID","LIFELINE","IP_ADDR","IMAGE", cc(CYBER_RESET));
    printf("%s────────────────────────────────┼──────────────┼────────────────────┼────────────────────%s\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    if (arr) {
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject  *obj   = json_array_get_object_element(arr, i);
            const gchar *name  = json_object_get_string_member(obj,"name");
            const gchar *state = json_object_get_string_member(obj,"state");
            const gchar *ip    = json_object_has_member(obj,"ip_addr")
                    ? json_object_get_string_member(obj,"ip_addr") : "-";
            const gchar *image = json_object_has_member(obj,"image")
                    ? json_object_get_string_member(obj,"image")   : "-";
            const gchar *color = g_strcmp0(state,"RUNNING")==0
                    ? cc(CYBER_GREEN) : cc(CYBER_RED);
            printf(" %-30s │ %s%-12s%s │ %-18s │ %-20s\n",
                name, color, state, cc(CYBER_RESET), ip, image);
        }
    }
    printf("%s────────────────────────────────────────────────────────────────────────────────%s\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
    g_object_unref(parser);
    g_free(resp);
}

void cmd_container_metrics(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container metrics <name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new(); json_object_set_string_member(p,"name",argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.metrics",p,&e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,r,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(r); return;
    }
    JsonObject *root   = json_node_get_object(json_parser_get_root(parser));
    JsonObject *result = json_object_has_member(root,"result")
            ? json_object_get_object_member(root,"result") : NULL;

    if (!result) { print_action_response(r,"CONTAINER_METRICS"); g_object_unref(parser); g_free(r); return; }

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        char cpu_s[32], memu_s[32], meml_s[32], rx_s[32], tx_s[32], pid_s[32];
        snprintf(cpu_s,  sizeof(cpu_s),  "%.1f", json_object_get_double_member(result,"cpu_percent"));
        snprintf(memu_s, sizeof(memu_s), "%.1f", json_object_get_double_member(result,"mem_used_mb"));
        snprintf(meml_s, sizeof(meml_s), "%.1f", json_object_get_double_member(result,"mem_limit_mb"));
        snprintf(rx_s,   sizeof(rx_s),   "%.2f", json_object_get_double_member(result,"net_rx_mb"));
        snprintf(tx_s,   sizeof(tx_s),   "%.2f", json_object_get_double_member(result,"net_tx_mb"));
        snprintf(pid_s,  sizeof(pid_s),  "%lld", (long long)json_object_get_int_member(result,"init_pid"));
        const char *hdrs[] = {"NAME","STATE","CPU%","MEM_USED","MEM_LIMIT","NET_RX","NET_TX","PID"};
        PcvTable   *t      = ptbl_new(hdrs, 8);
        ptbl_row(t,
            json_object_get_string_member(result,"name"),
            json_object_get_string_member(result,"state"),
            cpu_s, memu_s, meml_s, rx_s, tx_s, pid_s, NULL);
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else {
        print_cyber_banner();
        printf("%s Container Metrics: %s%s\n", cc(CYBER_CYAN),
            json_object_get_string_member(result,"name"), cc(CYBER_RESET));
        printf("  State     : %s\n",    json_object_get_string_member(result,"state"));
        printf("  CPU %%     : %.1f%%\n", json_object_get_double_member(result,"cpu_percent"));
        printf("  Mem Used  : %.1f MB / %.1f MB\n",
            json_object_get_double_member(result,"mem_used_mb"),
            json_object_get_double_member(result,"mem_limit_mb"));
        printf("  Net RX    : %.2f MB\n", json_object_get_double_member(result,"net_rx_mb"));
        printf("  Net TX    : %.2f MB\n", json_object_get_double_member(result,"net_tx_mb"));
        printf("  IP        : %s\n",    json_object_get_string_member(result,"ip_addr"));
        printf("  Init PID  : %lld\n",  (long long)json_object_get_int_member(result,"init_pid"));
    }
    g_object_unref(parser);
    g_free(r);
}

void cmd_container_exec(int argc, char *argv[]) {
    if (argc < 4) { printf("%sUsage: pcvctl container exec <name> <cmd>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p,"name",argv[2]);
    json_object_set_string_member(p,"cmd", argv[3]);
    GError *e = NULL; gchar *r = purectl_send_request("container.exec",p,&e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser,r,-1,NULL)&&json_parser_get_root(parser)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root,"result")) {
            JsonObject  *res    = json_object_get_object_member(root,"result");
            const gchar *output = json_object_has_member(res,"output")
                    ? json_object_get_string_member(res,"output") : "";
            printf("%s", output);
        } else {
            print_action_response(r,"CONTAINER_EXEC");
        }
    }
    g_object_unref(parser);
    g_free(r);
}

void cmd_container_snapshot(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl container snapshot create   <name> <snap_name>\n"
               "  pcvctl container snapshot list     <name>\n"
               "  pcvctl container snapshot rollback <name> <snap_name>\n"
               "  pcvctl container snapshot delete   <name> <snap_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action,"create") == 0) {
        if (argc < 6) { printf("%sNeed: <name> <snap_name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p,"name",     argv[3]);
        json_object_set_string_member(p,"snap_name",argv[4]);
        GError *e = NULL; gchar *r = purectl_send_request("container.snapshot.create",p,&e);
        if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
        print_action_response(r,"CONTAINER_SNAP_CREATE"); g_free(r);

    } else if (g_strcmp0(action,"list") == 0) {
        if (argc < 5) { printf("%sNeed: <name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
        JsonObject *p = json_object_new(); json_object_set_string_member(p,"name",argv[3]);
        GError *e = NULL; gchar *r = purectl_send_request("container.snapshot.list",p,&e);
        if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser,r,-1,NULL)&&json_parser_get_root(parser)) {
            JsonObject *root = json_node_get_object(json_parser_get_root(parser));
            JsonArray  *arr  = json_object_has_member(root,"result")
                    ? json_object_get_array_member(root,"result") : NULL;
            if (arr && g_ctx.fmt == FMT_TABLE) {
                print_cyber_banner();
                printf("%s Snapshots for: %s%s\n", cc(CYBER_CYAN), argv[3], cc(CYBER_RESET));
                for (guint i = 0; i < json_array_get_length(arr); i++)
                    printf("  [%u] %s\n", i+1, json_array_get_string_element(arr,i));
            } else if (arr) {
                for (guint i = 0; i < json_array_get_length(arr); i++)
                    printf("%s\n", json_array_get_string_element(arr,i));
            }
        }
        g_object_unref(parser); g_free(r);

    } else if (g_strcmp0(action,"rollback") == 0) {
        if (argc < 6) { printf("%sNeed: <name> <snap_name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
        if (g_ctx.fmt == FMT_TABLE)
            printf("%s[!] WARNING: Container will be stopped before rollback!%s\n",
                cc(CYBER_RED), cc(CYBER_RESET));
        JsonObject *p = json_object_new();
        json_object_set_string_member(p,"name",     argv[3]);
        json_object_set_string_member(p,"snap_name",argv[4]);
        GError *e = NULL; gchar *r = purectl_send_request("container.snapshot.rollback",p,&e);
        if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
        print_action_response(r,"CONTAINER_SNAP_ROLLBACK"); g_free(r);

    } else if (g_strcmp0(action,"delete") == 0) {
        if (argc < 6) { printf("%sNeed: <name> <snap_name>%s\n",cc(CYBER_YELLOW),cc(CYBER_RESET)); return; }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p,"name",     argv[3]);
        json_object_set_string_member(p,"snap_name",argv[4]);
        GError *e = NULL; gchar *r = purectl_send_request("container.snapshot.delete",p,&e);
        if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
        print_action_response(r,"CONTAINER_SNAP_DELETE"); g_free(r);
    } else {
        printf("%s[!] UNKNOWN SNAPSHOT ACTION: %s%s\n",cc(CYBER_RED),action,cc(CYBER_RESET));
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  클러스터 명령
 * ════════════════════════════════════════════════════════════════════ */


/* ════════════════════════════════════════════════════════════════════
 *  OVN 명령
 *
 *  OVN (Open Virtual Network) SDN 관련 CLI 명령 그룹.
 *  ovn-nbctl 기반 논리 스위치/라우터/ACL/NAT 관리.
 *
 *  모든 cmd_ovn_* 함수는 동일한 RPC 패턴을 따른다:
 *    1. purectl_send_request()로 JSON-RPC 전송
 *    2. 에러 처리 (GError → 출력 후 반환)
 *    3. FMT_JSON이면 raw 출력, 아니면 PcvTable 파싱 → 테이블 출력
 *
 *  CLI 사용 예:
 *    pcvctl ovn status
 *    pcvctl ovn switch list
 *    pcvctl ovn switch create my-ls --subnet 10.200.0.0/24
 *    pcvctl ovn router list
 *    pcvctl ovn nat list my-lr
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_ovn_status:
 * @argc: 인수 개수 (사용하지 않음)
 * @argv: 인수 배열 (사용하지 않음)
 *
 * OVN 컨트롤러 전체 상태 조회.
 * ovn.status RPC → JSON 원시 출력 (available, version, switch_count, router_count).
 */
void cmd_ovn_status(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("ovn.status", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_ovn_switch:
 * @argc: 전체 인수 개수
 * @argv: 인수 배열 (예: ["ovn", "switch", "list"])
 *
 * OVN 논리 스위치 관리 명령.
 * argv[2]로 sub-action 분기:
 *   "list"   → ovn.switch.list RPC → NAME/SUBNET/PORTS 테이블 출력
 *   "create" → ovn.switch.create RPC (argv[3]=이름, --subnet 옵션)
 *   "delete" → ovn.switch.delete RPC (argv[3]=이름, 멱등)
 *
 * 출력 포맷: g_ctx.fmt에 따라 table/plain/csv/json 자동 분기.
 * PcvTable(ptbl_*) 사용 시 FMT_CSV와 FMT_PLAIN 모두 지원.
 */
void cmd_ovn_switch(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl ovn switch list\n"
               "  pcvctl ovn switch create <name> [--subnet X]\n"
               "  pcvctl ovn switch delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "list") == 0) {
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("ovn.switch.list", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (!resp) return;
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
            g_object_unref(parser); g_free(resp); return;
        }
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonArray  *arr  = json_object_has_member(root, "result")
                ? json_object_get_array_member(root, "result") : NULL;

        if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
            const char *hdrs[] = {"NAME","SUBNET","PORTS"};
            PcvTable   *t      = ptbl_new(hdrs, 3);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                ptbl_row(t,
                    json_object_get_string_member_with_default(o, "name", "?"),
                    json_object_get_string_member_with_default(o, "subnet", "-"),
                    json_object_get_string_member_with_default(o, "ports", "0"),
                    NULL);
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else if (arr) {
            printf("%s%-30s │ %-20s │ %-10s%s\n", cc(CYBER_CYAN),
                   "SWITCH_NAME", "SUBNET", "PORTS", cc(CYBER_RESET));
            printf("────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                printf(" %-30s │ %-20s │ %-10s\n",
                    json_object_get_string_member_with_default(o, "name", "?"),
                    json_object_get_string_member_with_default(o, "subnet", "-"),
                    json_object_get_string_member_with_default(o, "ports", "0"));
            }
            printf("────────────────────────────────────────────────────────────────────\n");
        }
        g_object_unref(parser); g_free(resp);

    } else if (g_strcmp0(action, "create") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn switch create <name> [--subnet X]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        for (int i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--subnet") == 0 && i+1 < argc)
                json_object_set_string_member(params, "subnet", argv[++i]);
        }
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.switch.create", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_SWITCH_CREATE"); g_free(resp);

    } else if (g_strcmp0(action, "delete") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn switch delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.switch.delete", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_SWITCH_DELETE"); g_free(resp);

    } else {
        printf("%s[!] UNKNOWN OVN SWITCH ACTION: %s%s\n", cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/**
 * cmd_ovn_router:
 * @argc: 전체 인수 개수
 * @argv: 인수 배열 (예: ["ovn", "router", "list"])
 *
 * OVN 논리 라우터 관리 명령.
 * argv[2]로 sub-action 분기:
 *   "list"   → ovn.router.list RPC → NAME/PORTS/ROUTES 테이블 출력
 *   "create" → ovn.router.create RPC (argv[3]=이름)
 *   "delete" → ovn.router.delete RPC (argv[3]=이름, 멱등)
 */
void cmd_ovn_router(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl ovn router list\n"
               "  pcvctl ovn router create <name>\n"
               "  pcvctl ovn router delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "list") == 0) {
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("ovn.router.list", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (!resp) return;
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
            g_object_unref(parser); g_free(resp); return;
        }
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonArray  *arr  = json_object_has_member(root, "result")
                ? json_object_get_array_member(root, "result") : NULL;

        if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
            const char *hdrs[] = {"NAME","PORTS","ROUTES"};
            PcvTable   *t      = ptbl_new(hdrs, 3);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                ptbl_row(t,
                    json_object_get_string_member_with_default(o, "name", "?"),
                    json_object_get_string_member_with_default(o, "ports", "0"),
                    json_object_get_string_member_with_default(o, "routes", "0"),
                    NULL);
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else if (arr) {
            printf("%s%-30s │ %-10s │ %-10s%s\n", cc(CYBER_CYAN),
                   "ROUTER_NAME", "PORTS", "ROUTES", cc(CYBER_RESET));
            printf("────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                printf(" %-30s │ %-10s │ %-10s\n",
                    json_object_get_string_member_with_default(o, "name", "?"),
                    json_object_get_string_member_with_default(o, "ports", "0"),
                    json_object_get_string_member_with_default(o, "routes", "0"));
            }
            printf("────────────────────────────────────────────────────────────────────\n");
        }
        g_object_unref(parser); g_free(resp);

    } else if (g_strcmp0(action, "create") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn router create <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.router.create", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_ROUTER_CREATE"); g_free(resp);

    } else if (g_strcmp0(action, "delete") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn router delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.router.delete", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_ROUTER_DELETE"); g_free(resp);

    } else {
        printf("%s[!] UNKNOWN OVN ROUTER ACTION: %s%s\n", cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/**
 * cmd_ovn_nat:
 * @argc: 전체 인수 개수
 * @argv: 인수 배열 (예: ["ovn", "nat", "list", "my-lr"])
 *
 * OVN NAT 규칙 조회 명령.
 * argv[2]로 sub-action 분기:
 *   "list" → ovn.nat.list RPC (argv[3]=라우터 이름)
 *            결과는 문자열 배열 (ovn-nbctl lr-nat-list 출력 라인)
 *
 * NAT 규칙 추가/삭제는 현재 CLI 미구현 (list 조회만 제공).
 */
void cmd_ovn_nat(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl ovn nat list <router>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "list") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn nat list <router>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "router", argv[3]);
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("ovn.nat.list", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (!resp) return;
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
            g_object_unref(parser); g_free(resp); return;
        }
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonArray  *arr  = json_object_has_member(root, "result")
                ? json_object_get_array_member(root, "result") : NULL;

        /* NAT list는 문자열 배열 반환 (ovn-nbctl lr-nat-list 출력) */
        if (arr) {
            printf("%s%-60s%s\n", cc(CYBER_CYAN), "NAT RULES", cc(CYBER_RESET));
            printf("────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                const gchar *line = json_array_get_string_element(arr, i);
                if (line && *line)
                    printf(" %s\n", line);
            }
            printf("────────────────────────────────────────────────────────────────────\n");
        }
        g_object_unref(parser); g_free(resp);

    } else {
        printf("%s[!] UNKNOWN OVN NAT ACTION: %s%s\n", cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  OVS-DPDK / SR-IOV (Phase 4) 명령
 *
 *  고성능 데이터플레인 가속 CLI 그룹.
 *  OVS-DPDK: 유저스페이스 패킷 처리, hugepage 관리, DPDK 브릿지
 *  SR-IOV: PCI 패스스루 VF 관리, VM 직접 연결
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_dpdk_status:
 *
 * OVS-DPDK 전체 상태 조회.
 * dpdk.status RPC → JSON 원시 출력.
 */
void cmd_dpdk_status(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("dpdk.status", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_dpdk_bind:
 *
 * NIC를 DPDK 호환 드라이버에 바인딩.
 * argv[2]=pci_addr (필수), argv[3]=driver (선택, 기본 vfio-pci).
 */
void cmd_dpdk_bind(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl dpdk bind <pci_addr> [driver]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pci_addr", argv[2]);
    if (argc >= 4)
        json_object_set_string_member(params, "driver", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("dpdk.bind", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_dpdk_unbind:
 *
 * NIC DPDK 바인딩 해제 (커널 드라이버 복원).
 * argv[2]=pci_addr (필수).
 */
void cmd_dpdk_unbind(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl dpdk unbind <pci_addr>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pci_addr", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("dpdk.unbind", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_dpdk_list:
 *
 * DPDK 바인딩 가능/완료 디바이스 목록.
 * dpdk.list RPC → JSON 원시 출력.
 */
void cmd_dpdk_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("dpdk.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_dpdk_bridge:
 *
 * DPDK 브릿지 생성/삭제.
 * argv[2]=create|delete, argv[3]=name (필수), argv[4]=dpdk_port (선택).
 */
void cmd_dpdk_bridge(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage:\n"
               "  pcvctl dpdk bridge create <name> [dpdk_port]\n"
               "  pcvctl dpdk bridge delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "create") == 0) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        if (argc >= 5)
            json_object_set_string_member(params, "dpdk_port", argv[4]);
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("dpdk.bridge.create", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (resp) { print_raw_response(resp); g_free(resp); }

    } else if (g_strcmp0(action, "delete") == 0) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("dpdk.bridge.delete", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (resp) { print_raw_response(resp); g_free(resp); }

    } else {
        printf("%s[!] UNKNOWN DPDK BRIDGE ACTION: %s%s\n",
            cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/**
 * cmd_dpdk_hugepage:
 *
 * Hugepage 현황 조회.
 * dpdk.hugepage.info RPC → JSON 원시 출력.
 */
void cmd_dpdk_hugepage(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("dpdk.hugepage.info", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_status:
 *
 * SR-IOV PF/VF 전체 상태 조회.
 * sriov.status RPC → JSON 원시 출력.
 */
void cmd_sriov_status(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.status", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_enable:
 *
 * SR-IOV VF 활성화.
 * argv[2]=pf (필수), argv[3]=num_vfs (선택, 기본 1).
 */
void cmd_sriov_enable(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl sriov enable <pf> [num_vfs]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pf", argv[2]);
    int num_vfs = (argc >= 4) ? atoi(argv[3]) : 1;
    json_object_set_int_member(params, "num_vfs", num_vfs);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.enable", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_disable:
 *
 * SR-IOV VF 비활성화.
 * argv[2]=pf (필수).
 */
void cmd_sriov_disable(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl sriov disable <pf>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pf", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.disable", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_list:
 *
 * VF 목록 조회.
 * argv[2]=pf (선택, 생략 시 전체 PF).
 */
void cmd_sriov_list(int argc, char *argv[]) {
    JsonObject *params = json_object_new();
    if (argc >= 3)
        json_object_set_string_member(params, "pf", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.list", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_set:
 *
 * VF 속성 설정.
 * argv[2]=pf, argv[3]=vf_index (필수), --mac/--vlan/--spoofchk 옵션.
 */
void cmd_sriov_set(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl sriov set <pf> <vf_index> [--mac XX] [--vlan N] [--spoofchk on|off]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "pf", argv[2]);
    json_object_set_int_member(params, "vf_index", atoi(argv[3]));
    for (int i = 4; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mac") == 0 && i+1 < argc)
            json_object_set_string_member(params, "mac", argv[++i]);
        else if (g_strcmp0(argv[i], "--vlan") == 0 && i+1 < argc)
            json_object_set_int_member(params, "vlan", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--spoofchk") == 0 && i+1 < argc)
            json_object_set_string_member(params, "spoofchk", argv[++i]);
    }
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_attach:
 *
 * VM에 SR-IOV VF 연결.
 * argv[2]=vm_name, argv[3]=pf (필수), argv[4]=vf_index (선택).
 */
void cmd_sriov_attach(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl sriov attach <vm_name> <pf> [vf_index]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    json_object_set_string_member(params, "pf", argv[3]);
    if (argc >= 5)
        json_object_set_int_member(params, "vf_index", atoi(argv[4]));
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.attach", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_sriov_detach:
 *
 * VM에서 SR-IOV VF 분리.
 * argv[2]=vm_name, argv[3]=pci_addr (필수).
 */
void cmd_sriov_detach(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl sriov detach <vm_name> <pci_addr>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    json_object_set_string_member(params, "pci_addr", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("sriov.detach", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ════════════════════════════════════════════════════════════════════
 *  RBAC (auth) 명령
 *
 *  역할 기반 접근 제어 사용자 관리 CLI 그룹.
 *  JWT 인증 시스템의 사용자 CRUD + 역할 변경을 담당한다.
 *
 *  역할 종류: admin, operator, viewer (REST API 접근 권한 수준 결정)
 *
 *  CLI 사용 예:
 *    pcvctl auth list
 *    pcvctl auth create testuser pass123 operator
 *    pcvctl auth role testuser admin
 *    pcvctl auth delete testuser
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_auth_list:
 *
 * 등록된 사용자 목록 조회.
 * auth.user.list RPC → USERNAME/ROLE/CREATED 테이블 출력.
 */
void cmd_auth_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("auth.user.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root, "result")
            ? json_object_get_array_member(root, "result") : NULL;

    if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
        const char *hdrs[] = {"USERNAME","ROLE","CREATED"};
        PcvTable   *t      = ptbl_new(hdrs, 3);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            ptbl_row(t,
                json_object_get_string_member_with_default(o, "username", "?"),
                json_object_get_string_member_with_default(o, "role", "?"),
                json_object_get_string_member_with_default(o, "created", "-"),
                NULL);
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else if (arr) {
        printf("%s%-25s │ %-15s │ %-25s%s\n", cc(CYBER_CYAN),
               "USERNAME", "ROLE", "CREATED", cc(CYBER_RESET));
        printf("────────────────────────────────────────────────────────────────────\n");
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            printf(" %-25s │ %-15s │ %-25s\n",
                json_object_get_string_member_with_default(o, "username", "?"),
                json_object_get_string_member_with_default(o, "role", "?"),
                json_object_get_string_member_with_default(o, "created", "-"));
        }
        printf("────────────────────────────────────────────────────────────────────\n");
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_auth_create:
 * @argc: 인수 개수 (최소 5: auth create <username> <password> <role>)
 * @argv: 인수 배열
 *
 * 새 사용자 생성. auth.user.create RPC 호출.
 * 비밀번호는 서버 측에서 해시 저장된다.
 */
void cmd_auth_create(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl auth create <username> <password> <role>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", argv[2]);
    json_object_set_string_member(params, "password", argv[3]);
    json_object_set_string_member(params, "role",     argv[4]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("auth.user.create", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "AUTH_USER_CREATE"); g_free(resp);
}

/**
 * cmd_auth_delete:
 * @argc: 인수 개수 (최소 3: auth delete <username>)
 * @argv: 인수 배열
 *
 * 사용자 삭제. auth.user.delete RPC 호출.
 */
void cmd_auth_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl auth delete <username>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("auth.user.delete", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "AUTH_USER_DELETE"); g_free(resp);
}

/**
 * cmd_auth_role:
 * @argc: 인수 개수 (최소 4: auth role <username> <role>)
 * @argv: 인수 배열
 *
 * 사용자 역할 변경. auth.role.set RPC 호출.
 * 유효한 역할: admin, operator, viewer.
 */
void cmd_auth_role(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl auth role <username> <role>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", argv[2]);
    json_object_set_string_member(params, "role",     argv[3]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("auth.role.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "AUTH_ROLE_SET"); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  Template 명령
 *
 *  VM 템플릿 관리 CLI 그룹.
 *  미리 정의된 VM 스펙(vCPU/RAM/디스크/OS)을 저장하여
 *  cluster.vm.create 등에서 재사용할 수 있게 한다.
 *
 *  CLI 사용 예:
 *    pcvctl template list
 *    pcvctl template get ubuntu-base
 *    pcvctl template create ubuntu-base --vcpu 4 --memory_mb 4096 --disk_gb 40 --os_variant ubuntu24.04
 *    pcvctl template delete ubuntu-base
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_template_list:
 *
 * 등록된 VM 템플릿 목록 조회.
 * template.list RPC → NAME/VCPU/MEMORY_MB/DISK_GB/OS_VARIANT 테이블 출력.
 */
void cmd_template_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("template.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root, "result")
            ? json_object_get_array_member(root, "result") : NULL;

    if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
        const char *hdrs[] = {"NAME","VCPU","MEMORY_MB","DISK_GB","OS_VARIANT"};
        PcvTable   *t      = ptbl_new(hdrs, 5);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            char vcpu_s[16], mem_s[16], disk_s[16];
            snprintf(vcpu_s, sizeof(vcpu_s), "%lld",
                (long long)json_object_get_int_member_with_default(o, "vcpu", 0));
            snprintf(mem_s, sizeof(mem_s), "%lld",
                (long long)json_object_get_int_member_with_default(o, "memory_mb", 0));
            snprintf(disk_s, sizeof(disk_s), "%lld",
                (long long)json_object_get_int_member_with_default(o, "disk_gb", 0));
            ptbl_row(t,
                json_object_get_string_member_with_default(o, "name", "?"),
                vcpu_s, mem_s, disk_s,
                json_object_get_string_member_with_default(o, "os_variant", "-"),
                NULL);
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else if (arr) {
        printf("%s%-20s │ %-6s │ %-10s │ %-8s │ %-15s%s\n", cc(CYBER_CYAN),
               "TEMPLATE", "VCPU", "MEMORY_MB", "DISK_GB", "OS_VARIANT", cc(CYBER_RESET));
        printf("────────────────────────────────────────────────────────────────────────────\n");
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            printf(" %-20s │ %-6lld │ %-10lld │ %-8lld │ %-15s\n",
                json_object_get_string_member_with_default(o, "name", "?"),
                (long long)json_object_get_int_member_with_default(o, "vcpu", 0),
                (long long)json_object_get_int_member_with_default(o, "memory_mb", 0),
                (long long)json_object_get_int_member_with_default(o, "disk_gb", 0),
                json_object_get_string_member_with_default(o, "os_variant", "-"));
        }
        printf("────────────────────────────────────────────────────────────────────────────\n");
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_template_get:
 * @argc: 인수 개수 (최소 3: template get <name>)
 * @argv: 인수 배열
 *
 * 특정 템플릿 상세 정보 조회.
 * template.get RPC → JSON 원시 출력 (포맷 무관하게 항상 JSON).
 */
void cmd_template_get(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl template get <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("template.get", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_template_create:
 * @argc: 인수 개수 (최소 3: template create <name> [옵션])
 * @argv: 인수 배열
 *
 * VM 템플릿 생성. template.create RPC 호출.
 * 옵션 플래그:
 *   --vcpu N        : vCPU 수
 *   --memory_mb N   : 메모리 (MB)
 *   --disk_gb N     : 디스크 크기 (GB)
 *   --os_variant X  : OS 변종 (예: ubuntu24.04)
 */
void cmd_template_create(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl template create <name> --vcpu N --memory_mb N --disk_gb N --os_variant X%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--vcpu") == 0 && i+1 < argc)
            json_object_set_int_member(params, "vcpu", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--memory_mb") == 0 && i+1 < argc)
            json_object_set_int_member(params, "memory_mb", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--disk_gb") == 0 && i+1 < argc)
            json_object_set_int_member(params, "disk_gb", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--os_variant") == 0 && i+1 < argc)
            json_object_set_string_member(params, "os_variant", argv[++i]);
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("template.create", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "TEMPLATE_CREATE"); g_free(resp);
}

/**
 * cmd_template_delete:
 * @argc: 인수 개수 (최소 3: template delete <name>)
 * @argv: 인수 배열
 *
 * VM 템플릿 삭제. template.delete RPC 호출.
 */
void cmd_template_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl template delete <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("template.delete", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "TEMPLATE_DELETE"); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  Backup 명령
 *
 *  ZFS 스냅샷 기반 자동 백업 정책 관리 CLI 그룹.
 *  VM별 백업 주기(interval_hours)와 보존 횟수(retention_count)를 설정하면
 *  데몬이 주기적으로 ZFS 스냅샷을 생성/정리한다.
 *
 *  CLI 사용 예:
 *    pcvctl backup list
 *    pcvctl backup set web-prod --interval 1 --retention 24
 *    pcvctl backup history web-prod
 *    pcvctl backup delete web-prod
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_backup_list:
 *
 * 설정된 백업 정책 목록 조회.
 * backup.policy.list RPC → VM_NAME/INTERVAL_HOURS/RETENTION/ENABLED 테이블 출력.
 */
void cmd_backup_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("backup.policy.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root, "result")
            ? json_object_get_array_member(root, "result") : NULL;

    if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
        const char *hdrs[] = {"VM_NAME","INTERVAL_HOURS","RETENTION","ENABLED"};
        PcvTable   *t      = ptbl_new(hdrs, 4);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            char intv_s[16], ret_s[16];
            snprintf(intv_s, sizeof(intv_s), "%lld",
                (long long)json_object_get_int_member_with_default(o, "interval_hours", 0));
            snprintf(ret_s, sizeof(ret_s), "%lld",
                (long long)json_object_get_int_member_with_default(o, "retention_count", 0));
            ptbl_row(t,
                json_object_get_string_member_with_default(o, "vm_name", "?"),
                intv_s, ret_s,
                json_object_get_boolean_member_with_default(o, "enabled", FALSE) ? "yes" : "no",
                NULL);
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else if (arr) {
        printf("%s%-20s │ %-16s │ %-10s │ %-8s%s\n", cc(CYBER_CYAN),
               "VM_NAME", "INTERVAL(hours)", "RETENTION", "ENABLED", cc(CYBER_RESET));
        printf("──────────────────────────────────────────────────────────────────────────────\n");
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            printf(" %-20s │ %-16lld │ %-10lld │ %-8s\n",
                json_object_get_string_member_with_default(o, "vm_name", "?"),
                (long long)json_object_get_int_member_with_default(o, "interval_hours", 0),
                (long long)json_object_get_int_member_with_default(o, "retention_count", 0),
                json_object_get_boolean_member_with_default(o, "enabled", FALSE) ? "yes" : "no");
        }
        printf("──────────────────────────────────────────────────────────────────────────────\n");
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_backup_set:
 * @argc: 인수 개수 (최소 3: backup set <vm_name> [옵션])
 * @argv: 인수 배열
 *
 * VM 백업 정책 설정/갱신. backup.policy.set RPC 호출.
 * 옵션:
 *   --interval N  : 백업 주기 (시간 단위)
 *   --retention N : 보존할 스냅샷 수
 */
void cmd_backup_set(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl backup set <vm_name> --interval N --retention N%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--interval") == 0 && i+1 < argc)
            json_object_set_int_member(params, "interval_hours", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--retention") == 0 && i+1 < argc)
            json_object_set_int_member(params, "retention_count", atoi(argv[++i]));
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("backup.policy.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "BACKUP_POLICY_SET"); g_free(resp);
}

/**
 * cmd_backup_delete:
 * @argc: 인수 개수 (최소 3: backup delete <vm_name>)
 * @argv: 인수 배열
 *
 * VM 백업 정책 삭제. backup.policy.delete RPC 호출.
 * 기존 스냅샷은 유지되며, 자동 백업만 중단된다.
 */
void cmd_backup_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl backup delete <vm_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("backup.policy.delete", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "BACKUP_POLICY_DELETE"); g_free(resp);
}

/**
 * cmd_backup_history:
 * @argc: 인수 개수 (최소 3: backup history <vm_name>)
 * @argv: 인수 배열
 *
 * VM 백업 이력 조회. backup.history RPC 호출.
 * 현재 daemon은 문자열 배열 ["snap1","snap2",...]을 반환한다.
 */
void cmd_backup_history(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl backup history <vm_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("backup.history", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root, "result")
            ? json_object_get_array_member(root, "result") : NULL;

    if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
        const char *hdrs[] = {"SNAPSHOT"};
        PcvTable   *t      = ptbl_new(hdrs, 1);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            const gchar *snap = json_array_get_string_element(arr, i);
            ptbl_row(t,
                snap ? snap : "?",
                NULL);
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else if (arr) {
        printf("%s%-40s%s\n", cc(CYBER_CYAN), "SNAPSHOT", cc(CYBER_RESET));
        printf("──────────────────────────────────────────────\n");
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            const gchar *snap = json_array_get_string_element(arr, i);
            printf(" %-40s\n", snap ? snap : "?");
        }
        printf("──────────────────────────────────────────────\n");
    }
    g_object_unref(parser); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  NIC 관리 명령 (device.nic.list / device.nic.attach / device.nic.detach)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_nic_list - VM에 연결된 NIC 목록 조회
 * Usage: pcvctl nic list <vm_name>
 * RPC: device.nic.list {name: vm_name}
 */
void cmd_nic_list(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl nic list <vm_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("device.nic.list", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "result")) {
        JsonNode *res_node = json_object_get_member(root, "result");
        if (res_node && JSON_NODE_HOLDS_ARRAY(res_node)) {
            JsonArray *arr = json_node_get_array(res_node);
            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *n = json_array_get_object_element(arr, i);
                    printf("%s\t%s\t%s\n",
                        json_object_get_string_member_with_default(n, "mac", "-"),
                        json_object_get_string_member_with_default(n, "bridge", "-"),
                        json_object_get_string_member_with_default(n, "model", "-"));
                }
            } else {
                print_cyber_banner();
                printf("%s [ NIC LIST: %s ]\n\n%s", cc(CYBER_CYAN), argv[2], cc(CYBER_RESET));
                printf("%s%-20s │ %-18s │ %-10s%s\n", cc(CYBER_CYAN),
                    "MAC", "BRIDGE", "MODEL", cc(CYBER_RESET));
                printf("────────────────────────────────────────────────────────\n");
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *n = json_array_get_object_element(arr, i);
                    printf(" %-20s │ %-18s │ %-10s\n",
                        json_object_get_string_member_with_default(n, "mac", "-"),
                        json_object_get_string_member_with_default(n, "bridge", "-"),
                        json_object_get_string_member_with_default(n, "model", "-"));
                }
                printf("────────────────────────────────────────────────────────\n");
            }
        } else {
            print_action_response(resp, "NIC_LIST");
        }
    } else {
        print_action_response(resp, "NIC_LIST");
    }
    g_object_unref(parser);
    g_free(resp);
}

/**
 * cmd_nic_add - VM에 NIC 추가 (핫플러그)
 * Usage: pcvctl nic add <vm_name> <bridge>
 * RPC: device.nic.attach {name: vm_name, bridge: bridge}
 */
void cmd_nic_add(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl nic add <vm_name> <bridge>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "bridge", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("device.nic.attach", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NIC_ATTACH");
    g_free(resp);
}

/**
 * cmd_nic_remove - VM에서 NIC 제거 (핫플러그)
 * Usage: pcvctl nic remove <vm_name> <mac>
 * RPC: device.nic.detach {name: vm_name, mac: mac}
 */
void cmd_nic_remove(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl nic remove <vm_name> <mac>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "mac", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("device.nic.detach", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NIC_DETACH");
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  ISO 관리 명령 (vm.mount_iso / vm.eject / iso.list)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_iso_mount - VM에 ISO 마운트
 * Usage: pcvctl iso mount <vm_name> <iso_path>
 * RPC: vm.mount_iso {name: vm_name, iso_path: path}
 */
void cmd_iso_mount(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl iso mount <vm_name> <iso_path>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "iso_path", argv[3]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.mount_iso", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "ISO_MOUNT");
    g_free(resp);
}

/**
 * cmd_iso_eject - VM에서 ISO 추출
 * Usage: pcvctl iso eject <vm_name>
 * RPC: vm.eject {name: vm_name}
 */
void cmd_iso_eject(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl iso eject <vm_name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.eject", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "ISO_EJECT");
    g_free(resp);
}

/**
 * cmd_iso_list - 사용 가능한 ISO 파일 목록 조회
 * Usage: pcvctl iso list
 * RPC: iso.list {}
 */
void cmd_iso_list(int argc, char *argv[]) {
    (void)argc; (void)argv;
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("iso.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "result")) {
        JsonNode *res_node = json_object_get_member(root, "result");
        if (res_node && JSON_NODE_HOLDS_ARRAY(res_node)) {
            JsonArray *arr = json_node_get_array(res_node);
            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                for (guint i = 0; i < json_array_get_length(arr); i++)
                    printf("%s\n", json_array_get_string_element(arr, i));
            } else {
                print_cyber_banner();
                printf("%s [ ISO FILES ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
                printf("%s%-60s%s\n", cc(CYBER_CYAN), "FILENAME", cc(CYBER_RESET));
                printf("────────────────────────────────────────────────────────────\n");
                for (guint i = 0; i < json_array_get_length(arr); i++)
                    printf(" %s\n", json_array_get_string_element(arr, i));
                printf("────────────────────────────────────────────────────────────\n");
            }
        } else {
            print_action_response(resp, "ISO_LIST");
        }
    } else {
        print_action_response(resp, "ISO_LIST");
    }
    g_object_unref(parser);
    g_free(resp);
}

/** cmd_vm_usb_attach - USB host device passthrough attach. vm.usb.attach RPC. */
void cmd_vm_usb_attach(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl vm usb-attach <name> <vendor_id> <product_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("Example: pcvctl vm usb-attach web-prod 0x1234 0x5678\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    json_object_set_string_member(params, "vendor_id", argv[3]);
    json_object_set_string_member(params, "product_id", argv[4]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.attach", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "USB_ATTACH");
    g_free(resp);
}

/** cmd_vm_usb_detach - USB host device passthrough detach. vm.usb.detach RPC. */
void cmd_vm_usb_detach(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl vm usb-detach <name> <vendor_id> <product_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("Example: pcvctl vm usb-detach web-prod 0x1234 0x5678\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    json_object_set_string_member(params, "vendor_id", argv[3]);
    json_object_set_string_member(params, "product_id", argv[4]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.detach", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "USB_DETACH");
    g_free(resp);
}

/** cmd_vm_usb_list - USB host devices attached to VM. vm.usb.list RPC. */
void cmd_vm_usb_list(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm usb-list <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.list", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED),
            json_object_get_string_member_with_default(err, "message", "USB list failed"),
            ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result") &&
               JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "result"))) {
        JsonArray *arr = json_node_get_array(json_object_get_member(root, "result"));
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *usb = json_array_get_object_element(arr, i);
                printf("%s,%s\n",
                    json_object_get_string_member_with_default(usb, "vendor_id", "-"),
                    json_object_get_string_member_with_default(usb, "product_id", "-"));
            }
        } else {
            print_cyber_banner();
            printf("%s [ VM USB DEVICES ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s%-14s %-14s%s\n", cc(CYBER_CYAN), "VENDOR_ID", "PRODUCT_ID", cc(CYBER_RESET));
            printf("──────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *usb = json_array_get_object_element(arr, i);
                printf(" %-14s %-14s\n",
                    json_object_get_string_member_with_default(usb, "vendor_id", "-"),
                    json_object_get_string_member_with_default(usb, "product_id", "-"));
            }
            if (json_array_get_length(arr) == 0)
                printf(" (no USB hostdev attached)\n");
            printf("──────────────────────────────\n");
        }
    } else {
        print_action_response(resp, "USB_LIST");
    }
    g_object_unref(parser);
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  노드 관리 명령 (node.drain / node.resume / daemon.version)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_node_drain - 노드 드레인 모드 진입 (신규 RPC 수신 중단)
 * Usage: pcvctl node drain [--timeout N]
 * RPC: node.drain {timeout_sec: N}  (기본값 30)
 */
void cmd_node_drain(int argc, char *argv[]) {
    int timeout_sec = 30;
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_sec = atoi(argv[++i]);
    }
    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "timeout_sec", timeout_sec);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("node.drain", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NODE_DRAIN");
    g_free(resp);
}

/**
 * cmd_node_resume - 드레인 모드 해제 (RPC 수신 재개)
 * Usage: pcvctl node resume
 * RPC: node.resume {}
 */
void cmd_node_resume(int argc, char *argv[]) {
    (void)argc; (void)argv;
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("node.resume", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NODE_RESUME");
    g_free(resp);
}

/**
 * cmd_node_version - 데몬 버전 조회
 * Usage: pcvctl node version
 * RPC: daemon.version {}
 */
void cmd_node_version(int argc, char *argv[]) {
    (void)argc; (void)argv;
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("daemon.version", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "result")) {
        JsonObject  *res     = json_object_get_object_member(root, "result");
        const gchar *version = json_object_get_string_member_with_default(res, "version", "unknown");
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("version\t%s\n", version);
        } else {
            print_cyber_banner();
            printf("%s [ DAEMON VERSION ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s VERSION : %s%s\n", cc(CYBER_GREEN), version, cc(CYBER_RESET));
            printf("%s────────────────────────────────────────%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
        }
    } else {
        print_action_response(resp, "DAEMON_VERSION");
    }
    g_object_unref(parser);
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  VM 삭제 상태 조회 (vm.delete.status)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_vm_delete_status - VM 삭제 진행 상태 조회
 * Usage: pcvctl vm delete-status <name>
 * RPC: vm.delete.status {name: name}
 */
void cmd_vm_delete_status(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm delete-status <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.delete.status", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "result")) {
        /* VP-4: 데몬 계약은 result = 상태 문자열 값 노드. 객체 취급하면
         * Json-CRITICAL. 값/객체 양쪽 수용, 빈 값은 안내 문구로. */
        JsonNode    *rn     = json_object_get_member(root, "result");
        const gchar *status = NULL;
        const gchar *name   = argv[2];
        if (rn && JSON_NODE_HOLDS_VALUE(rn)) {
            status = json_node_get_string(rn);
        } else if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
            JsonObject *res = json_node_get_object(rn);
            status = json_object_get_string_member_with_default(res, "status", NULL);
            name   = json_object_get_string_member_with_default(res, "name", argv[2]);
        }
        if (!status || !*status) status = "no delete record";
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("%s\t%s\n", name, status);
        } else {
            print_cyber_banner();
            printf("%s [ VM DELETE STATUS ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s VM     : %s%s\n", cc(CYBER_GREEN), name, cc(CYBER_RESET));
            printf("%s STATUS : %s%s\n", cc(CYBER_YELLOW), status, cc(CYBER_RESET));
            printf("%s────────────────────────────────────────%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
        }
    } else {
        print_action_response(resp, "VM_DELETE_STATUS");
    }
    g_object_unref(parser);
    g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  VM 고급 조회/Guest Agent 명령
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_vm_memory_stats - VM 메모리 통계 조회
 * Usage: pcvctl vm memory-stats <name>
 * RPC: vm.memory.stats {name: name}
 */
void cmd_vm_memory_stats(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm memory-stats <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.memory.stats", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
        g_object_unref(parser); g_free(resp); return;
    }
    if (json_object_has_member(root, "result")) {
        JsonObject *res = json_object_get_object_member(root, "result");
        const char *keys[] = {"actual_balloon_kb","rss_kb","unused_kb",
                              "available_kb","usable_kb","swap_in","swap_out"};
        const char *labels[] = {"Actual Balloon (KB)","RSS (KB)","Unused (KB)",
                                "Available (KB)","Usable (KB)","Swap In","Swap Out"};
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"METRIC","VALUE"};
            PcvTable *t = ptbl_new(hdrs, 2);
            for (int i = 0; i < 7; i++) {
                gchar vbuf[32];
                g_snprintf(vbuf, sizeof(vbuf), "%lld",
                    (long long)json_object_get_int_member_with_default(res, keys[i], 0));
                ptbl_row(t, labels[i], vbuf);
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else {
            print_cyber_banner();
            printf("%s [ VM MEMORY STATS: %s ]%s\n\n",
                cc(CYBER_CYAN), argv[2], cc(CYBER_RESET));
            printf("%s%-22s │ %s%s\n", cc(CYBER_CYAN),
                "METRIC", "VALUE", cc(CYBER_RESET));
            printf("─────────────────────────┼──────────────────\n");
            for (int i = 0; i < 7; i++) {
                printf(" %s%-22s%s │ %s%lld%s\n",
                    cc(CYBER_YELLOW), labels[i], cc(CYBER_RESET),
                    cc(CYBER_GREEN),
                    (long long)json_object_get_int_member_with_default(res, keys[i], 0),
                    cc(CYBER_RESET));
            }
            printf("─────────────────────────┴──────────────────\n");
        }
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_vm_cpu_stats - VM CPU 통계 조회
 * Usage: pcvctl vm cpu-stats <name>
 * RPC: vm.cpu.stats {name: name}
 */
void cmd_vm_cpu_stats(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm cpu-stats <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.cpu.stats", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
        g_object_unref(parser); g_free(resp); return;
    }
    if (json_object_has_member(root, "result")) {
        JsonObject *res = json_object_get_object_member(root, "result");
        gint64 vcpu_count = json_object_get_int_member_with_default(res, "vcpu_count", 0);
        gint64 max_vcpu   = json_object_get_int_member_with_default(res, "max_vcpu", 0);
        gint64 cpu_time   = json_object_get_int_member_with_default(res, "cpu_time_ns", 0);

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("vcpu_count\t%lld\n", (long long)vcpu_count);
            printf("max_vcpu\t%lld\n",   (long long)max_vcpu);
            printf("cpu_time_ns\t%lld\n", (long long)cpu_time);
            JsonArray *vcpus = json_object_has_member(res, "vcpus")
                ? json_object_get_array_member(res, "vcpus") : NULL;
            if (vcpus) {
                for (guint i = 0; i < json_array_get_length(vcpus); i++) {
                    JsonObject *v = json_array_get_object_element(vcpus, i);
                    gint64 state_code = json_object_get_int_member_with_default(v, "state", -1);
                    const gchar *state = state_code == 0 ? "offline" :
                                         state_code == 1 ? "running" :
                                         state_code == 2 ? "blocked" :
                                         "unknown";
                    printf("vcpu\t%lld\t%s\t%lld\t%s\n",
                        (long long)json_object_get_int_member_with_default(v, "number", 0),
                        state,
                        (long long)json_object_get_int_member_with_default(v, "cpu_time", 0),
                        json_object_get_string_member_with_default(v, "cpu_affinity", "?"));
                }
            }
        } else {
            print_cyber_banner();
            printf("%s [ VM CPU STATS: %s ]%s\n\n",
                cc(CYBER_CYAN), argv[2], cc(CYBER_RESET));
            printf(" %svCPU Count : %s%lld%s\n",
                cc(CYBER_YELLOW), cc(CYBER_GREEN), (long long)vcpu_count, cc(CYBER_RESET));
            printf(" %sMax vCPU   : %s%lld%s\n",
                cc(CYBER_YELLOW), cc(CYBER_GREEN), (long long)max_vcpu, cc(CYBER_RESET));
            printf(" %sCPU Time   : %s%lld ns%s\n\n",
                cc(CYBER_YELLOW), cc(CYBER_GREEN), (long long)cpu_time, cc(CYBER_RESET));

            JsonArray *vcpus = json_object_has_member(res, "vcpus")
                ? json_object_get_array_member(res, "vcpus") : NULL;
            if (vcpus && json_array_get_length(vcpus) > 0) {
                printf("%s%-6s │ %-10s │ %-14s │ %-10s%s\n", cc(CYBER_CYAN),
                    "vCPU", "STATE", "CPU_TIME", "AFFINITY", cc(CYBER_RESET));
                printf("───────┼────────────┼────────────────┼───────────\n");
                for (guint i = 0; i < json_array_get_length(vcpus); i++) {
                    JsonObject *v = json_array_get_object_element(vcpus, i);
                    gint64 state_code = json_object_get_int_member_with_default(v, "state", -1);
                    const gchar *st = state_code == 0 ? "offline" :
                                      state_code == 1 ? "running" :
                                      state_code == 2 ? "blocked" :
                                      "unknown";
                    const gchar *sc = g_strcmp0(st, "running") == 0 ? cc(CYBER_GREEN) : cc(CYBER_DIM);
                    printf(" %-5lld │ %s%-10s%s │ %-14lld │ %-10s\n",
                        (long long)json_object_get_int_member_with_default(v, "number", 0),
                        sc, st, cc(CYBER_RESET),
                        (long long)json_object_get_int_member_with_default(v, "cpu_time", 0),
                        json_object_get_string_member_with_default(v, "cpu_affinity", "-"));
                }
                printf("───────┴────────────┴────────────────┴───────────\n");
            }
        }
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_vm_disk_resize - VM 디스크 라이브 리사이즈
 * Usage: pcvctl vm disk-resize <name> <target> <new_size_gb>
 * RPC: vm.disk.live_resize {name, target, new_size_gb}
 */
void cmd_vm_disk_resize(int argc, char *argv[]) {
    if (argc < 5) {
        printf("%sUsage: pcvctl vm disk-resize <name> <target> <new_size_gb>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name",   argv[2]);
    json_object_set_string_member(params, "target", argv[3]);
    json_object_set_int_member   (params, "new_size_gb", atoi(argv[4]));
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.disk.live_resize", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "DISK_RESIZE");
    g_free(resp);
}

/**
 * cmd_vm_guest_agent_status - Guest Agent channel/agent 상태 진단
 * Usage: pcvctl vm guest-agent-status <name>
 * RPC: vm.guest.agent.status {name: name}
 */
void cmd_vm_guest_agent_status(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm guest-agent-status <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.guest.agent.status", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result")) {
        JsonObject *res = json_object_get_object_member(root, "result");
        const gchar *status = json_object_get_string_member_with_default(res, "status", "unknown");
        const gchar *message = json_object_get_string_member_with_default(res, "message", "-");
        gboolean running = json_object_get_boolean_member_with_default(res, "running", FALSE);
        gboolean configured = json_object_get_boolean_member_with_default(res, "channel_configured", FALSE);
        gboolean live = json_object_get_boolean_member_with_default(res, "channel_live", FALSE);
        gboolean ping = json_object_get_boolean_member_with_default(res, "agent_ping", FALSE);
        gboolean package_required = json_object_get_boolean_member_with_default(res, "package_required", FALSE);
        gboolean reboot_required = json_object_get_boolean_member_with_default(res, "reboot_required", FALSE);

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("name\t%s\nstatus\t%s\nrunning\t%s\nchannel_configured\t%s\nchannel_live\t%s\nagent_ping\t%s\npackage_required\t%s\nreboot_required\t%s\nmessage\t%s\n",
                argv[2], status, running ? "true" : "false",
                configured ? "true" : "false", live ? "true" : "false",
                ping ? "true" : "false", package_required ? "true" : "false",
                reboot_required ? "true" : "false", message);
        } else {
            print_cyber_banner();
            printf("%s [ GUEST AGENT STATUS: %s ]%s\n\n",
                cc(CYBER_CYAN), argv[2], cc(CYBER_RESET));
            printf(" %sStatus      : %s%s%s\n", cc(CYBER_YELLOW),
                ping ? cc(CYBER_GREEN) : cc(CYBER_RED), status, cc(CYBER_RESET));
            printf(" %sRunning     : %s%s%s\n", cc(CYBER_YELLOW),
                running ? cc(CYBER_GREEN) : cc(CYBER_DIM),
                running ? "true" : "false", cc(CYBER_RESET));
            printf(" %sChannel cfg : %s%s%s\n", cc(CYBER_YELLOW),
                configured ? cc(CYBER_GREEN) : cc(CYBER_RED),
                configured ? "true" : "false", cc(CYBER_RESET));
            printf(" %sChannel live: %s%s%s\n", cc(CYBER_YELLOW),
                live ? cc(CYBER_GREEN) : cc(CYBER_RED),
                live ? "true" : "false", cc(CYBER_RESET));
            printf(" %sAgent ping  : %s%s%s\n", cc(CYBER_YELLOW),
                ping ? cc(CYBER_GREEN) : cc(CYBER_RED),
                ping ? "true" : "false", cc(CYBER_RESET));
            if (package_required)
                printf(" %sPackage     : qemu-guest-agent install/start required%s\n",
                    cc(CYBER_RED), cc(CYBER_RESET));
            if (reboot_required)
                printf(" %sReboot      : required to activate configured channel%s\n",
                    cc(CYBER_YELLOW), cc(CYBER_RESET));
            printf("\n%s%s%s\n", cc(CYBER_DIM), message, cc(CYBER_RESET));
        }
    }
    g_object_unref(parser);
    g_free(resp);
}

/**
 * cmd_vm_guest_agent_ensure_channel - Guest Agent libvirt channel 보정
 * Usage: pcvctl vm guest-agent-ensure-channel <name>
 * RPC: vm.guest.agent.ensure_channel {name: name}
 */
void cmd_vm_guest_agent_ensure_channel(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm guest-agent-ensure-channel <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.guest.agent.ensure_channel", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "GUEST_AGENT_ENSURE_CHANNEL");
    g_free(resp);
}

/**
 * cmd_vm_guest_ping - Guest Agent 연결 확인
 * Usage: pcvctl vm guest-ping <name>
 * RPC: vm.guest.ping {name: name}
 */
void cmd_vm_guest_ping(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm guest-ping <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.guest.ping", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] Guest agent: NOT REACHABLE [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
    } else {
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("%s\tconnected\n", argv[2]);
        } else {
            printf("%s[+] Guest agent: %sconnected%s\n",
                cc(CYBER_GREEN), cc(CYBER_BOLD), cc(CYBER_RESET));
        }
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_vm_guest_exec - Guest Agent 명령 실행
 * Usage: pcvctl vm guest-exec <name> <command>
 * RPC: vm.guest.exec {name, command}
 */
void cmd_vm_guest_exec(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm guest-exec <name> <command>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    /* argv[3..] 을 하나의 명령 문자열로 결합 */
    GString *cmd_str = g_string_new(argv[3]);
    for (int i = 4; i < argc; i++) {
        g_string_append_c(cmd_str, ' ');
        g_string_append(cmd_str, argv[i]);
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name",    argv[2]);
    json_object_set_string_member(params, "command", cmd_str->str);
    g_string_free(cmd_str, TRUE);

    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.guest.exec", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result")) {
        JsonObject  *res     = json_object_get_object_member(root, "result");
        const gchar *out     = json_object_get_string_member_with_default(res, "stdout", "");
        const gchar *err_out = json_object_get_string_member_with_default(res, "stderr", "");
        /* VP-3: 데몬 직렬화 키는 "exitcode" — 구키 "exit_code" 조회는 항상 기본값 -1이었음.
         * 표시 라벨은 스크립트 파싱 호환을 위해 exit_code 유지.
         * exited 부재(구버전 데몬)는 true 간주. */
        gint64       exitc   = json_object_get_int_member_with_default(res, "exitcode", -1);
        gboolean     exited  = json_object_get_boolean_member_with_default(res, "exited", TRUE);
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            if (!exited)    printf("exited\tfalse\n");
            printf("exit_code\t%lld\n", (long long)exitc);
            if (out[0])     printf("stdout\t%s\n", out);
            if (err_out[0]) printf("stderr\t%s\n", err_out);
        } else {
            if (out[0])
                printf("%s%s%s", cc(CYBER_RESET), out,
                    out[strlen(out)-1] != '\n' ? "\n" : "");
            if (err_out[0])
                printf("%s%s%s", cc(CYBER_RED), err_out,
                    err_out[strlen(err_out)-1] != '\n' ? "\n" : "");
            if (!exited)
                printf("%s[still running — exit code unavailable within 10s budget]%s\n",
                    cc(CYBER_YELLOW), cc(CYBER_RESET));
            const gchar *ec_color = (exitc == 0) ? cc(CYBER_GREEN) : cc(CYBER_RED);
            printf("%s[exit_code: %s%lld%s]%s\n",
                cc(CYBER_DIM), ec_color, (long long)exitc, cc(CYBER_DIM), cc(CYBER_RESET));
        }
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * cmd_vm_guest_shutdown - Guest Agent 기반 안전 종료
 * Usage: pcvctl vm guest-shutdown <name>
 * RPC: vm.guest.shutdown {name: name}
 */
void cmd_vm_guest_shutdown(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm guest-shutdown <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.guest.shutdown", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result")) {
        JsonObject  *res    = json_object_get_object_member(root, "result");
        const gchar *method = json_object_get_string_member_with_default(res, "method", "acpi");
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("%s\tshutdown_initiated\t%s\n", argv[2], method);
        } else {
            printf("%s[+] Shutdown initiated%s (method: %s%s%s)\n",
                cc(CYBER_GREEN), cc(CYBER_RESET),
                cc(CYBER_YELLOW), method, cc(CYBER_RESET));
        }
    }
    g_object_unref(parser); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  P2: 알림 관리 (Alert Management)
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_alert_list - 알림 히스토리 조회. alert.history RPC 호출. */
void cmd_alert_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("alert.history", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray  *arr  = json_object_has_member(root, "result")
            ? json_object_get_array_member(root, "result") : NULL;

    if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
        const char *hdrs[] = {"TIMESTAMP","SEVERITY","METRIC","VALUE","THRESHOLD"};
        PcvTable   *t      = ptbl_new(hdrs, 5);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            gchar val_buf[32], thr_buf[32];
            g_snprintf(val_buf, sizeof(val_buf), "%.1f",
                json_object_get_double_member_with_default(o, "value", 0));
            g_snprintf(thr_buf, sizeof(thr_buf), "%.1f",
                json_object_get_double_member_with_default(o, "threshold", 0));
            ptbl_row(t,
                json_object_get_string_member_with_default(o, "timestamp", "?"),
                json_object_get_string_member_with_default(o, "severity", "?"),
                json_object_get_string_member_with_default(o, "metric", "?"),
                val_buf, thr_buf);
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else if (arr) {
        printf("%s%-22s │ %-10s │ %-12s │ %-8s │ %-8s%s\n", cc(CYBER_CYAN),
               "TIMESTAMP", "SEVERITY", "METRIC", "VALUE", "THRESHOLD", cc(CYBER_RESET));
        printf("────────────────────────────────────────────────────────────────────────────\n");
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            printf(" %-22s │ %-10s │ %-12s │ %7.1f │ %7.1f\n",
                json_object_get_string_member_with_default(o, "timestamp", "?"),
                json_object_get_string_member_with_default(o, "severity", "?"),
                json_object_get_string_member_with_default(o, "metric", "?"),
                json_object_get_double_member_with_default(o, "value", 0),
                json_object_get_double_member_with_default(o, "threshold", 0));
        }
        printf("────────────────────────────────────────────────────────────────────────────\n");
    }
    g_object_unref(parser); g_free(resp);
}

/** cmd_alert_config - 알림 설정 조회. alert.config.get RPC 호출. */
void cmd_alert_config(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("alert.config.get", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_alert_set - 알림 임계값/Webhook 설정.
 * Usage: pcvctl alert set --cpu_warn N --cpu_crit N --mem_warn N
 *        --mem_crit N --webhook URL
 * RPC: alert.config.set {cpu_warn, cpu_crit, mem_warn, mem_crit, webhook_url}
 */
void cmd_alert_set(int argc, char *argv[]) {
    JsonObject *params = json_object_new();
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--cpu_warn") == 0 && i+1 < argc)
            json_object_set_int_member(params, "cpu_warn", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--cpu_crit") == 0 && i+1 < argc)
            json_object_set_int_member(params, "cpu_crit", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--mem_warn") == 0 && i+1 < argc)
            json_object_set_int_member(params, "mem_warn", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--mem_crit") == 0 && i+1 < argc)
            json_object_set_int_member(params, "mem_crit", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--webhook") == 0 && i+1 < argc)
            json_object_set_string_member(params, "webhook_url", argv[++i]);
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("alert.config.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "ALERT_CONFIG_SET"); g_free(resp);
}

/**
 * cmd_alert_reload - 알림 설정 리로드
 * Usage: pcvctl alert reload
 * RPC: alert.config.reload {}
 */
void cmd_alert_reload(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("alert.config.reload", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result")) {
        JsonObject *res = json_object_get_object_member(root, "result");
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            printf("alert_reload\tOK\n");
            if (json_object_has_member(res, "cpu_warn"))
                printf("cpu_warn\t%lld\n", (long long)json_object_get_int_member(res, "cpu_warn"));
            if (json_object_has_member(res, "cpu_crit"))
                printf("cpu_crit\t%lld\n", (long long)json_object_get_int_member(res, "cpu_crit"));
            if (json_object_has_member(res, "mem_warn"))
                printf("mem_warn\t%lld\n", (long long)json_object_get_int_member(res, "mem_warn"));
            if (json_object_has_member(res, "mem_crit"))
                printf("mem_crit\t%lld\n", (long long)json_object_get_int_member(res, "mem_crit"));
            if (json_object_has_member(res, "enabled"))
                printf("enabled\t%s\n",
                    json_object_get_boolean_member(res, "enabled") ? "true" : "false");
        } else {
            printf("%s%s[+] Alert config reloaded%s\n",
                cc(CYBER_GREEN), cc(CYBER_BOLD), cc(CYBER_RESET));
            if (json_object_has_member(res, "cpu_warn"))
                printf("  %sCPU warn : %s%lld%%%s\n", cc(CYBER_YELLOW), cc(CYBER_GREEN),
                    (long long)json_object_get_int_member(res, "cpu_warn"), cc(CYBER_RESET));
            if (json_object_has_member(res, "cpu_crit"))
                printf("  %sCPU crit : %s%lld%%%s\n", cc(CYBER_YELLOW), cc(CYBER_RED),
                    (long long)json_object_get_int_member(res, "cpu_crit"), cc(CYBER_RESET));
            if (json_object_has_member(res, "mem_warn"))
                printf("  %sMEM warn : %s%lld%%%s\n", cc(CYBER_YELLOW), cc(CYBER_GREEN),
                    (long long)json_object_get_int_member(res, "mem_warn"), cc(CYBER_RESET));
            if (json_object_has_member(res, "mem_crit"))
                printf("  %sMEM crit : %s%lld%%%s\n", cc(CYBER_YELLOW), cc(CYBER_RED),
                    (long long)json_object_get_int_member(res, "mem_crit"), cc(CYBER_RESET));
            if (json_object_has_member(res, "enabled"))
                printf("  %sEnabled  : %s%s%s\n", cc(CYBER_YELLOW),
                    json_object_get_boolean_member(res, "enabled") ? cc(CYBER_GREEN) : cc(CYBER_RED),
                    json_object_get_boolean_member(res, "enabled") ? "true" : "false",
                    cc(CYBER_RESET));
        }
    }
    g_object_unref(parser); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  P2: 클러스터 페일오버/복제 명령
 * ════════════════════════════════════════════════════════════════════ */


/* ════════════════════════════════════════════════════════════════════
 *  P3: AI Agent 명령
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_agent_config - AI Agent 설정 조회. agent.config.get RPC 호출. */
void cmd_agent_config(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("agent.config.get", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/**
 * cmd_agent_set - AI Agent 프로바이더 설정.
 * Usage: pcvctl agent set --provider NAME --api_key KEY --enabled true/false
 * RPC: agent.config.set {providers: {NAME: {api_key, enabled}}}
 */
void cmd_agent_set(int argc, char *argv[]) {
    const char *provider = NULL;
    const char *api_key  = NULL;
    const char *enabled  = NULL;
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--provider") == 0 && i+1 < argc)
            provider = argv[++i];
        else if (g_strcmp0(argv[i], "--api_key") == 0 && i+1 < argc)
            api_key = argv[++i];
        else if (g_strcmp0(argv[i], "--enabled") == 0 && i+1 < argc)
            enabled = argv[++i];
    }
    if (!provider) {
        printf("%sUsage: pcvctl agent set --provider NAME --api_key KEY --enabled true/false%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *prov_cfg = json_object_new();
    if (api_key) json_object_set_string_member(prov_cfg, "api_key", api_key);
    if (enabled) json_object_set_boolean_member(prov_cfg, "enabled",
        g_strcmp0(enabled, "true") == 0);
    JsonObject *providers = json_object_new();
    json_object_set_object_member(providers, provider, prov_cfg);
    JsonObject *params = json_object_new();
    json_object_set_object_member(params, "providers", providers);
    GError *error = NULL;
    gchar *resp = purectl_send_request("agent.config.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "AGENT_CONFIG_SET"); g_free(resp);
}

/** cmd_agent_history - AI 합의 이력 조회. agent.history RPC 호출. */
void cmd_agent_history(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("agent.history", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ════════════════════════════════════════════════════════════════════
 *  P3: OVN ACL/DHCP 확장 명령
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_ovn_acl - OVN ACL 관리.
 * Usage:
 *   pcvctl ovn acl list <switch>
 *   pcvctl ovn acl add <switch> <direction> <priority> <match> <action>
 * RPC: ovn.acl.list / ovn.acl.add
 */
void cmd_ovn_acl(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage:\n"
               "  pcvctl ovn acl list <switch>\n"
               "  pcvctl ovn acl add <switch> <direction> <priority> <match> <action>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "list") == 0) {
        if (argc < 4) { printf("%sUsage: pcvctl ovn acl list <switch>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "switch", argv[3]);
        GError *error = NULL;
        gchar  *resp  = purectl_send_request("ovn.acl.list", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (!resp) return;
        if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
            g_object_unref(parser); g_free(resp); return;
        }
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonArray  *arr  = json_object_has_member(root, "result")
                ? json_object_get_array_member(root, "result") : NULL;

        if (arr && (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV)) {
            const char *hdrs[] = {"DIRECTION","PRIORITY","MATCH","ACTION"};
            PcvTable   *t      = ptbl_new(hdrs, 4);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                gchar prio_buf[16];
                g_snprintf(prio_buf, sizeof(prio_buf), "%lld",
                    (long long)json_object_get_int_member_with_default(o, "priority", 0));
                ptbl_row(t,
                    json_object_get_string_member_with_default(o, "direction", "?"),
                    prio_buf,
                    json_object_get_string_member_with_default(o, "match", "?"),
                    json_object_get_string_member_with_default(o, "action", "?"));
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else if (arr) {
            printf("%s%-12s │ %-8s │ %-30s │ %-10s%s\n", cc(CYBER_CYAN),
                   "DIRECTION", "PRIORITY", "MATCH", "ACTION", cc(CYBER_RESET));
            printf("────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                printf(" %-12s │ %8lld │ %-30s │ %-10s\n",
                    json_object_get_string_member_with_default(o, "direction", "?"),
                    (long long)json_object_get_int_member_with_default(o, "priority", 0),
                    json_object_get_string_member_with_default(o, "match", "?"),
                    json_object_get_string_member_with_default(o, "action", "?"));
            }
            printf("────────────────────────────────────────────────────────────────────\n");
        }
        g_object_unref(parser); g_free(resp);

    } else if (g_strcmp0(action, "add") == 0) {
        if (argc < 8) {
            printf("%sUsage: pcvctl ovn acl add <switch> <direction> <priority> <match> <action>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "switch_name", argv[3]);
        json_object_set_string_member(params, "direction", argv[4]);
        json_object_set_int_member(params, "priority", atoi(argv[5]));
        json_object_set_string_member(params, "match", argv[6]);
        json_object_set_string_member(params, "action", argv[7]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.acl.add", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_ACL_ADD"); g_free(resp);

    } else {
        printf("%s[!] UNKNOWN OVN ACL ACTION: %s%s\n", cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/**
 * cmd_ovn_dhcp - OVN DHCP 활성화.
 * Usage: pcvctl ovn dhcp enable <switch> <cidr>
 * RPC: ovn.dhcp.enable {switch_name, cidr}
 */
void cmd_ovn_dhcp(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl ovn dhcp enable <switch> <cidr>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *action = argv[2];

    if (g_strcmp0(action, "enable") == 0) {
        if (argc < 5) {
            printf("%sUsage: pcvctl ovn dhcp enable <switch> <cidr>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "switch_name", argv[3]);
        json_object_set_string_member(params, "cidr", argv[4]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("ovn.dhcp.enable", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "OVN_DHCP_ENABLE"); g_free(resp);

    } else {
        printf("%s[!] UNKNOWN OVN DHCP ACTION: %s%s\n", cc(CYBER_RED), action, cc(CYBER_RESET));
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  P3: 네트워크 편집 명령
 * ════════════════════════════════════════════════════════════════════ */

/**
 * cmd_net_edit - 브릿지 모드 변경.
 * Usage: pcvctl network edit <bridge> --mode MODE
 * RPC: network.mode_set {bridge_name, mode}
 */
void cmd_net_edit(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl network edit <bridge> --mode MODE%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const char *bridge = argv[2];
    const char *mode   = NULL;
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mode") == 0 && i+1 < argc)
            mode = argv[++i];
    }
    if (!mode) {
        printf("%sUsage: pcvctl network edit <bridge> --mode MODE%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "bridge_name", bridge);
    json_object_set_string_member(params, "mode", mode);
    GError *error = NULL;
    gchar *resp = purectl_send_request("network.mode_set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NET_MODE_SET"); g_free(resp);
}

/**
 * cmd_net_dhcp - 브릿지 DHCP 토글.
 * Usage: pcvctl network dhcp <bridge> --enable/--disable
 * RPC: network.dhcp_toggle {bridge_name, action: "start"/"stop"}
 */
void cmd_net_dhcp(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl network dhcp <bridge> --enable|--disable%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const char *bridge  = argv[2];
    const char *dhcp_action = NULL;
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--enable") == 0)
            dhcp_action = "start";
        else if (g_strcmp0(argv[i], "--disable") == 0)
            dhcp_action = "stop";
    }
    if (!dhcp_action) {
        printf("%sUsage: pcvctl network dhcp <bridge> --enable|--disable%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "bridge_name", bridge);
    json_object_set_string_member(params, "action", dhcp_action);
    GError *error = NULL;
    gchar *resp = purectl_send_request("network.dhcp_toggle", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NET_DHCP_TOGGLE"); g_free(resp);
}

/**
 * cmd_net_bind - 브릿지에 물리 NIC 바인딩.
 * Usage: pcvctl network bind <bridge> <nic>
 * RPC: network.bind_phys {bridge_name, physical_if}
 */
void cmd_net_bind(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl network bind <bridge> <nic>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "bridge_name", argv[2]);
    json_object_set_string_member(params, "physical_if", argv[3]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("network.bind_phys", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "NET_BIND_PHYS"); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  Phase 2 + Phase 3 CLI 핸들러 (29개 RPC)
 * ════════════════════════════════════════════════════════════════════ */

/* ── Phase 2: VM Operations ── */

/** cmd_vm_autostart - VM 자동시작 설정. vm.autostart RPC. */
void cmd_vm_autostart(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm autostart <name> --enable/--disable%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    gboolean enable = TRUE;
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--disable") == 0) enable = FALSE;
        else if (g_strcmp0(argv[i], "--enable") == 0) enable = TRUE;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_boolean_member(params, "enable", enable);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.autostart", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "VM_AUTOSTART"); g_free(resp);
}

/** cmd_vm_disk_throttle - 디스크 I/O 스로틀링. vm.blkio.set RPC. */
void cmd_vm_disk_throttle(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm disk-throttle <name> [--device vda] --read-iops N --write-iops N%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "device", "vda");
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--device") == 0 && i+1 < argc)
            json_object_set_string_member(params, "device", argv[++i]);
        else if (g_strcmp0(argv[i], "--read-iops") == 0 && i+1 < argc)
            json_object_set_int_member(params, "read_iops_sec", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--write-iops") == 0 && i+1 < argc)
            json_object_set_int_member(params, "write_iops_sec", atoi(argv[++i]));
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.blkio.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "DISK_THROTTLE"); g_free(resp);
}

/** cmd_vm_bandwidth - VM 네트워크 대역폭 제한. vm.set_bandwidth RPC. */
void cmd_vm_bandwidth(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm bandwidth <name> --inbound-kbps N --outbound-kbps N%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    gboolean has_limit = FALSE;
    for (int i = 3; i < argc; i++) {
        if ((g_strcmp0(argv[i], "--inbound-kbps") == 0 ||
             g_strcmp0(argv[i], "--inbound") == 0) && i+1 < argc) {
            json_object_set_int_member(params, "inbound_kbps", atoi(argv[++i]));
            has_limit = TRUE;
        } else if ((g_strcmp0(argv[i], "--outbound-kbps") == 0 ||
                    g_strcmp0(argv[i], "--outbound") == 0 ||
                    g_strcmp0(argv[i], "--rate") == 0) && i+1 < argc) {
            json_object_set_int_member(params, "outbound_kbps", atoi(argv[++i]));
            has_limit = TRUE;
        }
    }
    if (!has_limit) {
        printf("%sUsage: pcvctl vm bandwidth <name> --inbound-kbps N --outbound-kbps N%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        json_object_unref(params);
        return;
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.set_bandwidth", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "VM_BANDWIDTH"); g_free(resp);
}

/** cmd_vm_numa_info - NUMA 토폴로지 조회. vm.numa.info RPC. */
void cmd_vm_numa_info(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.numa.info", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_vm_sla - VM SLA 리포트. vm.sla.report RPC. */
void cmd_vm_sla(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm sla <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.sla.report", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_vm_schedule - VM 스케줄 설정/조회. vm.schedule.set / vm.schedule.list RPC. */
void cmd_vm_schedule(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl vm schedule set <name> --start \"cron\" --stop \"cron\"\n"
               "  pcvctl vm schedule list%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *sub = argv[2];
    if (g_strcmp0(sub, "list") == 0) {
        GError *error = NULL;
        gchar *resp = purectl_send_request("vm.schedule.list", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (resp) { print_raw_response(resp); g_free(resp); }
    } else if (g_strcmp0(sub, "set") == 0) {
        if (argc < 4) {
            printf("%sUsage: pcvctl vm schedule set <name> --start \"cron\" --stop \"cron\"%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        for (int i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--start") == 0 && i+1 < argc)
                json_object_set_string_member(params, "start_cron", argv[++i]);
            else if (g_strcmp0(argv[i], "--stop") == 0 && i+1 < argc)
                json_object_set_string_member(params, "stop_cron", argv[++i]);
        }
        GError *error = NULL;
        gchar *resp = purectl_send_request("vm.schedule.set", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "VM_SCHEDULE_SET"); g_free(resp);
    } else {
        printf("%sUnknown sub-command: %s%s\n", cc(CYBER_RED), sub, cc(CYBER_RESET));
    }
}

/* ── Phase 2: Monitoring ── */

/** cmd_storage_health - 스토리지 풀 헬스. storage.pool.health RPC. */
void cmd_storage_health(int argc, char *argv[]) {
    JsonObject *params = json_object_new();
    if (argc >= 3)
        json_object_set_string_member(params, "pool", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("storage.pool.health", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_capacity_forecast - 용량 예측. capacity.forecast RPC. */
void cmd_capacity_forecast(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("capacity.forecast", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_billing_report - VM 빌링 리포트. vm.billing.report RPC. */
void cmd_billing_report(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.billing.report", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ── Phase 2: Advanced ── */

/** cmd_job_list - 비동기 작업 목록. job.list RPC. */
void cmd_job_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("jobs.list", NULL, &error);  /* [감사 AF-C2] 오타 수정: 백엔드 등록명은 복수형 jobs.list */
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_batch_execute - 다중 VM에 whitelist action 팬아웃 실행. vm.batch RPC.
 *  action whitelist(서버 실측)는 start/stop — 그 외는 -32602 "unsupported batch action"
 *  으로 거부되며, 그 에러를 그대로 사용자에게 노출한다. */
void cmd_batch_execute(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm batch <start|stop> <vm1> <vm2> ...\n"
               "  action: start | stop (server-whitelisted)%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p   = json_object_new();
    JsonArray  *vms = json_array_new();
    json_object_set_string_member(p, "action", argv[2]);
    for (int i = 3; i < argc; i++)
        json_array_add_string_element(vms, argv[i]);
    json_object_set_array_member(p, "vms", vms);

    GError *e = NULL;
    gchar  *r = purectl_send_request("vm.batch", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n",ce(CYBER_RED),e->message,ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) { g_printerr("%s[!] NULL RESPONSE%s\n", ce(CYBER_RED), ce(CYBER_RESET)); return; }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, r, -1, NULL) && json_parser_get_root(parser)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *err_obj = json_object_get_object_member(root, "error");
            g_printerr("%s[!] COMMAND REJECTED [%lld]: %s%s\n",
                ce(CYBER_RED),
                (long long)json_object_get_int_member(err_obj, "code"),
                json_object_get_string_member(err_obj, "message"),
                ce(CYBER_RESET));
        } else if (json_object_has_member(root, "result")) {
            JsonObject *res      = json_object_get_object_member(root, "result");
            JsonArray  *accepted = json_object_has_member(res, "accepted")
                    ? json_object_get_array_member(res, "accepted") : NULL;
            JsonArray  *rejected = json_object_has_member(res, "rejected")
                    ? json_object_get_array_member(res, "rejected") : NULL;
            guint na = accepted ? json_array_get_length(accepted) : 0;
            guint nr = rejected ? json_array_get_length(rejected) : 0;

            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                printf("%s\t%u\t%u\n", argv[2], na, nr);
            } else {
                printf("%s%s[+] BATCH %s: accepted=%u rejected=%u%s\n",
                    cc(CYBER_GREEN), cc(CYBER_BOLD), argv[2], na, nr, cc(CYBER_RESET));
                for (guint i = 0; i < na; i++)
                    printf("  %s[+] %s%s\n", cc(CYBER_GREEN),
                        json_array_get_string_element(accepted, i), cc(CYBER_RESET));
                for (guint i = 0; i < nr; i++) {
                    JsonObject *rj = json_array_get_object_element(rejected, i);
                    printf("  %s[!] %s: %s%s\n", cc(CYBER_RED),
                        json_object_get_string_member(rj, "vm"),
                        json_object_get_string_member(rj, "reason"),
                        cc(CYBER_RESET));
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(r);
}

/** cmd_prometheus_sd - Prometheus 서비스 디스커버리. prometheus.sd RPC. */
void cmd_prometheus_sd(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("prometheus.sd", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_webhook_list - 이벤트 Webhook 목록. vm.event.webhook.list RPC. */
void cmd_webhook_list(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.event.webhook.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_alert_actions - 알림 액션 목록. alert.action.list RPC. */
void cmd_alert_actions(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("alert.action.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ── Phase 3: Audit ── */

/**
 * cmd_audit_search - 감사 로그 검색. audit.search RPC.
 * Usage: pcvctl audit search [--user U] [--from TS] [--to TS] [--method M] [--limit N]
 */
void cmd_audit_search(int argc, char *argv[]) {
    JsonObject *params = json_object_new();
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--user") == 0 && i+1 < argc)
            json_object_set_string_member(params, "username", argv[++i]);
        else if (g_strcmp0(argv[i], "--from") == 0 && i+1 < argc)
            json_object_set_string_member(params, "from_ts", argv[++i]);
        else if (g_strcmp0(argv[i], "--to") == 0 && i+1 < argc)
            json_object_set_string_member(params, "to_ts", argv[++i]);
        else if (g_strcmp0(argv[i], "--method") == 0 && i+1 < argc)
            json_object_set_string_member(params, "method", argv[++i]);
        else if (g_strcmp0(argv[i], "--limit") == 0 && i+1 < argc)
            json_object_set_int_member(params, "limit", atoi(argv[++i]));
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("audit.search", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ── Native Host HIDS/HIPS Security Guard ── */

static void security_usage(void) {
    printf("%sUsage:\n"
           "  pcvctl security status\n"
           "  pcvctl security events [--limit N] [--severity info|warn|crit] [--status open|resolved|suppressed]\n"
           "  pcvctl security event <event_id>\n"
           "  pcvctl security pending\n"
           "  pcvctl security approve <event_id>\n"
           "  pcvctl security dismiss <event_id> [--reason TEXT]\n"
           "  pcvctl security baseline-status\n"
           "  pcvctl security baseline-refresh --path PATH [--path PATH...]\n"
           "  pcvctl security enable\n"
           "  pcvctl security disable%s\n",
        cc(CYBER_YELLOW), cc(CYBER_RESET));
}

static JsonObject *security_parse_root(const gchar *resp, JsonParser **out_parser) {
    *out_parser = NULL;
    if (!resp) {
        g_printerr("%s[!] NULL RESPONSE%s\n", ce(CYBER_RED), ce(CYBER_RESET));
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, resp, -1, &error) ||
        !json_parser_get_root(parser) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_printerr("%s[!] JSON parse failed: %s%s\n",
            ce(CYBER_RED), error ? error->message : "invalid response", ce(CYBER_RESET));
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }

    *out_parser = parser;
    return json_node_get_object(json_parser_get_root(parser));
}

static gboolean security_print_error_if_any(JsonObject *root) {
    if (!root || !json_object_has_member(root, "error")) {
        return FALSE;
    }
    JsonObject *err = json_object_get_object_member(root, "error");
    g_printerr("%s[!] SECURITY RPC ERROR [%lld]: %s%s\n",
        ce(CYBER_RED),
        (long long)json_object_get_int_member_with_default(err, "code", -1),
        json_object_get_string_member_with_default(err, "message", "unknown error"),
        ce(CYBER_RESET));
    return TRUE;
}

static gchar *security_int_string(JsonObject *obj, const gchar *key) {
    return g_strdup_printf("%lld",
        (long long)json_object_get_int_member_with_default(obj, key, 0));
}

static gchar *security_node_to_string(JsonNode *node) {
    if (!node) {
        return g_strdup("");
    }
    if (JSON_NODE_HOLDS_VALUE(node)) {
        GType t = json_node_get_value_type(node);
        if (t == G_TYPE_STRING) {
            return g_strdup(json_node_get_string(node));
        }
        if (t == G_TYPE_BOOLEAN) {
            return g_strdup(json_node_get_boolean(node) ? "true" : "false");
        }
    }
    return json_to_string(node, FALSE);
}

static void security_print_kv_object(JsonObject *obj, const char *title) {
    if (!obj) {
        return;
    }

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdr[] = {"KEY", "VALUE"};
        PcvTable *t = ptbl_new(hdr, 2);
        GList *keys = json_object_get_members(obj);
        for (GList *it = keys; it; it = it->next) {
            const gchar *key = it->data;
            JsonNode *node = json_object_get_member(obj, key);
            g_autofree gchar *value = security_node_to_string(node);
            ptbl_row(t, key, value ? value : "");
        }
        g_list_free(keys);
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        return;
    }

    printf("%s%s%s\n", cc(CYBER_BOLD), title, cc(CYBER_RESET));
    GList *keys = json_object_get_members(obj);
    for (GList *it = keys; it; it = it->next) {
        const gchar *key = it->data;
        JsonNode *node = json_object_get_member(obj, key);
        g_autofree gchar *value = security_node_to_string(node);
        printf("  %s%-18s%s %s\n", cc(CYBER_YELLOW), key, cc(CYBER_RESET),
               value ? value : "");
    }
    g_list_free(keys);
}

static void security_print_config_response(const gchar *resp) {
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(resp);
        return;
    }

    JsonParser *parser = NULL;
    JsonObject *root = security_parse_root(resp, &parser);
    if (!root || security_print_error_if_any(root)) {
        if (parser) g_object_unref(parser);
        return;
    }
    JsonObject *res = json_object_get_object_member(root, "result");
    if (!res) {
        if (parser) g_object_unref(parser);
        return;
    }

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdr[] = {"ENABLED", "BASELINE", "OPEN_RISK", "PENDING", "DEGRADED"};
        PcvTable *t = ptbl_new(hdr, 5);
        g_autofree gchar *risk = security_int_string(res, "open_risk");
        g_autofree gchar *pending = security_int_string(res, "pending_actions");
        ptbl_row(t,
            json_object_get_boolean_member_with_default(res, "enabled", FALSE) ? "true" : "false",
            json_object_get_string_member_with_default(res, "baseline_status", "unknown"),
            risk,
            pending,
            json_object_get_boolean_member_with_default(res, "degraded", FALSE) ? "true" : "false");
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
    } else {
        printf("%s%sSecurity Guard%s\n", cc(CYBER_BOLD), cc(CYBER_CYAN), cc(CYBER_RESET));
        printf("  enabled         : %s%s%s\n",
            json_object_get_boolean_member_with_default(res, "enabled", FALSE) ? cc(CYBER_GREEN) : cc(CYBER_RED),
            json_object_get_boolean_member_with_default(res, "enabled", FALSE) ? "true" : "false",
            cc(CYBER_RESET));
        printf("  baseline_status : %s%s%s\n", cc(CYBER_YELLOW),
            json_object_get_string_member_with_default(res, "baseline_status", "unknown"),
            cc(CYBER_RESET));
        printf("  open_risk       : %lld\n",
            (long long)json_object_get_int_member_with_default(res, "open_risk", 0));
        printf("  pending_actions : %lld\n",
            (long long)json_object_get_int_member_with_default(res, "pending_actions", 0));
        printf("  degraded        : %s\n",
            json_object_get_boolean_member_with_default(res, "degraded", FALSE) ? "true" : "false");
    }
    g_object_unref(parser);
}

static void security_print_array_response(const gchar *resp, gboolean actions) {
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(resp);
        return;
    }

    JsonParser *parser = NULL;
    JsonObject *root = security_parse_root(resp, &parser);
    if (!root || security_print_error_if_any(root)) {
        if (parser) g_object_unref(parser);
        return;
    }
    JsonNode *node = json_object_get_member(root, "result");
    JsonArray *arr = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    guint len = arr ? json_array_get_length(arr) : 0;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *event_hdr[] = {"EVENT_ID", "SEVERITY", "STATUS", "SOURCE", "TARGET", "ACTION", "SUMMARY"};
        const char *action_hdr[] = {"EVENT_ID", "ACTION", "TARGET_KIND", "TARGET", "STATUS"};
        PcvTable *t = actions ? ptbl_new(action_hdr, 5) : ptbl_new(event_hdr, 7);
        for (guint i = 0; i < len; i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            if (actions) {
                ptbl_row(t,
                    json_object_get_string_member_with_default(o, "event_id", "-"),
                    json_object_get_string_member_with_default(o, "action", "-"),
                    json_object_get_string_member_with_default(o, "target_kind", "-"),
                    json_object_get_string_member_with_default(o, "target", "-"),
                    json_object_get_string_member_with_default(o, "status", "-"));
            } else {
                ptbl_row(t,
                    json_object_get_string_member_with_default(o, "event_id", "-"),
                    json_object_get_string_member_with_default(o, "severity", "-"),
                    json_object_get_string_member_with_default(o, "status", "-"),
                    json_object_get_string_member_with_default(o, "source", "-"),
                    json_object_get_string_member_with_default(o, "target", "-"),
                    json_object_get_string_member_with_default(o, "recommended_action", "-"),
                    json_object_get_string_member_with_default(o, "summary", "-"));
            }
        }
        g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
        ptbl_free(t);
        g_object_unref(parser);
        return;
    }

    printf("%s%s%s\n", cc(CYBER_BOLD),
        actions ? "Security Guard Pending Actions" : "Security Events",
        cc(CYBER_RESET));
    if (len == 0) {
        printf("  (none)\n");
    }
    for (guint i = 0; i < len; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (actions) {
            printf("  %s%-28s%s %-12s %-10s %s\n",
                cc(CYBER_YELLOW), json_object_get_string_member_with_default(o, "event_id", "-"),
                cc(CYBER_RESET),
                json_object_get_string_member_with_default(o, "action", "-"),
                json_object_get_string_member_with_default(o, "status", "-"),
                json_object_get_string_member_with_default(o, "target", "-"));
        } else {
            printf("  %s%-28s%s %-5s %-10s %-14s %s\n",
                cc(CYBER_YELLOW), json_object_get_string_member_with_default(o, "event_id", "-"),
                cc(CYBER_RESET),
                json_object_get_string_member_with_default(o, "severity", "-"),
                json_object_get_string_member_with_default(o, "status", "-"),
                json_object_get_string_member_with_default(o, "recommended_action", "-"),
                json_object_get_string_member_with_default(o, "summary", "-"));
        }
    }
    g_object_unref(parser);
}

static void security_print_object_response(const gchar *resp, const char *title) {
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(resp);
        return;
    }

    JsonParser *parser = NULL;
    JsonObject *root = security_parse_root(resp, &parser);
    if (!root || security_print_error_if_any(root)) {
        if (parser) g_object_unref(parser);
        return;
    }
    JsonObject *res = json_object_get_object_member(root, "result");
    security_print_kv_object(res, title);
    g_object_unref(parser);
}

static gchar *security_request(const gchar *method, JsonObject *params) {
    GError *error = NULL;
    gchar *resp = purectl_send_request(method, params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return NULL;
    }
    return resp;
}

void cmd_security(int argc, char *argv[]) {
    if (argc < 2) {
        security_usage();
        return;
    }

    const gchar *sub = argv[1];
    if (g_strcmp0(sub, "status") == 0) {
        gchar *resp = security_request("security.config.get", NULL);
        if (resp) { security_print_config_response(resp); g_free(resp); }
    } else if (g_strcmp0(sub, "events") == 0) {
        JsonObject *params = json_object_new();
        json_object_set_int_member(params, "limit", 20);
        for (int i = 2; i < argc; i++) {
            if (g_strcmp0(argv[i], "--limit") == 0 && i + 1 < argc)
                json_object_set_int_member(params, "limit", atoi(argv[++i]));
            else if (g_strcmp0(argv[i], "--offset") == 0 && i + 1 < argc)
                json_object_set_int_member(params, "offset", atoi(argv[++i]));
            else if (g_strcmp0(argv[i], "--severity") == 0 && i + 1 < argc)
                json_object_set_string_member(params, "severity", argv[++i]);
            else if (g_strcmp0(argv[i], "--source") == 0 && i + 1 < argc)
                json_object_set_string_member(params, "source", argv[++i]);
            else if (g_strcmp0(argv[i], "--status") == 0 && i + 1 < argc)
                json_object_set_string_member(params, "status", argv[++i]);
        }
        gchar *resp = security_request("security.event.list", params);
        if (resp) { security_print_array_response(resp, FALSE); g_free(resp); }
    } else if (g_strcmp0(sub, "event") == 0) {
        if (argc < 3) {
            printf("%sUsage: pcvctl security event <event_id>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "event_id", argv[2]);
        gchar *resp = security_request("security.event.get", params);
        if (resp) { security_print_object_response(resp, "Security Event"); g_free(resp); }
    } else if (g_strcmp0(sub, "pending") == 0) {
        gchar *resp = security_request("security.action.pending", NULL);
        if (resp) { security_print_array_response(resp, TRUE); g_free(resp); }
    } else if (g_strcmp0(sub, "approve") == 0) {
        if (argc < 3) {
            printf("%sUsage: pcvctl security approve <event_id>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "event_id", argv[2]);
        gchar *resp = security_request("security.action.approve", params);
        if (resp) { print_action_response(resp, "SECURITY_APPROVE"); g_free(resp); }
    } else if (g_strcmp0(sub, "dismiss") == 0) {
        if (argc < 3) {
            printf("%sUsage: pcvctl security dismiss <event_id> [--reason TEXT]%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "event_id", argv[2]);
        for (int i = 3; i < argc; i++) {
            if (g_strcmp0(argv[i], "--reason") == 0 && i + 1 < argc)
                json_object_set_string_member(params, "reason", argv[++i]);
        }
        gchar *resp = security_request("security.action.dismiss", params);
        if (resp) { print_action_response(resp, "SECURITY_DISMISS"); g_free(resp); }
    } else if (g_strcmp0(sub, "baseline-status") == 0) {
        gchar *resp = security_request("security.baseline.status", NULL);
        if (resp) { security_print_object_response(resp, "Security Baseline"); g_free(resp); }
    } else if (g_strcmp0(sub, "baseline-refresh") == 0) {
        JsonObject *params = json_object_new();
        JsonArray *paths = json_array_new();
        for (int i = 2; i < argc; i++) {
            if (g_strcmp0(argv[i], "--path") == 0 && i + 1 < argc)
                json_array_add_string_element(paths, argv[++i]);
        }
        if (json_array_get_length(paths) == 0) {
            json_array_unref(paths);
            json_object_unref(params);
            printf("%sUsage: pcvctl security baseline-refresh --path PATH [--path PATH...]%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        json_object_set_array_member(params, "paths", paths);
        gchar *resp = security_request("security.baseline.refresh", params);
        if (resp) { print_action_response(resp, "SECURITY_BASELINE_REFRESH"); g_free(resp); }
    } else if (g_strcmp0(sub, "enable") == 0 || g_strcmp0(sub, "disable") == 0) {
        JsonObject *params = json_object_new();
        json_object_set_boolean_member(params, "enabled", g_strcmp0(sub, "enable") == 0);
        gchar *resp = security_request("security.config.set", params);
        if (resp) { print_action_response(resp, "SECURITY_CONFIG_SET"); g_free(resp); }
    } else {
        printf("%sUnknown security command: %s%s\n", cc(CYBER_RED), sub, cc(CYBER_RESET));
        security_usage();
    }
}

/* ── Phase 3: Cluster Affinity ── */


/* ── Phase 3: Security Groups ── */

/** cmd_secgroup - 보안 그룹 관리. security_group.* RPC.
 * 라우트 테이블에서 object="security-group", action="list/create/delete/rule"로
 * 개별 매칭되므로 argv[1]이 sub-command이다.
 */
void cmd_secgroup(int argc, char *argv[]) {
    if (argc < 2) {
        printf("%sUsage:\n"
               "  pcvctl security-group list\n"
               "  pcvctl security-group create <name>\n"
               "  pcvctl security-group delete <name>\n"
               "  pcvctl security-group rule add <name> --direction ingress|egress --proto tcp --port 80%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *sub = argv[1];

    if (g_strcmp0(sub, "list") == 0) {
        GError *error = NULL;
        gchar *resp = purectl_send_request("security_group.list", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (resp) { print_raw_response(resp); g_free(resp); }

    } else if (g_strcmp0(sub, "create") == 0) {
        if (argc < 3) {
            printf("%sUsage: pcvctl security-group create <name>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[2]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("security_group.create", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "SECGROUP_CREATE"); g_free(resp);

    } else if (g_strcmp0(sub, "delete") == 0) {
        if (argc < 3) {
            printf("%sUsage: pcvctl security-group delete <name>%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[2]);
        GError *error = NULL;
        gchar *resp = purectl_send_request("security_group.delete", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "SECGROUP_DELETE"); g_free(resp);

    } else if (g_strcmp0(sub, "rule") == 0) {
        if (argc < 4 || g_strcmp0(argv[2], "add") != 0) {
            printf("%sUsage: pcvctl security-group rule add <name> --direction ingress|egress (별칭 in/out) --proto tcp --port 80%s\n",
                cc(CYBER_YELLOW), cc(CYBER_RESET));
            return;
        }
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", argv[3]);
        for (int i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--direction") == 0 && i+1 < argc) {
                /* VP-7: 데몬 계약은 ingress/egress — 사용자 친화 별칭을 정규화.
                 * 미인식 값은 그대로 전달해 데몬이 거부·로그하게 둔다. */
                const gchar *dir = argv[++i];
                if (g_strcmp0(dir, "in") == 0 || g_strcmp0(dir, "inbound") == 0)
                    dir = "ingress";
                else if (g_strcmp0(dir, "out") == 0 || g_strcmp0(dir, "outbound") == 0)
                    dir = "egress";
                json_object_set_string_member(params, "direction", dir);
            }
            else if (g_strcmp0(argv[i], "--proto") == 0 && i+1 < argc)
                json_object_set_string_member(params, "proto", argv[++i]);
            else if (g_strcmp0(argv[i], "--port") == 0 && i+1 < argc)
                json_object_set_int_member(params, "port", atoi(argv[++i]));
        }
        GError *error = NULL;
        gchar *resp = purectl_send_request("security_group.rule.add", params, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "SECGROUP_RULE_ADD"); g_free(resp);
    } else {
        printf("%sUnknown sub-command: %s%s\n", cc(CYBER_RED), sub, cc(CYBER_RESET));
    }
}

/** cmd_vm_secgroup - VM에 보안 그룹 할당. vm.security_group.set RPC. */
void cmd_vm_secgroup(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm security-group <vm> <sg>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    json_object_set_string_member(params, "group_name", argv[3]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.security_group.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "VM_SECGROUP_SET"); g_free(resp);
}

/* ── Phase 3: Webhook DLQ ── */

/** cmd_webhook_dlq - Webhook DLQ 관리. webhook.dlq.list / webhook.dlq.retry RPC. */
void cmd_webhook_dlq(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage:\n"
               "  pcvctl webhook dlq list\n"
               "  pcvctl webhook dlq retry%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    const gchar *sub = argv[2];
    if (g_strcmp0(sub, "list") == 0) {
        GError *error = NULL;
        gchar *resp = purectl_send_request("webhook.dlq.list", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        if (resp) { print_raw_response(resp); g_free(resp); }
    } else if (g_strcmp0(sub, "retry") == 0) {
        GError *error = NULL;
        gchar *resp = purectl_send_request("webhook.dlq.retry", NULL, &error);
        if (error) {
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
            g_error_free(error); return;
        }
        print_action_response(resp, "WEBHOOK_DLQ_RETRY"); g_free(resp);
    } else {
        printf("%sUnknown sub-command: %s%s\n", cc(CYBER_RED), sub, cc(CYBER_RESET));
    }
}

/* ── Phase 3: GPU Metrics ── */

/** cmd_gpu_metrics - GPU 메트릭 조회. gpu.metrics RPC. */
void cmd_gpu_metrics(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("gpu.metrics", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ── Phase 3: Config History/Backup ── */

/** cmd_config_history - 설정 변경 이력 조회. config.history RPC. */
void cmd_config_history(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("config.history", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/** cmd_config_backup - 설정 백업. config.backup RPC. */
void cmd_config_backup(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("config.backup", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    print_action_response(resp, "CONFIG_BACKUP"); g_free(resp);
}

/* ── Config Validate (client-side, no daemon needed) ── */

/**
 * cmd_config_validate - daemon.conf 설정 파일의 유효성을 검증합니다.
 * 데몬 연결 없이 클라이언트에서 직접 설정 파일을 읽어 검증합니다.
 *
 * [검증 항목]
 *   1. rest_port: 1-65535 범위
 *   2. socket_path: 상위 디렉터리 존재 여부
 *   3. TLS 인증서: 파일 존재 여부
 *   4. etcd_endpoints: 노드 수 (3개 권장)
 *   5. drain_timeout: >= 5초
 *   6. pool_max_conn: 1-64 범위
 *   7. image_dir: 디렉터리 존재 여부
 */
void cmd_config_validate(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    const gchar *conf_path = "/etc/purecvisor/daemon.conf";
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    if (!g_key_file_load_from_file(kf, conf_path, G_KEY_FILE_NONE, &err)) {
        printf("%s[FAIL]%s Cannot read %s: %s\n",
               ce(CYBER_RED), ce(CYBER_RESET), conf_path,
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_key_file_free(kf);
        return;
    }

    gint pass = 0, warn = 0, fail = 0;

    /* 1. rest_port (daemon 섹션) */
    {
        gint port = g_key_file_get_integer(kf, "daemon", "rest_port", NULL);
        if (port >= 1 && port <= 65535) {
            printf("%s[OK]  %s rest_port = %d\n", ce(CYBER_GREEN), ce(CYBER_RESET), port);
            pass++;
        } else if (port == 0) {
            printf("%s[OK]  %s rest_port = default (80)\n", ce(CYBER_GREEN), ce(CYBER_RESET));
            pass++;
        } else {
            printf("%s[FAIL]%s rest_port = %d (valid range: 1-65535)\n",
                   ce(CYBER_RED), ce(CYBER_RESET), port);
            fail++;
        }
    }

    /* 2. socket_path */
    {
        gchar *sock = g_key_file_get_string(kf, "daemon", "socket_path", NULL);
        if (sock && *sock) {
            gchar *dir = g_path_get_dirname(sock);
            if (g_file_test(dir, G_FILE_TEST_IS_DIR)) {
                printf("%s[OK]  %s socket_path dir exists: %s\n",
                       ce(CYBER_GREEN), ce(CYBER_RESET), dir);
                pass++;
            } else {
                printf("%s[FAIL]%s socket_path dir missing: %s\n",
                       ce(CYBER_RED), ce(CYBER_RESET), dir);
                fail++;
            }
            g_free(dir);
            g_free(sock);
        } else {
            printf("%s[OK]  %s socket_path = default\n", ce(CYBER_GREEN), ce(CYBER_RESET));
            pass++;
        }
    }

    /* 3. TLS cert_file */
    {
        gboolean tls_on = g_key_file_get_boolean(kf, "tls", "enabled", NULL);
        if (tls_on) {
            gchar *cert = g_key_file_get_string(kf, "tls", "cert_file", NULL);
            if (cert && *cert) {
                if (g_file_test(cert, G_FILE_TEST_EXISTS)) {
                    printf("%s[OK]  %s TLS cert exists: %s\n",
                           ce(CYBER_GREEN), ce(CYBER_RESET), cert);
                    pass++;
                } else {
                    printf("%s[FAIL]%s TLS cert not found: %s\n",
                           ce(CYBER_RED), ce(CYBER_RESET), cert);
                    fail++;
                }
                g_free(cert);
            } else {
                printf("%s[FAIL]%s TLS enabled but cert_file not set\n",
                       ce(CYBER_RED), ce(CYBER_RESET));
                fail++;
            }
            gchar *key = g_key_file_get_string(kf, "tls", "key_file", NULL);
            if (key && *key) {
                if (g_file_test(key, G_FILE_TEST_EXISTS)) {
                    printf("%s[OK]  %s TLS key exists: %s\n",
                           ce(CYBER_GREEN), ce(CYBER_RESET), key);
                    pass++;
                } else {
                    printf("%s[FAIL]%s TLS key not found: %s\n",
                           ce(CYBER_RED), ce(CYBER_RESET), key);
                    fail++;
                }
                g_free(key);
            }
        }
    }

    /* 4. etcd_endpoints */
    {
        gchar *eps = g_key_file_get_string(kf, "cluster", "etcd_endpoints", NULL);
        if (eps && *eps) {
            gchar **parts = g_strsplit(eps, ",", -1);
            gint n = (gint)g_strv_length(parts);
            if (n < 3) {
                printf("%s[WARN]%s etcd_endpoints: only %d node(s) (recommend 3+)\n",
                       ce(CYBER_YELLOW), ce(CYBER_RESET), n);
                warn++;
            } else {
                printf("%s[OK]  %s etcd_endpoints: %d nodes\n",
                       ce(CYBER_GREEN), ce(CYBER_RESET), n);
                pass++;
            }
            g_strfreev(parts);
            g_free(eps);
        }
    }

    /* 5. drain_timeout */
    {
        gint dt = g_key_file_get_integer(kf, "daemon", "drain_timeout", NULL);
        if (dt > 0 && dt < 5) {
            printf("%s[FAIL]%s drain_timeout = %d (minimum 5)\n",
                   ce(CYBER_RED), ce(CYBER_RESET), dt);
            fail++;
        } else if (dt >= 5) {
            printf("%s[OK]  %s drain_timeout = %d\n",
                   ce(CYBER_GREEN), ce(CYBER_RESET), dt);
            pass++;
        }
    }

    /* 6. pool_max_conn */
    {
        gint pm = g_key_file_get_integer(kf, "daemon", "pool_max_conn", NULL);
        if (pm > 0) {
            if (pm >= 1 && pm <= 64) {
                printf("%s[OK]  %s pool_max_conn = %d\n",
                       ce(CYBER_GREEN), ce(CYBER_RESET), pm);
                pass++;
            } else {
                printf("%s[FAIL]%s pool_max_conn = %d (valid range: 1-64)\n",
                       ce(CYBER_RED), ce(CYBER_RESET), pm);
                fail++;
            }
        }
    }

    /* 7. image_dir */
    {
        gchar *img = g_key_file_get_string(kf, "storage", "image_dir", NULL);
        if (img && *img) {
            if (g_file_test(img, G_FILE_TEST_IS_DIR)) {
                printf("%s[OK]  %s image_dir exists: %s\n",
                       ce(CYBER_GREEN), ce(CYBER_RESET), img);
                pass++;
            } else {
                printf("%s[WARN]%s image_dir missing: %s\n",
                       ce(CYBER_YELLOW), ce(CYBER_RESET), img);
                warn++;
            }
            g_free(img);
        }
    }

    g_key_file_free(kf);

    printf("\n%s── Result ──%s  %s%d OK%s  %s%d WARN%s  %s%d FAIL%s\n",
           ce(CYBER_CYAN), ce(CYBER_RESET),
           ce(CYBER_GREEN), pass, ce(CYBER_RESET),
           ce(CYBER_YELLOW), warn, ce(CYBER_RESET),
           ce(CYBER_RED), fail, ce(CYBER_RESET));
}

/* ── Phase 3: Template History ── */

/** cmd_template_history - 템플릿 변경 이력 조회. template.history RPC. */
void cmd_template_history(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("template.history", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (resp) { print_raw_response(resp); g_free(resp); }
}

/* ── gRPC 관리 ── */

/**
 * cmd_grpc_status - gRPC 서버 상태 확인.
 *
 * TCP 50051 포트 연결을 시도하여 gRPC 서버 활성 여부를 확인한다.
 * 연결 성공 시 ACTIVE, 실패 시 DISABLED 출력.
 */
static void cmd_grpc_status(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        g_printerr("%s[!] socket() failed%s\n", ce(CYBER_RED), ce(CYBER_RESET));
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(50051);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        printf("%s gRPC SERVER: ACTIVE (port 50051)%s\n", cc(CYBER_GREEN), cc(CYBER_RESET));
        printf(" Protocol: protobuf-c binary framing\n");
        printf(" Transport: TCP (HTTP/2 planned)\n");
    } else {
        printf("%s gRPC SERVER: DISABLED%s\n", cc(CYBER_RED), cc(CYBER_RESET));
        printf(" Enable: daemon.conf [grpc] enabled=true\n");
    }
    close(fd);
}

/**
 * cmd_grpc_test - gRPC 연결 테스트.
 *
 * gRPC 포트(50051)에 연결하여 daemon.version 요청을 전송한다.
 * 바이너리 프레이밍: [4B method_len][method][4B payload_len][payload]
 * 응답: [4B resp_len][json]
 */
static void cmd_grpc_test(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        g_printerr("%s[!] socket() failed%s\n", ce(CYBER_RED), ce(CYBER_RESET));
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(50051);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_printerr("%s[!] gRPC server not reachable (port 50051)%s\n",
                   ce(CYBER_RED), ce(CYBER_RESET));
        g_printerr("    Enable: daemon.conf [grpc] enabled=true\n");
        close(fd);
        return;
    }

    /* Send: [4B method_len][method][4B payload_len][payload] */
    const char *method = "/purecvisor.v1.SystemService/Version";
    const char *payload = "{}";
    uint32_t method_len = (uint32_t)strlen(method);
    uint32_t payload_len = (uint32_t)strlen(payload);
    uint32_t net_method_len = htonl(method_len);
    uint32_t net_payload_len = htonl(payload_len);

    ssize_t written = 0;
    written += write(fd, &net_method_len, 4);
    written += write(fd, method, method_len);
    written += write(fd, &net_payload_len, 4);
    written += write(fd, payload, payload_len);

    if (written < (ssize_t)(8 + method_len + payload_len)) {
        g_printerr("%s[!] Failed to send gRPC request%s\n",
                   ce(CYBER_RED), ce(CYBER_RESET));
        close(fd);
        return;
    }

    /* Read: [4B resp_len][json response] */
    uint32_t resp_len_net = 0;
    ssize_t rd = read(fd, &resp_len_net, 4);
    if (rd == 4) {
        uint32_t resp_len = ntohl(resp_len_net);
        if (resp_len > 0 && resp_len < 65536) {
            char *buf = g_malloc0(resp_len + 1);
            ssize_t total = 0;
            while (total < (ssize_t)resp_len) {
                ssize_t n = read(fd, buf + total, resp_len - (uint32_t)total);
                if (n <= 0) break;
                total += n;
            }
            buf[total] = '\0';
            printf("%s gRPC Response:%s\n%s\n", cc(CYBER_GREEN), cc(CYBER_RESET), buf);
            g_free(buf);
        } else {
            printf("%s gRPC: Connected but unexpected response length: %u%s\n",
                   cc(CYBER_YELLOW), resp_len, cc(CYBER_RESET));
        }
    } else {
        printf("%s gRPC: Connected but no response (server may not support this framing)%s\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf(" Connection to port 50051 succeeded — gRPC server is running.\n");
    }
    close(fd);
}

/* ════════════════════════════════════════════════════════════════════
 *  라우팅 테이블
 *
 *  CLI 명령 디스패치의 핵심 자료구조.
 *  사용자 입력 "pcvctl <object> <action> [args]"에서
 *  object+action 조합으로 핸들러 함수를 찾아 호출한다.
 *
 *  구조:
 *    { object, action, handler_func, help_text }
 *
 *  동작 흐름:
 *    1. main() → route_exec() 호출
 *    2. route_exec()가 routes[] 순회하며 object+action 매칭
 *    3. 매칭된 handler(argc, argv) 호출
 *    4. 미매칭 시 에러 출력 + print_help(object)
 *
 *  readline 자동완성: build_completions()가 routes[]에서
 *  "object action" 문자열을 동적 생성하여 Tab 완성 지원.
 * ════════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════════
 * Cloud Migration CLI Commands
 * ════════════════════════════════════════════════════════════════════ */

/**
 * pcvctl cloud import --ami ami-xxx --name web-prod [--region ap-northeast-2]
 *   [--bucket pcv-migration] [--vcpu 4] [--memory 8192] [--bridge pcvbr0]
 *   [--mode near-live]
 */
static void cmd_cloud_import(int argc, char *argv[]) {
    const char *name = NULL, *ami = NULL, *region = "ap-northeast-2";
    const char *bucket = "", *bridge = "pcvbr0", *mode = NULL;
    int vcpu = 2, mem = 2048;
    for (int i = 2; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--ami") && i+1 < argc) ami = argv[++i];
        else if (g_str_has_prefix(argv[i], "--name") && i+1 < argc) name = argv[++i];
        else if (g_str_has_prefix(argv[i], "--region") && i+1 < argc) region = argv[++i];
        else if (g_str_has_prefix(argv[i], "--bucket") && i+1 < argc) bucket = argv[++i];
        else if (g_str_has_prefix(argv[i], "--vcpu") && i+1 < argc) vcpu = atoi(argv[++i]);
        else if (g_str_has_prefix(argv[i], "--memory") && i+1 < argc) mem = atoi(argv[++i]);
        else if (g_str_has_prefix(argv[i], "--bridge") && i+1 < argc) bridge = argv[++i];
        else if (g_strcmp0(argv[i], "--mode") == 0 && i + 1 < argc) mode = argv[++i];
    }
    if (!name || !ami) {
        printf("%sUsage: pcvctl cloud import --ami <ami-id> --name <vm-name>%s\n"
               "  --region <aws-region>    (default: ap-northeast-2)\n"
               "  --bucket <s3-bucket>     (default: daemon.conf)\n"
               "  --vcpu <count>           (default: 2)\n"
               "  --memory <mb>            (default: 2048)\n"
               "  --bridge <bridge>        (default: pcvbr0)\n"
               "  --mode <standard|near-live>  (default: standard)\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    json_object_set_string_member(params, "ami_id", ami);
    json_object_set_string_member(params, "aws_region", region);
    if (bucket[0]) json_object_set_string_member(params, "s3_bucket", bucket);
    json_object_set_int_member(params, "vcpu", vcpu);
    json_object_set_int_member(params, "memory_mb", mem);
    json_object_set_string_member(params, "network_bridge", bridge);
    if (mode) json_object_set_string_member(params, "mode", mode);

    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.import.ec2", params, &error);
    if (error) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET)); g_error_free(error); return; }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonObject *r = json_object_has_member(root, "result")
            ? json_object_get_object_member(root, "result") : root;
        printf("%s[+] Import started%s\n", cc(CYBER_GREEN), cc(CYBER_RESET));
        printf("  VM:     %s%s%s\n", cc(CYBER_CYAN), name, cc(CYBER_RESET));
        printf("  AMI:    %s\n", ami);
        printf("  Job ID: %s%s%s\n", cc(CYBER_YELLOW),
            json_object_has_member(r, "job_id") ? json_object_get_string_member(r, "job_id") : "?",
            cc(CYBER_RESET));
        printf("  Track:  pcvctl cloud status --name %s\n", name);
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * pcvctl cloud export --name web-prod [--region ap-northeast-2] [--bucket pcv-migration]
 *   [--ami-name web-exported] [--description "..."]
 */
static void cmd_cloud_export(int argc, char *argv[]) {
    const char *name = NULL, *region = "ap-northeast-2", *bucket = "";
    const char *ami_name = "", *desc = "";
    for (int i = 2; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--name") && i+1 < argc) name = argv[++i];
        else if (g_str_has_prefix(argv[i], "--region") && i+1 < argc) region = argv[++i];
        else if (g_str_has_prefix(argv[i], "--bucket") && i+1 < argc) bucket = argv[++i];
        else if (g_str_has_prefix(argv[i], "--ami-name") && i+1 < argc) ami_name = argv[++i];
        else if (g_str_has_prefix(argv[i], "--description") && i+1 < argc) desc = argv[++i];
    }
    if (!name) {
        printf("%sUsage: pcvctl cloud export --name <vm-name>%s\n"
               "  --region <aws-region>    (default: ap-northeast-2)\n"
               "  --bucket <s3-bucket>     (default: daemon.conf)\n"
               "  --ami-name <ami-name>    (default: <vm>-exported)\n"
               "  --description <text>\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    json_object_set_string_member(params, "aws_region", region);
    if (bucket[0]) json_object_set_string_member(params, "s3_bucket", bucket);
    if (ami_name[0]) json_object_set_string_member(params, "ami_name", ami_name);
    if (desc[0]) json_object_set_string_member(params, "ami_description", desc);

    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.export.ec2", params, &error);
    if (error) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET)); g_error_free(error); return; }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonObject *r = json_object_has_member(root, "result")
            ? json_object_get_object_member(root, "result") : root;
        printf("%s[+] Export started%s\n", cc(CYBER_GREEN), cc(CYBER_RESET));
        printf("  VM:     %s%s%s\n", cc(CYBER_CYAN), name, cc(CYBER_RESET));
        printf("  Job ID: %s%s%s\n", cc(CYBER_YELLOW),
            json_object_has_member(r, "job_id") ? json_object_get_string_member(r, "job_id") : "?",
            cc(CYBER_RESET));
        printf("  Track:  pcvctl cloud status --name %s\n", name);
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * pcvctl cloud status --name <vm-name>
 */
static void cmd_cloud_status(int argc, char *argv[]) {
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--name") && i+1 < argc) name = argv[++i];
        else if (!g_str_has_prefix(argv[i], "--")) name = argv[i]; /* positional */
    }
    if (!name) {
        printf("%sUsage: pcvctl cloud status --name <vm-name>%s\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);

    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.import.status", params, &error);
    if (error) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET)); g_error_free(error); return; }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        JsonObject *r = json_object_has_member(root, "result")
            ? json_object_get_object_member(root, "result") : root;
        const char *status = json_object_has_member(r, "status")
            ? json_object_get_string_member(r, "status") : "?";
        const char *detail = json_object_has_member(r, "detail")
            ? json_object_get_string_member(r, "detail") : "";
        const char *direction = json_object_has_member(r, "direction")
            ? json_object_get_string_member(r, "direction") : "";
        const char *job_id = json_object_has_member(r, "job_id")
            ? json_object_get_string_member(r, "job_id") : "";
        gint64 progress = json_object_has_member(r, "progress_percent")
            ? json_object_get_int_member(r, "progress_percent") : 0;
        gint64 elapsed = json_object_has_member(r, "elapsed_sec")
            ? json_object_get_int_member(r, "elapsed_sec") : 0;

        const char *sc = (g_strcmp0(status, "done") == 0) ? CYBER_GREEN
                       : (g_strcmp0(status, "failed") == 0) ? CYBER_RED
                       : CYBER_YELLOW;

        printf("%s[%s] %s — %s%s%s\n", cc(CYBER_BOLD), direction, name, cc(sc), status, cc(CYBER_RESET));
        printf("  Job ID:   %s\n", job_id);
        printf("  Progress: %s%" G_GINT64_FORMAT "%%%s\n", cc(CYBER_CYAN), progress, cc(CYBER_RESET));
        printf("  Detail:   %s\n", detail);
        printf("  Elapsed:  %" G_GINT64_FORMAT "s\n", elapsed);

        /* 진행률 바 */
        printf("  [");
        int bar_len = 40;
        int filled = (int)(progress * bar_len / 100);
        for (int i = 0; i < bar_len; i++)
            printf("%s", i < filled ? "█" : "░");
        printf("] %" G_GINT64_FORMAT "%%\n", progress);
    }
    g_object_unref(parser); g_free(resp);
}

/**
 * pcvctl cloud finalize --name <vm-name>
 * Finalize near-live import (Phase 2: stop EC2, delta sync, start VM)
 */
static void cmd_cloud_finalize(int argc, char *argv[])
{
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--name") == 0 && i + 1 < argc) { name = argv[++i]; }
        else if (!g_str_has_prefix(argv[i], "--")) name = argv[i];
    }
    if (!name) {
        printf("%sUsage: pcvctl cloud finalize --name <vm-name>%s\n"
               "  Finalize near-live import (Phase 2): stop EC2, delta sync, start VM\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }

    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    json_object_set_boolean_member(p, "finalize", TRUE);

    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.import.ec2", p, &error);
    if (error) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET)); g_error_free(error); return; }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *e = json_object_get_object_member(root, "error");
            fprintf(stderr, "%s[!] %s%s\n", cc(CYBER_RED),
                json_object_get_string_member(e, "message"), cc(CYBER_RESET));
        } else {
            JsonObject *r = json_object_has_member(root, "result")
                ? json_object_get_object_member(root, "result") : root;
            const char *jid = json_object_has_member(r, "job_id")
                ? json_object_get_string_member(r, "job_id") : "?";
            printf("%s[+] Finalize started%s — job: %s, VM: %s\n",
                   cc(CYBER_GREEN), cc(CYBER_RESET), jid, name);
            printf("  Use 'pcvctl cloud status --name %s' to track progress\n", name);
        }
    }
    g_object_unref(parser); g_free(resp);
}

/* ── cloud jobs — 전체 마이그레이션 작업 목록 ───────────────── */
static void cmd_cloud_jobs(int argc, char *argv[])
{
    (void)argc; (void)argv;
    gchar *resp = purectl_send_request("cloud.jobs.list", NULL, NULL);
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        fprintf(stderr, "Failed to parse response\n");
        g_object_unref(parser); g_free(resp);
        return;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        fprintf(stderr, "Error: %s\n",
                json_object_get_string_member(e, "message"));
        g_object_unref(parser); g_free(resp);
        return;
    }

    JsonArray *arr = json_object_has_member(root, "result")
        ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
    if (!arr || json_array_get_length(arr) == 0) {
        printf("No migration jobs found.\n");
        g_object_unref(parser); g_free(resp);
        return;
    }

    printf("%s%-14s %-16s %-8s %-12s %5s  %-8s  %s%s\n",
           cc(CYBER_BOLD), "JOB_ID", "VM", "DIR", "STATUS", "%",
           "ELAPSED", "DETAIL", cc(CYBER_RESET));
    printf("──────────────────────────────────────────────────────────────────────────────\n");

    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonObject *j = json_array_get_object_element(arr, i);
        const char *job_id = json_object_get_string_member(j, "job_id");
        const char *name   = json_object_get_string_member(j, "name");
        const char *dir    = json_object_get_string_member(j, "direction");
        const char *status = json_object_get_string_member(j, "status");
        gint64 prog        = json_object_get_int_member(j, "progress_percent");
        gint64 elapsed     = json_object_get_int_member(j, "elapsed_sec");
        const char *detail = json_object_get_string_member(j, "detail");

        const char *sc = (g_strcmp0(status, "done") == 0) ? CYBER_GREEN
                       : (g_strcmp0(status, "failed") == 0) ? CYBER_RED
                       : CYBER_YELLOW;

        printf("%-14s %-16s %-8s %s%-12s%s %3" G_GINT64_FORMAT "%%  %5" G_GINT64_FORMAT "s  %s\n",
               job_id, name, dir, cc(sc), status, cc(CYBER_RESET),
               prog, elapsed, detail ?: "");
    }
    g_object_unref(parser); g_free(resp);
}

/* ── cloud cancel — 마이그레이션 작업 취소 ───────────────────── */
static void cmd_cloud_cancel(int argc, char *argv[])
{
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--name") == 0 && i + 1 < argc) { name = argv[++i]; }
    }
    if (!name && argc > 2) name = argv[2];
    if (!name) { fprintf(stderr, "Usage: pcvctl cloud cancel --name <vm>\n"); return; }

    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    gchar *resp = purectl_send_request("cloud.job.cancel", p, NULL);
    /* p 소유권은 purectl_send_request → json_object_set_object_member로 이전됨 */
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, resp, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *e = json_object_get_object_member(root, "error");
            fprintf(stderr, "Error: %s\n",
                    json_object_get_string_member(e, "message"));
        } else {
            printf("%sCancel requested%s for VM: %s\n",
                   cc(CYBER_GREEN), cc(CYBER_RESET), name);
        }
    }
    g_object_unref(parser); g_free(resp);
}

/* ── Phase 3: Cluster Node Evacuate ── */


/* ── Phase 3: Storage Pool Forecast ── */

/**
 * cmd_storage_pool_forecast - 스토리지 풀 용량 예측.
 * Usage: pcvctl storage pool forecast [pool_name]
 * RPC: storage.pool.forecast {"pool":"pcvpool"}
 */
void cmd_storage_pool_forecast(int argc, char *argv[]) {
    JsonObject *params = json_object_new();
    /* argv: storage pool forecast [pool_name] → argv[3] */
    if (argc >= 4)
        json_object_set_string_member(params, "pool", argv[3]);
    else
        json_object_set_string_member(params, "pool", "pcvpool");
    GError *error = NULL;
    gchar *resp = purectl_send_request("storage.pool.forecast", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
        g_object_unref(parser); g_free(resp); return;
    }
    JsonNode *res_node = json_object_has_member(root, "result")
        ? json_object_get_member(root, "result") : NULL;

    if (res_node && JSON_NODE_HOLDS_OBJECT(res_node)) {
        JsonObject *r = json_node_get_object(res_node);
        const char *pool = json_object_get_string_member_with_default(r, "pool", "pcvpool");
        double used_pct  = json_object_get_double_member_with_default(r, "used_percent", 0);
        double daily_gb  = json_object_get_double_member_with_default(r, "daily_growth_gb", 0);
        gint64 days_left = json_object_get_int_member_with_default(r, "days_to_full", -1);
        const char *pred_date = json_object_get_string_member_with_default(r, "predicted_date", "-");
        const char *alert_lv  = json_object_get_string_member_with_default(r, "alert_level", "ok");

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"POOL","USED%","DAILY_GROWTH_GB","DAYS_TO_FULL","PREDICTED_DATE","ALERT"};
            PcvTable *t = ptbl_new(hdrs, 6);
            gchar used_b[16], daily_b[16], days_b[16];
            g_snprintf(used_b, sizeof(used_b), "%.1f", used_pct);
            g_snprintf(daily_b, sizeof(daily_b), "%.2f", daily_gb);
            g_snprintf(days_b, sizeof(days_b), "%" G_GINT64_FORMAT, days_left);
            ptbl_row(t, pool, used_b, daily_b, days_b, pred_date, alert_lv);
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else {
            const char *ac = g_strcmp0(alert_lv, "critical") == 0 ? CYBER_RED
                           : g_strcmp0(alert_lv, "warning") == 0  ? CYBER_YELLOW
                           : CYBER_GREEN;
            printf("%s%-12s %6s  %12s  %10s  %12s  %s%s\n",
                cc(CYBER_BOLD), "POOL", "USED%", "DAILY_GROWTH", "DAYS_LEFT",
                "PREDICTED", "ALERT", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────────────\n");
            printf("%-12s %5.1f%%  %10.2f GB  %10" G_GINT64_FORMAT "  %12s  %s%s%s\n",
                pool, used_pct, daily_gb, days_left, pred_date,
                cc(ac), alert_lv, cc(CYBER_RESET));
        }
    } else {
        print_raw_response(resp);
    }
    g_object_unref(parser); g_free(resp);
}

/* ── Phase 3: VM Block I/O Set ── */

/**
 * cmd_vm_blkio_set - VM 블록 I/O 제한 설정.
 * Usage: pcvctl vm blkio-set <name> [--read_bps N] [--write_bps N] [--read_iops N] [--write_iops N]
 * RPC: vm.blkio.set {"name":"...", "device":"vda", "read_bytes_sec":N, ...}
 */
void cmd_vm_blkio_set(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm blkio-set <name> [--read_bps N] [--write_bps N] "
               "[--read_iops N] [--write_iops N]%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "device", "vda");
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--read_bps") == 0 && i+1 < argc)
            json_object_set_int_member(params, "read_bytes_sec", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--write_bps") == 0 && i+1 < argc)
            json_object_set_int_member(params, "write_bytes_sec", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--read_iops") == 0 && i+1 < argc)
            json_object_set_int_member(params, "read_iops_sec", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--write_iops") == 0 && i+1 < argc)
            json_object_set_int_member(params, "write_iops_sec", atol(argv[++i]));
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.blkio.set", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    printf("%s[+] Block I/O limits set for %s%s\n",
        cc(CYBER_GREEN), argv[2], cc(CYBER_RESET));
    if (g_ctx.fmt == FMT_JSON) print_raw_response(resp);
    g_free(resp);
}

/* ── Phase 3: VM Block I/O Get ── */

/**
 * cmd_vm_blkio_get - VM 블록 I/O 제한 조회.
 * Usage: pcvctl vm blkio-get <name>
 * RPC: vm.blkio.get {"name":"...", "device":"vda"}
 */
void cmd_vm_blkio_get(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl vm blkio-get <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", argv[2]);
    json_object_set_string_member(params, "device", "vda");
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.blkio.get", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
        g_object_unref(parser); g_free(resp); return;
    }
    JsonNode *res_node = json_object_has_member(root, "result")
        ? json_object_get_member(root, "result") : NULL;
    if (res_node && JSON_NODE_HOLDS_OBJECT(res_node)) {
        JsonObject *r = json_node_get_object(res_node);
        const char *dev    = json_object_get_string_member_with_default(r, "device", "vda");
        gint64 rd_bps      = json_object_get_int_member_with_default(r, "read_bytes_sec", 0);
        gint64 wr_bps      = json_object_get_int_member_with_default(r, "write_bytes_sec", 0);
        gint64 rd_iops     = json_object_get_int_member_with_default(r, "read_iops_sec", 0);
        gint64 wr_iops     = json_object_get_int_member_with_default(r, "write_iops_sec", 0);

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"DEVICE","READ_BPS","WRITE_BPS","READ_IOPS","WRITE_IOPS"};
            PcvTable *t = ptbl_new(hdrs, 5);
            gchar rb[32], wb[32], ri[32], wi[32];
            g_snprintf(rb, sizeof(rb), "%" G_GINT64_FORMAT, rd_bps);
            g_snprintf(wb, sizeof(wb), "%" G_GINT64_FORMAT, wr_bps);
            g_snprintf(ri, sizeof(ri), "%" G_GINT64_FORMAT, rd_iops);
            g_snprintf(wi, sizeof(wi), "%" G_GINT64_FORMAT, wr_iops);
            ptbl_row(t, dev, rb, wb, ri, wi);
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else {
            printf("%s%-8s %12s %12s %12s %12s%s\n",
                cc(CYBER_BOLD), "DEVICE", "READ_BPS", "WRITE_BPS",
                "READ_IOPS", "WRITE_IOPS", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────\n");
            printf("%-8s %12" G_GINT64_FORMAT " %12" G_GINT64_FORMAT
                   " %12" G_GINT64_FORMAT " %12" G_GINT64_FORMAT "\n",
                dev, rd_bps, wr_bps, rd_iops, wr_iops);
        }
    } else {
        print_raw_response(resp);
    }
    g_object_unref(parser); g_free(resp);
}

/* ── Phase 3: Cluster Config Push ── */


/* ── Phase 3: Snapshot Schedule Status ── */

/**
 * cmd_snapshot_schedule_status - 스냅샷 스케줄 상태 조회.
 * Usage: pcvctl snapshot schedule-status
 * RPC: snapshot.schedule.status {}
 */
void cmd_snapshot_schedule_status(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *error = NULL;
    gchar *resp = purectl_send_request("snapshot.schedule.status", NULL, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (!resp) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *e = json_object_get_object_member(root, "error");
        g_printerr("%s[!] ERROR [%lld]: %s%s\n", ce(CYBER_RED),
            (long long)json_object_get_int_member(e, "code"),
            json_object_get_string_member(e, "message"), ce(CYBER_RESET));
        g_object_unref(parser); g_free(resp); return;
    }
    JsonNode *res_node = json_object_has_member(root, "result")
        ? json_object_get_member(root, "result") : NULL;
    JsonArray *arr = NULL;
    if (res_node && JSON_NODE_HOLDS_ARRAY(res_node))
        arr = json_node_get_array(res_node);

    if (arr && json_array_get_length(arr) > 0) {
        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
            const char *hdrs[] = {"VM","SNAPSHOTS","LAST_SNAPSHOT","NEXT_DUE","INTERVAL","RETENTION"};
            PcvTable *t = ptbl_new(hdrs, 6);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *s = json_array_get_object_element(arr, i);
                gchar snap_b[16], intv_b[16], ret_b[16];
                g_snprintf(snap_b, sizeof(snap_b), "%" G_GINT64_FORMAT,
                    json_object_get_int_member_with_default(s, "snapshot_count", 0));
                g_snprintf(intv_b, sizeof(intv_b), "%" G_GINT64_FORMAT "h",
                    json_object_get_int_member_with_default(s, "interval_hours", 0));
                g_snprintf(ret_b, sizeof(ret_b), "%" G_GINT64_FORMAT,
                    json_object_get_int_member_with_default(s, "retention", 0));
                ptbl_row(t,
                    json_object_get_string_member_with_default(s, "vm", "-"),
                    snap_b,
                    json_object_get_string_member_with_default(s, "last_snapshot", "-"),
                    json_object_get_string_member_with_default(s, "next_due", "-"),
                    intv_b, ret_b);
            }
            g_ctx.fmt == FMT_CSV ? ptbl_print_csv(t) : ptbl_print_plain(t);
            ptbl_free(t);
        } else {
            printf("%s%-16s %6s  %-20s  %-20s  %8s  %6s%s\n",
                cc(CYBER_BOLD), "VM", "SNAPS", "LAST_SNAPSHOT",
                "NEXT_DUE", "INTERVAL", "RETAIN", cc(CYBER_RESET));
            printf("───────────────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *s = json_array_get_object_element(arr, i);
                printf("%-16s %6" G_GINT64_FORMAT "  %-20s  %-20s  %6" G_GINT64_FORMAT "h  %6" G_GINT64_FORMAT "\n",
                    json_object_get_string_member_with_default(s, "vm", "-"),
                    json_object_get_int_member_with_default(s, "snapshot_count", 0),
                    json_object_get_string_member_with_default(s, "last_snapshot", "-"),
                    json_object_get_string_member_with_default(s, "next_due", "-"),
                    json_object_get_int_member_with_default(s, "interval_hours", 0),
                    json_object_get_int_member_with_default(s, "retention", 0));
            }
        }
    } else {
        printf("No snapshot schedules found.\n");
    }
    g_object_unref(parser); g_free(resp);
}

/* ════════════════════════════════════════════════════════════════════
 *  Container Advanced (15 commands)
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_container_logs - 컨테이너 로그 조회. container.logs RPC. */
static void cmd_container_logs(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container logs <name> [--lines N]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    gint64 lines = 100;
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--lines") == 0 && i + 1 < argc) lines = atol(argv[++i]);
    }
    json_object_set_int_member(p, "lines", lines);
    GError *e = NULL; gchar *r = purectl_send_request("container.logs", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, r, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *err = json_object_get_object_member(root, "error");
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
        } else if (json_object_has_member(root, "result")) {
            JsonNode *rn = json_object_get_member(root, "result");
            if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
                JsonObject *ro = json_node_get_object(rn);
                const char *log = json_object_get_string_member_with_default(ro, "log", "");
                printf("%s", log);
            } else {
                print_raw_response(r);
            }
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_container_volume_attach - 컨테이너 볼륨 연결. container.volume.attach RPC. */
static void cmd_container_volume_attach(int argc, char *argv[]) {
    if (argc < 5) { printf("%sUsage: pcvctl container volume-attach <name> <host_path> <container_path>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    json_object_set_string_member(p, "host_path", argv[3]);
    json_object_set_string_member(p, "container_path", argv[4]);
    GError *e = NULL; gchar *r = purectl_send_request("container.volume.attach", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_VOLUME_ATTACH"); g_free(r);
}

/** cmd_container_volume_detach - 컨테이너 볼륨 분리. container.volume.detach RPC. */
static void cmd_container_volume_detach(int argc, char *argv[]) {
    if (argc < 4) { printf("%sUsage: pcvctl container volume-detach <name> <container_path>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    json_object_set_string_member(p, "container_path", argv[3]);
    GError *e = NULL; gchar *r = purectl_send_request("container.volume.detach", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_VOLUME_DETACH"); g_free(r);
}

/** cmd_container_volume_list - 컨테이너 볼륨 목록. container.volume.list RPC. */
static void cmd_container_volume_list(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container volume-list <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.volume.list", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%-30s %-30s%s\n", cc(CYBER_BOLD), "HOST_PATH", "CONTAINER_PATH", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *v = json_array_get_object_element(arr, i);
                printf("%-30s %-30s\n",
                    json_object_get_string_member_with_default(v, "host_path", "-"),
                    json_object_get_string_member_with_default(v, "container_path", "-"));
            }
        } else {
            printf("No volumes attached.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_container_env_set - 컨테이너 환경변수 설정. container.env.set RPC. */
static void cmd_container_env_set(int argc, char *argv[]) {
    if (argc < 5) { printf("%sUsage: pcvctl container env-set <name> <key> <value>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    json_object_set_string_member(p, "key", argv[3]);
    json_object_set_string_member(p, "value", argv[4]);
    GError *e = NULL; gchar *r = purectl_send_request("container.env.set", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_ENV_SET"); g_free(r);
}

/** cmd_container_env_list - 컨테이너 환경변수 목록. container.env.list RPC. */
static void cmd_container_env_list(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container env-list <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.env.list", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonObject *envs = json_object_has_member(root, "result")
            ? json_object_get_object_member(root, "result") : NULL;
        if (envs && json_object_get_size(envs) > 0) {
            printf("%s%-24s %-40s%s\n", cc(CYBER_BOLD), "KEY", "VALUE", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────────\n");
            GList *members = json_object_get_members(envs);
            for (GList *it = members; it; it = it->next) {
                const gchar *key = it->data;
                const gchar *value = json_object_get_string_member_with_default(envs, key, "-");
                printf("%-24s %-40s\n", key, value);
            }
            g_list_free(members);
        } else {
            printf("No environment variables set.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_container_env_delete - 컨테이너 환경변수 삭제. container.env.delete RPC. */
static void cmd_container_env_delete(int argc, char *argv[]) {
    if (argc < 4) { printf("%sUsage: pcvctl container env-delete <name> <key>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    json_object_set_string_member(p, "key", argv[3]);
    GError *e = NULL; gchar *r = purectl_send_request("container.env.delete", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_ENV_DELETE"); g_free(r);
}

/** cmd_container_health_set - 컨테이너 헬스체크 설정. container.health.set RPC. */
static void cmd_container_health_set(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container health-set <name> --type http --target <url>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--type") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "type", argv[++i]);
        else if (g_strcmp0(argv[i], "--target") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "target", argv[++i]);
        else if (g_strcmp0(argv[i], "--interval") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "interval_sec", atoi(argv[++i]));
    }
    GError *e = NULL; gchar *r = purectl_send_request("container.health.set", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_HEALTH_SET"); g_free(r);
}

/** cmd_container_health_get - 컨테이너 헬스체크 조회. container.health.get RPC. */
static void cmd_container_health_get(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container health-get <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.health.get", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, r, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *err = json_object_get_object_member(root, "error");
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
        } else if (json_object_has_member(root, "result")) {
            JsonObject *res = json_object_get_object_member(root, "result");
            printf("%sHealth Check for %s%s\n", cc(CYBER_BOLD), argv[2], cc(CYBER_RESET));
            printf("  Type:     %s\n", json_object_get_string_member_with_default(res, "type", "-"));
            printf("  Target:   %s\n", json_object_get_string_member_with_default(res, "target", "-"));
            printf("  Status:   %s\n", json_object_get_string_member_with_default(res, "status", "-"));
            printf("  Interval: %" G_GINT64_FORMAT "s\n", json_object_get_int_member_with_default(res, "interval", 0));
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_container_health_delete - 컨테이너 헬스체크 삭제. container.health.delete RPC. */
static void cmd_container_health_delete(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container health-delete <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.health.delete", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_HEALTH_DELETE"); g_free(r);
}

/** cmd_container_nic_list - 컨테이너 NIC 목록. container.nic.list RPC. */
static void cmd_container_nic_list(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container nic-list <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("container.nic.list", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%-12s %-18s %-16s %-10s%s\n", cc(CYBER_BOLD), "IFACE", "MAC", "BRIDGE", "STATE", cc(CYBER_RESET));
            printf("────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *n = json_array_get_object_element(arr, i);
                printf("%-12s %-18s %-16s %-10s\n",
                    json_object_get_string_member_with_default(n, "iface", "-"),
                    json_object_get_string_member_with_default(n, "mac", "-"),
                    json_object_get_string_member_with_default(n, "bridge", "-"),
                    json_object_get_string_member_with_default(n, "state", "-"));
            }
        } else {
            printf("No NICs attached.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_container_nic_attach - 컨테이너 NIC 추가. container.nic.attach RPC. */
static void cmd_container_nic_attach(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container nic-attach <name> --bridge pcvbr0%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--bridge") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "bridge", argv[++i]);
        else if (g_strcmp0(argv[i], "--mac") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "hwaddr", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("container.nic.attach", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_NIC_ATTACH"); g_free(r);
}

/** cmd_container_nic_detach - 컨테이너 NIC 분리. container.nic.detach RPC. */
static void cmd_container_nic_detach(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container nic-detach <name> --mac XX:XX:XX:XX:XX:XX%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mac") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "mac", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("container.nic.detach", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_NIC_DETACH"); g_free(r);
}

/** cmd_container_set_limits - 컨테이너 리소스 제한. container.set_limits RPC. */
static void cmd_container_set_limits(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container set-limits <name> --memory_mb N --cpu_quota N%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--memory_mb") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "memory_mb", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--cpu_quota") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "cpu_percent", atol(argv[++i]));
    }
    GError *e = NULL; gchar *r = purectl_send_request("container.set_limits", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_SET_LIMITS"); g_free(r);
}

/** cmd_container_set_bandwidth - 컨테이너 대역폭 설정. container.set_bandwidth RPC. */
static void cmd_container_set_bandwidth(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl container set-bandwidth <name> --inbound N --outbound N%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--inbound") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "inbound_kbps", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--outbound") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "outbound_kbps", atol(argv[++i]));
    }
    GError *e = NULL; gchar *r = purectl_send_request("container.set_bandwidth", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "CONTAINER_SET_BANDWIDTH"); g_free(r);
}

/* ════════════════════════════════════════════════════════════════════
 *  Backup Advanced (5 commands)
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_backup_restore - 백업 복원. backup.restore RPC. */
static void cmd_backup_restore(int argc, char *argv[]) {
    if (argc < 4) { printf("%sUsage: pcvctl backup restore <name> <snapshot>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "vm_name", argv[2]);
    json_object_set_string_member(p, "snapshot_name", argv[3]);
    GError *e = NULL; gchar *r = purectl_send_request("backup.restore", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "BACKUP_RESTORE"); g_free(r);
}

/** cmd_backup_incremental - 증분 백업. backup.incremental RPC. */
static void cmd_backup_incremental(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl backup incremental <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("backup.incremental", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "BACKUP_INCREMENTAL"); g_free(r);
}

/** cmd_backup_verify - 백업 무결성 검증. backup.verify RPC. */
static void cmd_backup_verify(int argc, char *argv[]) {
    if (argc < 4) { printf("%sUsage: pcvctl backup verify <name> <snapshot>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    json_object_set_string_member(p, "snapshot", argv[3]);
    GError *e = NULL; gchar *r = purectl_send_request("backup.verify", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "BACKUP_VERIFY"); g_free(r);
}

/** cmd_backup_replicate - 백업 원격 복제. backup.replicate RPC. */
static void cmd_backup_replicate(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl backup replicate <name> --target <ip> --user <user>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--target") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "target_node", argv[++i]);
        else if (g_strcmp0(argv[i], "--user") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "ssh_user", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("backup.replicate", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "BACKUP_REPLICATE"); g_free(r);
}

/** cmd_backup_export_s3 - 백업 S3 내보내기. backup.export_s3 RPC. */
static void cmd_backup_export_s3(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl backup export-s3 <name>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("backup.export_s3", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "BACKUP_EXPORT_S3"); g_free(r);
}

/* ════════════════════════════════════════════════════════════════════
 *  VM Additional (4 commands)
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_vm_clone - VM 복제. vm.clone RPC. */
static void cmd_vm_clone(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm clone <source> <clone_name> [--mode cow|full] [--template-prepared|--guest-reset]%s\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "source", argv[2]);
    json_object_set_string_member(p, "clone_name", argv[3]);
    for (int i = 4; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mode") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "mode", argv[++i]);
        else if (g_strcmp0(argv[i], "--template-prepared") == 0)
            json_object_set_boolean_member(p, "template_prepared", TRUE);
        else if (g_strcmp0(argv[i], "--guest-reset") == 0)
            json_object_set_boolean_member(p, "guest_reset", TRUE);
        else if (g_strcmp0(argv[i], "--no-guest-reset") == 0)
            json_object_set_boolean_member(p, "guest_reset", FALSE);
    }
    GError *e = NULL; gchar *r = purectl_send_request("vm.clone", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "VM_CLONE"); g_free(r);
}

/** cmd_vm_pin_vcpu - vCPU 피닝. vm.pin_vcpu RPC. */
static void cmd_vm_pin_vcpu(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl vm pin-vcpu <name> --vcpu N --cpuset 0-3%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--vcpu") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "vcpu", atoi(argv[++i]));
        else if (g_strcmp0(argv[i], "--cpuset") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "cpuset", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("vm.pin_vcpu", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "VM_PIN_VCPU"); g_free(r);
}

/** cmd_vm_snapshot_delete_all - 스냅샷 일괄 삭제. vm.snapshot.delete_all RPC. */
static void cmd_vm_snapshot_delete_all(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl vm snapshot-delete-all <name> [--prefix auto-] [--keep N]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--prefix") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "prefix", argv[++i]);
        else if (g_strcmp0(argv[i], "--keep") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "keep_recent", atoi(argv[++i]));
    }
    GError *e = NULL; gchar *r = purectl_send_request("vm.snapshot.delete_all", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "VM_SNAPSHOT_DELETE_ALL"); g_free(r);
}

/** cmd_vm_export_ova - VM OVA 내보내기. vm.export.ova RPC. */
static void cmd_vm_export_ova(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl vm export-ova <name> [--output-dir /tmp]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", argv[2]);
    for (int i = 3; i < argc; i++) {
        if ((g_strcmp0(argv[i], "--output-dir") == 0 ||
             g_strcmp0(argv[i], "--output") == 0) && i + 1 < argc)
            json_object_set_string_member(p, "output_dir", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("vm.export.ova", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "VM_EXPORT_OVA"); g_free(r);
}

/** cmd_vm_import_ova - OVA 파일 가져오기. vm.import.ova RPC. */
static void cmd_vm_import_ova(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl vm import-ova <ova_path> <name> [--pool pcvpool/vms]%s\n",
               cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "ova_path", argv[2]);
    json_object_set_string_member(p, "name", argv[3]);
    for (int i = 4; i < argc; i++) {
        if (g_strcmp0(argv[i], "--pool") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "pool", argv[++i]);
    }
    GError *e = NULL; gchar *r = purectl_send_request("vm.import.ova", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "VM_IMPORT_OVA"); g_free(r);
}

/* ════════════════════════════════════════════════════════════════════
 *  Monitor / QoS / Misc (6 commands)
 * ════════════════════════════════════════════════════════════════════ */

/** cmd_monitor_processes - 프로세스 모니터링. monitor.processes RPC. */
static void cmd_monitor_processes(int argc, char *argv[]) {
    JsonObject *p = json_object_new();
    gint64 top = 10;
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--type") == 0 && i + 1 < argc)
            json_object_set_string_member(p, "type", argv[++i]);
        else if (g_strcmp0(argv[i], "--top") == 0 && i + 1 < argc)
            top = atol(argv[++i]);
    }
    json_object_set_int_member(p, "top", top);
    GError *e = NULL; gchar *r = purectl_send_request("monitor.processes", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%7s %-20s %6s %8s  %s%s\n", cc(CYBER_BOLD), "PID", "COMMAND", "CPU%", "MEM_MB", "STATE", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *proc = json_array_get_object_element(arr, i);
                printf("%7" G_GINT64_FORMAT " %-20s %5.1f %8" G_GINT64_FORMAT "  %s\n",
                    json_object_get_int_member_with_default(proc, "pid", 0),
                    json_object_get_string_member_with_default(proc, "command", "-"),
                    json_object_get_double_member_with_default(proc, "cpu_percent", 0),
                    json_object_get_int_member_with_default(proc, "mem_mb", 0),
                    json_object_get_string_member_with_default(proc, "state", "-"));
            }
        } else {
            printf("No processes found.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_network_qos_set - 네트워크 QoS 설정. network.qos.set RPC. */
static void cmd_network_qos_set(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl network qos-set <iface> --rate N%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "iface", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], "--rate") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "rate", atol(argv[++i]));
        else if (g_strcmp0(argv[i], "--burst") == 0 && i + 1 < argc)
            json_object_set_int_member(p, "burst", atol(argv[++i]));
    }
    GError *e = NULL; gchar *r = purectl_send_request("network.qos.set", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "NETWORK_QOS_SET"); g_free(r);
}

/** cmd_network_qos_get - 네트워크 QoS 조회. network.qos.get RPC. */
static void cmd_network_qos_get(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl network qos-get <iface>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "iface", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("network.qos.get", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, r, -1, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "error")) {
            JsonObject *err = json_object_get_object_member(root, "error");
            g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
        } else if (json_object_has_member(root, "result")) {
            JsonObject *res = json_object_get_object_member(root, "result");
            printf("%sQoS for %s%s\n", cc(CYBER_BOLD), argv[2], cc(CYBER_RESET));
            printf("  Rate:  %" G_GINT64_FORMAT " kbps\n", json_object_get_int_member_with_default(res, "rate", 0));
            printf("  Burst: %" G_GINT64_FORMAT " kbps\n", json_object_get_int_member_with_default(res, "burst", 0));
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_network_qos_remove - 네트워크 QoS 제거. network.qos.remove RPC. */
static void cmd_network_qos_remove(int argc, char *argv[]) {
    if (argc < 3) { printf("%sUsage: pcvctl network qos-remove <iface>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET)); return; }
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "iface", argv[2]);
    GError *e = NULL; gchar *r = purectl_send_request("network.qos.remove", p, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    print_action_response(r, "NETWORK_QOS_REMOVE"); g_free(r);
}

/** cmd_healing_history - 자가치유 이력. healing.history RPC. */
static void cmd_healing_history(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    GError *e = NULL; gchar *r = purectl_send_request("healing.history", NULL, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%-20s %-16s %-12s %-30s%s\n", cc(CYBER_BOLD), "TIMESTAMP", "TARGET", "ACTION", "DETAIL", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *h = json_array_get_object_element(arr, i);
                printf("%-20s %-16s %-12s %-30s\n",
                    json_object_get_string_member_with_default(h, "timestamp", "-"),
                    json_object_get_string_member_with_default(h, "target", "-"),
                    json_object_get_string_member_with_default(h, "action", "-"),
                    json_object_get_string_member_with_default(h, "detail", "-"));
            }
        } else {
            printf("No healing history found.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

/** cmd_gpu_list - GPU 목록. gpu.list RPC. */
static void cmd_gpu_list(int argc, char *argv[]) {
    GError *e = NULL; gchar *r = purectl_send_request("gpu.list", NULL, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%-12s %-40s %-10s%s\n", cc(CYBER_BOLD), "PCI_ADDR", "DEVICE", "DRIVER", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *g = json_array_get_object_element(arr, i);
                printf("%-12s %-40s %-10s\n",
                    json_object_get_string_member_with_default(g, "pci_addr", "-"),
                    json_object_get_string_member_with_default(g, "device", "-"),
                    json_object_get_string_member_with_default(g, "driver", "-"));
            }
        } else {
            printf("No GPUs found.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

typedef void (*CmdHandler)(int argc, char *argv[]);
typedef struct {
    const char *object;
    const char *action;
    CmdHandler  handler;
    const char *help_text;
} CommandRoute;

static CommandRoute routes[] = {
    /* ── VM 라이프사이클 ── */
    {"vm","create",      cmd_vm_create,      "Create a new VM"},
    {"vm","delete",      cmd_vm_delete,      "Delete a VM (interactive confirm)"},
    {"vm","list",        cmd_vm_list,        "List all VMs"},
    {"vm","rename",      cmd_vm_rename,      "Rename a shut off VM and standard disk path"},
    {"vm","start",       cmd_vm_start,       "Start a VM"},
    {"vm","stop",        cmd_vm_stop,        "Stop a VM"},
    {"vm","pause",       cmd_vm_pause,       "Pause (suspend) a VM"},
    {"vm","resume",      cmd_vm_resume,      "Resume a paused VM"},
    {"vm","limit",       cmd_vm_limit,       "Set cgroup CPU/MEM limits"},
    {"vm","set-memory",  cmd_vm_set_memory,  "Hot-set RAM (MB)"},
    {"vm","set-vcpu",    cmd_vm_set_vcpu,    "Hot-set vCPU count"},
    {"vm","vnc",         cmd_vm_vnc,         "Get VNC port for a running VM"},
    {"vm","eject",       cmd_vm_eject,       "Eject ISO from virtual cdrom"},
    {"vm","delete-status",cmd_vm_delete_status,"Check VM delete progress"},
    {"vm","memory-stats",  cmd_vm_memory_stats,   "VM memory balloon/RSS/swap stats"},
    {"vm","cpu-stats",     cmd_vm_cpu_stats,       "VM CPU time + per-vCPU stats"},
    {"vm","disk-resize",   cmd_vm_disk_resize,     "Live resize VM disk"},
    {"vm","guest-agent-status", cmd_vm_guest_agent_status, "Inspect qemu-guest-agent channel/package status"},
    {"vm","guest-agent-ensure-channel", cmd_vm_guest_agent_ensure_channel, "Add qemu-guest-agent libvirt channel"},
    {"vm","guest-ping",    cmd_vm_guest_ping,      "Ping guest agent"},
    {"vm","guest-exec",    cmd_vm_guest_exec,      "Execute command via guest agent"},
    {"vm","guest-shutdown", cmd_vm_guest_shutdown,  "Graceful shutdown via guest agent"},
    {"vm","blkio-set",      cmd_vm_blkio_set,       "Set block I/O limits (--read_bps/--write_bps/--read_iops/--write_iops)"},
    {"vm","blkio-get",      cmd_vm_blkio_get,       "Get block I/O limits"},
    {"vm","bandwidth",      cmd_vm_bandwidth,       "Set VM network bandwidth (--inbound-kbps/--outbound-kbps)"},
    /* ── NIC 관리 ── */
    {"nic","list",       cmd_nic_list,       "List NICs attached to a VM"},
    {"nic","add",        cmd_nic_add,        "Hot-attach NIC to a VM"},
    {"nic","remove",     cmd_nic_remove,     "Hot-detach NIC from a VM"},
    /* ── ISO 관리 ── */
    {"iso","mount",      cmd_iso_mount,      "Mount ISO to VM cdrom"},
    {"iso","eject",      cmd_iso_eject,      "Eject ISO from VM cdrom"},
    {"iso","list",       cmd_iso_list,       "List available ISO files"},
    /* ── 노드 관리 ── */
    {"node","drain",     cmd_node_drain,     "Drain node (stop accepting new RPCs)"},
    {"node","resume",    cmd_node_resume,    "Resume node after drain"},
    {"node","version",   cmd_node_version,   "Show daemon version"},
    /* ── 스냅샷 ── */
    {"snapshot","create",          cmd_snapshot_create,          "Create ZFS snapshot"},
    {"snapshot","list",            cmd_snapshot_list,            "List ZFS snapshots for a VM"},
    {"snapshot","rollback",        cmd_snapshot_rollback,        "Rollback to a ZFS snapshot"},
    {"snapshot","delete",          cmd_snapshot_delete,          "Delete a ZFS snapshot"},
    {"snapshot","verify",          cmd_snapshot_verify,          "Verify ZFS snapshot exists"},
    {"snapshot","schedule-status", cmd_snapshot_schedule_status, "Snapshot schedule status"},
    /* ── 모니터링 ── */
    {"monitor","metrics",   cmd_monitor_metrics,   "VM CPU/MEM usage"},
    {"monitor","fleet",     cmd_monitor_fleet,     "Global fleet stats"},
    /* ── 네트워크 ── */
    {"network","create",    cmd_net_create,  "Create bridge (nat/isolated/routed)"},
    {"network","delete",    cmd_net_delete,  "Delete bridge"},
    {"network","list",      cmd_net_list,    "List bridges"},
    {"network","mode",      cmd_net_mode,    "Change bridge mode"},
    {"network","edit",      cmd_net_edit,    "Edit bridge mode (--mode)"},
    {"network","dhcp",      cmd_net_dhcp,    "Toggle DHCP (--enable/--disable)"},
    {"network","bind",      cmd_net_bind,    "Bind physical NIC to bridge"},
    /* ── 스토리지 ── */
    {"storage","pool",      cmd_storage_pool,         "ZFS pool list"},
    {"storage","pool-forecast", cmd_storage_pool_forecast, "Storage pool capacity forecast"},
    {"storage","zvol",      cmd_storage_zvol,         "ZVOL create/delete/list"},
    /* ── 디바이스 핫플러그 ── */
    {"device","disk",       cmd_device_disk, "Live attach/detach block device"},
    /* ── 컨테이너 (LXC) ── */
    {"container","create",   cmd_container_create,   "Create LXC container"},
    {"container","destroy",  cmd_container_destroy,  "Destroy LXC container"},
    {"container","start",    cmd_container_start,    "Start container"},
    {"container","stop",     cmd_container_stop,     "Stop container"},
    {"container","list",     cmd_container_list,     "List containers"},
    {"container","metrics",  cmd_container_metrics,  "Container resource usage"},
    {"container","exec",     cmd_container_exec,     "Exec command in container"},
    {"container","snapshot", cmd_container_snapshot, "Container ZFS snapshots"},
    /* ── OVN SDN ── */
    {"ovn","status",        cmd_ovn_status,        "OVN controller status"},
    {"ovn","switch",        cmd_ovn_switch,        "OVN logical switch (list/create/delete)"},
    {"ovn","router",        cmd_ovn_router,        "OVN logical router (list/create/delete)"},
    {"ovn","nat",           cmd_ovn_nat,            "OVN NAT rules (list)"},
    {"ovn","acl",           cmd_ovn_acl,            "OVN ACL rules (list/add)"},
    {"ovn","dhcp",          cmd_ovn_dhcp,           "OVN DHCP (enable)"},
    /* ── OVS-DPDK / SR-IOV (Phase 4) ── */
    {"dpdk",  "status",  cmd_dpdk_status,   "OVS-DPDK 상태"},
    {"dpdk",  "bind",    cmd_dpdk_bind,     "NIC DPDK 바인딩"},
    {"dpdk",  "unbind",  cmd_dpdk_unbind,   "NIC DPDK 해제"},
    {"dpdk",  "list",    cmd_dpdk_list,     "DPDK 디바이스 목록"},
    {"dpdk",  "bridge",  cmd_dpdk_bridge,   "DPDK 브릿지 create/delete"},
    {"dpdk",  "hugepage",cmd_dpdk_hugepage, "Hugepage 현황"},
    {"sriov", "status",  cmd_sriov_status,  "SR-IOV PF/VF 상태"},
    {"sriov", "enable",  cmd_sriov_enable,  "SR-IOV VF 활성화"},
    {"sriov", "disable", cmd_sriov_disable, "SR-IOV VF 비활성화"},
    {"sriov", "list",    cmd_sriov_list,    "VF 목록"},
    {"sriov", "set",     cmd_sriov_set,     "VF 속성 설정"},
    {"sriov", "attach",  cmd_sriov_attach,  "VM에 VF 연결"},
    {"sriov", "detach",  cmd_sriov_detach,  "VM에서 VF 분리"},
    /* ── RBAC 인증 ── */
    {"auth","list",         cmd_auth_list,         "List users (RBAC)"},
    {"auth","create",       cmd_auth_create,       "Create user (RBAC)"},
    {"auth","delete",       cmd_auth_delete,       "Delete user (RBAC)"},
    {"auth","role",         cmd_auth_role,         "Set user role (RBAC)"},
    /* ── VM 템플릿 ── */
    {"template","list",     cmd_template_list,     "List VM templates"},
    {"template","get",      cmd_template_get,      "Get template detail (JSON)"},
    {"template","create",   cmd_template_create,   "Create VM template"},
    {"template","delete",   cmd_template_delete,   "Delete VM template"},
    /* ── 백업 정책 ── */
    {"backup","list",       cmd_backup_list,       "List backup policies"},
    {"backup","set",        cmd_backup_set,        "Set backup policy for VM"},
    {"backup","delete",     cmd_backup_delete,     "Delete backup policy"},
    {"backup","history",    cmd_backup_history,    "Backup history for VM"},
    /* ── 알림 관리 (P2) ── */
    {"alert","list",        cmd_alert_list,         "Alert history"},
    {"alert","config",      cmd_alert_config,       "Alert config (thresholds/webhook)"},
    {"alert","set",         cmd_alert_set,          "Set alert thresholds/webhook"},
    {"alert","reload",      cmd_alert_reload,       "Reload alert config from daemon.conf"},
    /* ── AI Agent (P3) ── */
    {"agent","config",      cmd_agent_config,       "AI Agent config"},
    {"agent","set",         cmd_agent_set,          "Set AI provider (--provider/--api_key/--enabled)"},
    {"agent","history",     cmd_agent_history,      "AI consensus history"},
    /* ── Phase 2: VM Operations ── */
    {"vm","autostart",       cmd_vm_autostart,       "Set VM autostart (--enable/--disable)"},
    {"vm","disk-throttle",   cmd_vm_disk_throttle,   "Set disk I/O throttle (--read-iops/--write-iops)"},
    {"vm","numa",            cmd_vm_numa_info,       "NUMA topology info"},
    {"vm","sla",             cmd_vm_sla,             "VM SLA report"},
    {"vm","schedule",        cmd_vm_schedule,        "VM schedule (set/list)"},
    {"vm","security-group",  cmd_vm_secgroup,        "Assign security group to VM"},
    /* ── Phase 2: Monitoring ── */
    {"storage","health",     cmd_storage_health,     "Storage pool health check"},
    {"capacity","forecast",  cmd_capacity_forecast,  "Capacity forecast"},
    {"billing","report",     cmd_billing_report,     "VM billing report"},
    /* ── Phase 2: Advanced ── */
    {"job","list",           cmd_job_list,           "List async jobs"},
    {"vm","batch",           cmd_batch_execute,      "Batch start/stop multiple VMs"},
    {"prometheus","sd",      cmd_prometheus_sd,      "Prometheus service discovery"},
    {"webhook","list",       cmd_webhook_list,       "Event webhook list"},
    {"alert","actions",      cmd_alert_actions,      "Alert action list"},
    /* ── Phase 3: Audit ── */
    {"audit","search",       cmd_audit_search,       "Search audit logs (--user/--from/--to/--method/--limit)"},
    /* ── Native Host HIDS/HIPS Security Guard ── */
    {"security","status",           cmd_security,     "Security Guard status"},
    {"security","events",           cmd_security,     "List HIDS/HIPS security events"},
    {"security","event",            cmd_security,     "Show one HIDS/HIPS security event"},
    {"security","pending",          cmd_security,     "List pending HIPS actions"},
    {"security","approve",          cmd_security,     "Approve a pending HIPS action"},
    {"security","dismiss",          cmd_security,     "Dismiss a pending HIPS action"},
    {"security","baseline-status",  cmd_security,     "Show HIDS file baseline status"},
    {"security","baseline-refresh", cmd_security,     "Refresh HIDS file baseline"},
    {"security","enable",           cmd_security,     "Enable Security Guard"},
    {"security","disable",          cmd_security,     "Disable Security Guard"},
    /* ── Phase 3: Security Groups ── */
    {"security-group","list",   cmd_secgroup,        "List security groups"},
    {"security-group","create", cmd_secgroup,        "Create security group"},
    {"security-group","delete", cmd_secgroup,        "Delete security group"},
    {"security-group","rule",   cmd_secgroup,        "Add security group rule"},
    /* ── Phase 3: Webhook DLQ ── */
    {"webhook","dlq",        cmd_webhook_dlq,        "Webhook DLQ (list/retry)"},
    /* ── Phase 3: GPU ── */
    {"gpu","metrics",        cmd_gpu_metrics,        "GPU metrics"},
    /* ── Phase 3: Config ── */
    {"config","history",     cmd_config_history,     "Config change history"},
    {"config","backup",      cmd_config_backup,      "Backup current config"},
    {"config","validate",    cmd_config_validate,    "Validate daemon.conf (no daemon needed)"},
    /* ── Phase 3: Template ── */
    {"template","history",   cmd_template_history,   "Template change history"},
    /* ── gRPC ── */
    {"grpc",    "status",    cmd_grpc_status,        "gRPC 서버 상태 확인"},
    {"grpc",    "test",      cmd_grpc_test,          "gRPC 연결 테스트"},
    /* ── Cloud Migration ── */
    {"cloud",   "import",    cmd_cloud_import,       "Import EC2 AMI → PureCVisor VM"},
    {"cloud",   "export",    cmd_cloud_export,       "Export PureCVisor VM → EC2 AMI"},
    {"cloud",   "status",    cmd_cloud_status,       "Check cloud migration job status"},
    {"cloud",   "jobs",      cmd_cloud_jobs,         "List all cloud migration jobs"},
    {"cloud",   "cancel",    cmd_cloud_cancel,       "Cancel a running migration job"},
    {"cloud",   "finalize",  cmd_cloud_finalize,     "Finalize near-live import (Phase 2)"},
    /* ── Container Advanced ── */
    {"container","logs",           cmd_container_logs,           "Container logs (--lines N)"},
    {"container","volume-attach",  cmd_container_volume_attach,  "Attach host volume to container"},
    {"container","volume-detach",  cmd_container_volume_detach,  "Detach volume from container"},
    {"container","volume-list",    cmd_container_volume_list,    "List container volumes"},
    {"container","env-set",        cmd_container_env_set,        "Set container environment variable"},
    {"container","env-list",       cmd_container_env_list,       "List container environment variables"},
    {"container","env-delete",     cmd_container_env_delete,     "Delete container environment variable"},
    {"container","health-set",     cmd_container_health_set,     "Set container health check (--type/--target)"},
    {"container","health-get",     cmd_container_health_get,     "Get container health check config"},
    {"container","health-delete",  cmd_container_health_delete,  "Delete container health check"},
    {"container","nic-list",       cmd_container_nic_list,       "List container NICs"},
    {"container","nic-attach",     cmd_container_nic_attach,     "Attach NIC to container (--bridge)"},
    {"container","nic-detach",     cmd_container_nic_detach,     "Detach NIC from container (--mac)"},
    {"container","set-limits",     cmd_container_set_limits,     "Set container resource limits (--memory_mb/--cpu_quota)"},
    {"container","set-bandwidth",  cmd_container_set_bandwidth,  "Set container bandwidth (--inbound/--outbound)"},
    /* ── Backup Advanced ── */
    {"backup","restore",       cmd_backup_restore,       "Restore VM from backup snapshot"},
    {"backup","incremental",   cmd_backup_incremental,   "Run incremental backup"},
    {"backup","verify",        cmd_backup_verify,        "Verify backup integrity"},
    {"backup","replicate",     cmd_backup_replicate,     "Replicate backup to remote (--target/--user)"},
    {"backup","export-s3",     cmd_backup_export_s3,     "Export backup to S3"},
    /* ── VM Additional ── */
    {"vm","clone",               cmd_vm_clone,               "Clone VM (--mode cow|full, --guest-reset or --template-prepared)"},
    {"vm","pin-vcpu",            cmd_vm_pin_vcpu,            "Pin vCPU to cpuset (--vcpu/--cpuset)"},
    {"vm","snapshot-delete-all", cmd_vm_snapshot_delete_all, "Bulk delete snapshots (--prefix/--keep)"},
    {"vm","export-ova",          cmd_vm_export_ova,          "Export VM to OVA (--output-dir)"},
    {"vm","import-ova",          cmd_vm_import_ova,          "Import VM from OVA (<ova_path> <name>)"},
    {"vm","usb-list",            cmd_vm_usb_list,            "List USB hostdevs attached to VM"},
    {"vm","usb-attach",          cmd_vm_usb_attach,          "Attach USB host device (<vendor_id> <product_id>)"},
    {"vm","usb-detach",          cmd_vm_usb_detach,          "Detach USB host device (<vendor_id> <product_id>)"},
    /* ── Monitor / QoS / Misc ── */
    {"monitor","processes",  cmd_monitor_processes,  "Top processes (--type/--top)"},
    {"network","qos-set",    cmd_network_qos_set,    "Set network QoS (--rate)"},
    {"network","qos-get",    cmd_network_qos_get,    "Get network QoS"},
    {"network","qos-remove", cmd_network_qos_remove, "Remove network QoS"},
    {"healing","history",    cmd_healing_history,     "Self-healing action history"},
    {"gpu","list",           cmd_gpu_list,            "List GPUs (lspci)"},
    {NULL,NULL,NULL,NULL}
};

/* ════════════════════════════════════════════════════════════════════
 *  도움말
 * ════════════════════════════════════════════════════════════════════ */

void print_help(const char *filter) {
    print_cyber_banner();
    printf("%sUsage: pcvctl [FLAGS] <object> <action> [args...]%s\n\n",
        cc(CYBER_YELLOW), cc(CYBER_RESET));
    printf("%sGlobal Flags:%s\n", cc(CYBER_BOLD), cc(CYBER_RESET));
    printf("  --format=table|json|plain|csv   출력 포맷 (기본: table)\n");
    printf("  --socket=<path>                 UDS 소켓 (기본: %s)\n", DAEMON_SOCK_PATH);
    printf("  --no-color                      ANSI 컬러 비활성\n");
    printf("  --verbose / -v                  RPC 페이로드 출력\n");
    printf("  --interactive / -i              REPL 모드 진입\n");
    printf("  --batch                         stdin 파이프라인 모드\n");
    printf("  --version                       버전 출력\n\n");

    printf("%sAvailable Commands:%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
    printf("──────────────────────────────────────────────────────────────────────────\n");
    const char *prev_obj = "";
    for (int i = 0; routes[i].object != NULL; i++) {
        if (filter && !strstr(routes[i].object, filter) && !strstr(routes[i].action, filter))
            continue;
        if (g_strcmp0(routes[i].object, prev_obj) != 0) {
            printf("\n");
            prev_obj = routes[i].object;
        }
        printf("  %s%-10s%s %-14s │ %s%s%s\n",
            cc(CYBER_YELLOW), routes[i].object, cc(CYBER_RESET),
            routes[i].action,
            cc(CYBER_DIM), routes[i].help_text, cc(CYBER_RESET));
    }
    printf("\n──────────────────────────────────────────────────────────────────────────\n");
}

/* ════════════════════════════════════════════════════════════════════
 *  내부 라우팅 실행 (REPL / Batch 공용)
 * ════════════════════════════════════════════════════════════════════ */

/**
 * route_exec - 명령 라우팅 실행 (REPL / Batch / One-shot 공용)
 *
 * routes[] 배열을 순회하며 argv[0](object) + argv[1](action) 조합이
 * 일치하는 핸들러를 찾아 호출한다.
 *
 * 내장 명령 (routes[] 이전에 처리):
 *   help [filter]  - 도움말 출력 (선택적 필터링)
 *   version        - pcvctl 버전 출력
 *   format <fmt>   - REPL 내 출력 포맷 변경 (json/plain/csv/table)
 *   clear          - 터미널 화면 클리어
 *
 * @argc: 인수 개수 (argv[0] = object, argv[1] = action)
 * @argv: 인수 배열 (main의 argv가 아닌, 플래그 제거 후 커맨드 부분)
 * @return: 0=성공, 1=미매칭 또는 에러
 */
static int route_exec(int argc, char **argv) {
    if (argc < 1) { print_help(NULL); return 0; }

    /* 내장 명령 */
    if (g_strcmp0(argv[0],"help") == 0) {
        print_help(argc > 1 ? argv[1] : NULL);
        return 0;
    }
    if (g_strcmp0(argv[0],"version") == 0) {
        printf("pcvctl %s\n", PCVCTL_VERSION);
        return 0;
    }
    /* format 변경 (REPL 전용) */
    if (g_strcmp0(argv[0],"format") == 0 && argc > 1) {
        if      (g_strcmp0(argv[1],"json")  == 0) g_ctx.fmt = FMT_JSON;
        else if (g_strcmp0(argv[1],"plain") == 0) g_ctx.fmt = FMT_PLAIN;
        else if (g_strcmp0(argv[1],"csv")   == 0) g_ctx.fmt = FMT_CSV;
        else                                       g_ctx.fmt = FMT_TABLE;
        if (g_ctx.fmt == FMT_TABLE)
            printf("%sformat → table%s\n", cc(CYBER_CYAN), cc(CYBER_RESET));
        else
            printf("format → %s\n", argv[1]);
        return 0;
    }
    if (g_strcmp0(argv[0],"clear") == 0) { printf("\033[2J\033[H"); return 0; }

    if (argc < 2) {
        g_printerr("%s[!] UNKNOWN COMMAND: %s%s\n",
            ce(CYBER_RED), argv[0], ce(CYBER_RESET));
        print_help(argv[0]);
        return 1;
    }

    for (int i = 0; routes[i].object != NULL; i++) {
        if (g_strcmp0(argv[0], routes[i].object) == 0 &&
            g_strcmp0(argv[1], routes[i].action) == 0) {
            routes[i].handler(argc, argv);
            return 0;
        }
    }
    g_printerr("%s[!] UNKNOWN COMMAND: %s %s%s\n",
        ce(CYBER_RED), argv[0], argv[1], ce(CYBER_RESET));
    print_help(argv[0]);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 *  Interactive REPL (Sprint H 신규)
 * ════════════════════════════════════════════════════════════════════ */

#ifdef HAVE_READLINE
/* readline 자동완성 — routes[] 기반 동적 생성 */
static char **g_completions = NULL;
static int    g_comp_count  = 0;

static void build_completions(void) {
    /* "object action" 형식 문자열 + 내장 명령 */
    GPtrArray *arr = g_ptr_array_new();
    for (int i = 0; routes[i].object != NULL; i++) {
        g_ptr_array_add(arr,
            g_strdup_printf("%s %s", routes[i].object, routes[i].action));
        /* object 단독도 추가 (첫 토큰 완성) */
        bool dup = false;
        for (guint j = 0; j < arr->len - 1; j++) {
            if (g_str_equal((char *)g_ptr_array_index(arr,j), routes[i].object)) {
                dup = true; break;
            }
        }
        if (!dup) g_ptr_array_insert(arr, 0, g_strdup(routes[i].object));
    }
    const char *builtins[] = {"help","version","format","clear","exit","quit",NULL};
    for (int i = 0; builtins[i]; i++)
        g_ptr_array_add(arr, g_strdup(builtins[i]));
    g_ptr_array_add(arr, NULL);

    g_completions = (char **)g_ptr_array_free(arr, FALSE);
    g_comp_count  = 0;
    for (int i = 0; g_completions[i]; i++) g_comp_count++;
}

static char *_rl_generator(const char *text, int state) {
    static int idx;
    if (!state) idx = 0;
    size_t len = strlen(text);
    while (idx < g_comp_count) {
        const char *c = g_completions[idx++];
        if (strncmp(c, text, len) == 0) return g_strdup(c);
    }
    return NULL;
}

static char **_rl_completion(const char *text, int start __attribute__((unused)),
                              int end __attribute__((unused))) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, _rl_generator);
}
#endif /* HAVE_READLINE */

/**
 * repl_run - readline 기반 대화형 REPL (Read-Eval-Print Loop)
 *
 * 사용자가 "pcvctl -i" 또는 인수 없이 실행하면 이 함수가 호출된다.
 *
 * 동작 흐름:
 *   1. readline 자동완성 초기화 (routes[] 기반 "object action" 완성)
 *   2. ~/.pcvctl_history에서 명령 히스토리 로드
 *   3. "(pcv) >" 프롬프트 표시 → 사용자 입력 대기
 *   4. 입력을 wordexp()로 토큰 분리 → route_exec()에 전달
 *   5. "exit"/"quit" 입력 시 루프 종료 + 히스토리 저장
 *
 * readline 미설치 환경(HAVE_READLINE 미정의)에서는 fgets() 폴백으로 동작한다.
 * 이 경우 자동완성과 히스토리 기능이 없다.
 *
 * @return: 항상 0 (정상 종료)
 */
static int repl_run(void) {
    g_ctx.interactive = true;
    if (g_ctx.fmt == FMT_TABLE) {
        print_cyber_banner();
        printf("%s  Type 'help' for commands │ 'exit' to quit │ Tab to complete%s\n\n",
            cc(CYBER_DIM), cc(CYBER_RESET));
    }

#ifdef HAVE_READLINE
    build_completions();
    rl_attempted_completion_function = _rl_completion;
    rl_bind_key('\t', rl_complete);

    char *hist_path = g_strdup_printf("%s/.pcvctl_history", g_get_home_dir());
    read_history(hist_path);

    char *prompt = g_strdup_printf("%s(pcv)%s %s❯%s ",
        cc(CYBER_CYAN), cc(CYBER_RESET), cc(CYBER_GREEN), cc(CYBER_RESET));
    char *line;
    while ((line = readline(prompt)) != NULL) {
        g_strstrip(line);
        if (!*line) { free(line); continue; }
        if (g_strcmp0(line,"exit") == 0 || g_strcmp0(line,"quit") == 0) {
            free(line); break;
        }
        add_history(line);

        wordexp_t we;
        if (wordexp(line, &we, WRDE_NOCMD | WRDE_UNDEF) == 0 && we.we_wordc > 0) {
            /* argv[0]에 "fake binary" 넣어 route_exec 호환 */
            char **av = g_new0(char *, we.we_wordc + 2);
            av[0] = g_strdup("pcvctl");
            for (size_t i = 0; i < we.we_wordc; i++)
                av[i + 1] = g_strdup(we.we_wordv[i]);
            route_exec((int)we.we_wordc, av + 1);
            g_strfreev(av);
            wordfree(&we);
        }
        free(line);
    }
    g_free(prompt);
    write_history(hist_path);
    g_free(hist_path);
    if (g_completions) { g_strfreev(g_completions); g_completions = NULL; }
    printf("\n%s[ NEURAL LINK SEVERED ]%s\n", cc(CYBER_DIM), cc(CYBER_RESET));
#else
    /* readline 없음: fgets fallback */
    char buf[4096];
    while (1) {
        fprintf(stderr, "(pcv) ❯ ");
        fflush(stderr);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\r\n")] = '\0';
        if (g_strcmp0(buf,"exit")==0 || g_strcmp0(buf,"quit")==0) break;

        wordexp_t we;
        if (wordexp(buf, &we, WRDE_NOCMD | WRDE_UNDEF) == 0 && we.we_wordc > 0) {
            char **av = g_new0(char *, we.we_wordc + 2);
            av[0] = g_strdup("pcvctl");
            for (size_t i = 0; i < we.we_wordc; i++)
                av[i+1] = g_strdup(we.we_wordv[i]);
            route_exec((int)we.we_wordc, av + 1);
            g_strfreev(av);
            wordfree(&we);
        }
    }
#endif
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 *  Batch 모드 (Sprint H 신규)
 * ════════════════════════════════════════════════════════════════════
 *
 *  입력 형식 (stdin or 파일 리다이렉션):
 *    # 주석 (무시)
 *    vm list
 *    vm start myvm
 *    monitor fleet
 *
 *  파이프라인 예:
 *    printf 'vm list\nmonitor fleet\n' | pcvctl --format=plain --batch
 *    pcvctl --format=plain --batch < ops.pcv | grep running
 */
static int batch_run(void) {
    g_ctx.batch = true;
    /* Batch 기본 포맷: 사용자가 명시하지 않은 경우 plain */
    if (g_ctx.fmt == FMT_TABLE) g_ctx.fmt = FMT_PLAIN;

    char buf[4096];
    int  lineno   = 0;
    int  total_rc = 0;

    while (fgets(buf, sizeof(buf), stdin)) {
        lineno++;
        buf[strcspn(buf, "\r\n")] = '\0';
        g_strstrip(buf);
        if (!*buf || buf[0] == '#') continue;

        if (g_ctx.verbose)
            g_printerr("%s[batch:%d]%s %s\n",
                ce(CYBER_DIM), lineno, ce(CYBER_RESET), buf);

        wordexp_t we;
        int rc = 0;
        if (wordexp(buf, &we, WRDE_NOCMD | WRDE_UNDEF) == 0 && we.we_wordc > 0) {
            char **av = g_new0(char *, we.we_wordc + 2);
            av[0] = g_strdup("pcvctl");
            for (size_t i = 0; i < we.we_wordc; i++)
                av[i+1] = g_strdup(we.we_wordv[i]);
            rc = route_exec((int)we.we_wordc, av + 1);
            g_strfreev(av);
            wordfree(&we);
        }
        if (rc != 0) {
            g_printerr("%s[ERR line %d]%s %s\n",
                ce(CYBER_RED), lineno, ce(CYBER_RESET), buf);
            total_rc = rc;
        }
    }
    return total_rc;
}

/* ════════════════════════════════════════════════════════════════════
 *  진입점
 * ════════════════════════════════════════════════════════════════════ */

/**
 * main - pcvctl CLI 진입점
 *
 * 실행 모드 3가지:
 *   1. 단발 실행 (One-shot): pcvctl vm list
 *      → argv에서 object+action 추출 → route_exec() 1회 호출 → 종료
 *   2. 대화형 REPL: pcvctl -i  또는  pcvctl (인수 없이 tty에서)
 *      → repl_run() 진입 → "exit" 입력까지 반복
 *   3. 배치 모드: pcvctl --batch  또는  echo "vm list" | pcvctl
 *      → batch_run() 진입 → stdin EOF까지 줄 단위 실행
 *
 * 글로벌 플래그 (모든 모드 공통):
 *   --format=table|json|plain|csv  출력 포맷 선택
 *   --socket=<path>                UDS 소켓 경로 오버라이드
 *   --no-color                     ANSI 컬러 비활성화
 *   --verbose / -v                 RPC 페이로드 stderr 출력
 *   --interactive / -i             REPL 모드 강제 진입
 *   --batch                        배치 모드 강제 진입
 *
 * 플래그 파싱 후 cmd_start 인덱스가 실제 커맨드 시작 위치를 가리킨다.
 * argv[cmd_start]가 object, argv[cmd_start+1]이 action이 된다.
 */
int main(int argc, char *argv[]) {
    /* ── 글로벌 플래그 파싱 ─────────────────────────────────────── */
    int cmd_start = 1;  /* 플래그가 아닌 첫 인수의 인덱스 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (g_strcmp0(a,"--version") == 0) {
            printf("pcvctl %s (%s)\n", PCVCTL_VERSION,
                "Single Edge"
            );
#ifdef HAVE_READLINE
            printf("readline: enabled\n");
#else
            printf("readline: disabled\n");
#endif
            return EXIT_SUCCESS;
        }
        if (g_strcmp0(a,"--help") == 0 || g_strcmp0(a,"-h") == 0) {
            print_help(NULL); return EXIT_SUCCESS;
        }
        if (g_strcmp0(a,"--interactive") == 0 || g_strcmp0(a,"-i") == 0) {
            g_ctx.interactive = true; cmd_start = i + 1; continue;
        }
        if (g_strcmp0(a,"--batch") == 0) {
            g_ctx.batch = true; cmd_start = i + 1; continue;
        }
        if (g_strcmp0(a,"--no-color") == 0) {
            g_ctx.no_color = true; cmd_start = i + 1; continue;
        }
        if (g_strcmp0(a,"--verbose") == 0 || g_strcmp0(a,"-v") == 0) {
            g_ctx.verbose = true; cmd_start = i + 1; continue;
        }
        if (g_str_has_prefix(a,"--format=")) {
            const char *f = a + strlen("--format=");
            if      (g_strcmp0(f,"json")  == 0) g_ctx.fmt = FMT_JSON;
            else if (g_strcmp0(f,"plain") == 0) g_ctx.fmt = FMT_PLAIN;
            else if (g_strcmp0(f,"csv")   == 0) g_ctx.fmt = FMT_CSV;
            else                                g_ctx.fmt = FMT_TABLE;
            cmd_start = i + 1; continue;
        }
        if (g_str_has_prefix(a,"--socket=")) {
            g_ctx.socket_path = a + strlen("--socket=");
            cmd_start = i + 1; continue;
        }
        /* 플래그가 아닌 첫 인수 → 커맨드 시작 */
        cmd_start = i;
        break;
    }

    /* stdin 리다이렉션 감지 → 자동 batch */
    if (!isatty(STDIN_FILENO) && !g_ctx.interactive && cmd_start >= argc)
        g_ctx.batch = true;

    /* ── 모드 분기 ──────────────────────────────────────────────── */
    if (g_ctx.interactive) return repl_run();
    if (g_ctx.batch)       return batch_run();

    /* 인수 없이 tty → 인터랙티브 */
    if (cmd_start >= argc) {
        if (isatty(STDIN_FILENO)) return repl_run();
        print_help(NULL);
        return EXIT_FAILURE;
    }

    /* ── 일반 단일 명령 ─────────────────────────────────────────── */
    /* argv 배열을 routes 기준으로 맞춤: argv[cmd_start] = object */
    /* route_exec 는 argv[0]=object 기준으로 동작 */
    return route_exec(argc - cmd_start, argv + cmd_start)
               ? EXIT_FAILURE : EXIT_SUCCESS;
}
