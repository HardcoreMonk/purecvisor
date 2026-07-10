/**
 * @file cli_output.c
 * @brief CLI 출력 포맷터 — 배너, 메트릭 바, PcvTable 동적 테이블 시스템
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  purecvisorctl.c(CLI 커맨드 파서)에서 분리된 출력 전용 계층입니다.
 *  RPC 응답 JSON을 사람이 읽기 쉬운 형태로 포맷팅합니다.
 *
 *    purecvisorctl.c → cli_rpc.c (RPC 전송) → 데몬 응답
 *         │
 *         └→ cli_output.c (이 파일) → stdout/stderr 출력
 *
 * ============================================================================
 *  출력 형식 (g_ctx.fmt)
 * ============================================================================
 *  - FMT_TABLE: 사이버펑크 스타일 컬러 테이블 (기본, 터미널용)
 *  - FMT_JSON:  원본 JSON 그대로 출력 (--json 플래그, jq 파이핑용)
 *  - FMT_PLAIN: 탭 구분 텍스트 (--plain 플래그, awk/cut 파싱용)
 *  - FMT_CSV:   CSV 헤더+데이터 (--csv 플래그, 스프레드시트 호환)
 *
 * ============================================================================
 *  주니어 개발자 필독: PcvTable 시스템
 * ============================================================================
 *  PcvTable은 동적 컬럼 정렬 테이블을 위한 간단한 추상화입니다.
 *
 *  사용법:
 *    const char *hdrs[] = {"NAME", "STATE", "VCPU"};
 *    PcvTable *t = ptbl_new(hdrs, 3);          // 테이블 생성
 *    ptbl_row(t, "web-prod", "running", "4");   // 행 추가 (가변 인자)
 *    ptbl_row(t, "db-main", "stopped", "8");
 *    ptbl_print_plain(t);  // 또는 ptbl_print_csv(t)
 *    ptbl_free(t);                              // 메모리 해제
 *
 *  내부 구조:
 *    - rows는 GPtrArray<gchar**>로 저장 (각 행은 ncols개 gchar* 배열)
 *    - g_strfreev로 각 행을 자동 해제 (GPtrArray free_func)
 *    - FMT_TABLE일 때는 purecvisorctl.c에서 직접 컬럼 폭 계산 후 printf
 *    - FMT_PLAIN/CSV일 때는 ptbl_print_plain()/ptbl_print_csv() 사용
 *
 *  CSV 이스케이프:
 *    값에 쉼표(,), 큰따옴표("), 개행(\n)이 포함되면 큰따옴표로 감싸고
 *    내부 큰따옴표는 ""로 이스케이프합니다 (RFC 4180 준수).
 * ============================================================================
 */
#include "cli_output.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ── 배너 ──────────────────────────────────────────────────────────── */

/**
 * print_cyber_banner — CLI 시작 시 ASCII 아트 배너를 출력
 *
 * [호출 시점] interactive(REPL) 모드 진입 시 1회 호출
 * [동작] FMT_TABLE 모드일 때만 사이버펑크 컬러 ASCII 아트를 출력.
 *        JSON/PLAIN/CSV 모드에서는 출력하지 않습니다 (파싱 방해 방지).
 */
void print_cyber_banner(void) {
    if (g_ctx.fmt != FMT_TABLE) return;
    printf("%s", cc(CYBER_BOLD));
    printf("%s ___  %s_   _  ___  ___  %s___  _ _  %s_  ___  ___  ___ \n",
        cc(CYBER_BLUE), cc(CYBER_PINK), cc(CYBER_BLUE), cc(CYBER_PINK));
    printf("%s| . \\%s| | | || . \\| __>%s|  _>| | |%s| |/ __>/ . \\| . \\\n",
        cc(CYBER_BLUE), cc(CYBER_PINK), cc(CYBER_BLUE), cc(CYBER_PINK));
    printf("%s|  /%s| |_| ||   /| _> %s| <__| V |%s| |\\__ \\| | ||   /\n",
        cc(CYBER_BLUE), cc(CYBER_PINK), cc(CYBER_BLUE), cc(CYBER_PINK));
    printf("%s|_|  %s\\___/ |_|_\\<___>%s\\___/ \\_/ %s|_|<___/\\___/|_|_\\\n",
        cc(CYBER_BLUE), cc(CYBER_PINK), cc(CYBER_BLUE), cc(CYBER_PINK));
    printf("%s            [ NEURAL LINK ESTABLISHED ]            \n%s\n",
        cc(CYBER_CYAN), cc(CYBER_RESET));
}

/**
 * print_metrics_bar — 수평 퍼센트 바를 터미널에 출력 (▰▰▰▰▱▱▱▱ 형태)
 *
 * [호출 시점] `pcvctl status` 등 호스트 메트릭 표시 시
 * [동작] FMT_TABLE: [ CPU      ] ▰▰▰▰▰▰▰▰▱▱▱▱▱▱▱▱▱▱▱▱  40%
 *        FMT_PLAIN/CSV: "CPU\t40" (탭 구분, 기계 파싱용)
 *
 * @param label   메트릭 이름 (예: "CPU", "MEMORY")
 * @param percent 퍼센트 값 (0~100, 범위 밖은 클램핑)
 * @param color   ANSI 컬러 이스케이프 코드 (FMT_TABLE용)
 */
void print_metrics_bar(const char *label, int percent, const char *color) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        printf("%s\t%d\n", label, percent);
        return;
    }
    printf("%s[ %-8s ] %s%s", cc(CYBER_CYAN), label, cc(CYBER_RESET), color);
    for (int i = 0; i < 20; i++) {
        if (i < percent / 5) printf("\xe2\x96\xb0");
        else printf("%s\xe2\x96\xb1%s%s", cc(CYBER_DIM), cc(CYBER_RESET), color);
    }
    printf(" %3d%%%s\n", percent, cc(CYBER_RESET));
}

/* ── 액션 응답 출력 ────────────────────────────────────────────────── */

/** print_raw_response — JSON 응답 문자열을 그대로 stdout에 출력 (FMT_JSON용) */
void print_raw_response(const gchar *json_string) {
    if (json_string) printf("%s\n", json_string);
}

/**
 * print_action_response — RPC 액션(start/stop/create 등) 응답을 출력 형식에 맞게 표시
 *
 * [호출 시점] CLI의 상태 변경 커맨드(vm start, vm stop 등) 실행 후
 * [동작]
 *   - FMT_JSON: 원본 JSON 그대로 출력
 *   - FMT_PLAIN/CSV: "action_name\tOK" 또는 "action_name\tERROR" 한 줄 출력
 *   - FMT_TABLE: JSON을 파싱하여:
 *     - error 필드 → "[!] COMMAND REJECTED [code]: message" (stderr, 빨강)
 *     - result 필드 → "[+] ACTION COMMAND ACCEPTED: [status]" (초록+사이버펑크)
 *     - DHCP 경고가 있으면 추가 경고 출력
 *
 * @param json_string RPC 응답 JSON 문자열 (NULL이면 에러 표시)
 * @param action_name 액션 이름 (예: "VM_START", "NETWORK_DELETE")
 */
void print_action_response(const gchar *json_string, const gchar *action_name) {
    if (!json_string) {
        g_printerr("%s[!] NULL RESPONSE%s\n", ce(CYBER_RED), ce(CYBER_RESET));
        return;
    }
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(json_string);
        return;
    }
    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
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

    /* FMT_TABLE */
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
        /* B7-M3: has_member != object — null-safe 접근 */
        JsonObject *err_obj = json_object_get_object_member(root_obj, "error");
        if (err_obj) {
            g_printerr("%s[!] COMMAND REJECTED [%lld]: %s%s\n",
                ce(CYBER_RED),
                (long long)json_object_get_int_member(err_obj, "code"),
                json_object_get_string_member(err_obj, "message"),
                ce(CYBER_RESET));
        } else {
            g_printerr("%s[!] COMMAND REJECTED: malformed error response%s\n",
                ce(CYBER_RED), ce(CYBER_RESET));
        }
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

/* ── PcvTable ──────────────────────────────────────────────────────── */

/**
 * ptbl_new — 동적 테이블 객체를 생성
 *
 * [동작] PcvTable 구조체 할당 + 헤더 참조 저장 + 빈 rows GPtrArray 생성
 *        rows의 free_func으로 g_strfreev를 설정하여 자동 메모리 해제
 *
 * @param hdrs 컬럼 헤더 문자열 배열 (호출자가 수명 관리, ptbl_free까지 유효해야 함)
 * @param n    컬럼 수
 * @return 새 PcvTable 포인터 (ptbl_free로 해제 필요)
 */
PcvTable *ptbl_new(const char **hdrs, size_t n) {
    PcvTable *t = g_new0(PcvTable, 1);
    t->headers = hdrs;
    t->ncols   = n;
    t->rows    = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    return t;
}

/**
 * ptbl_row — 테이블에 한 행을 추가 (가변 인자: ncols개의 const char*)
 *
 * [동작] va_list로 ncols개 문자열 인자를 읽어 gchar** 배열로 복사 후 rows에 추가
 * [주의] 인자 수가 ncols와 정확히 일치해야 합니다 (컴파일러가 검증하지 않음!)
 *        NULL 인자는 빈 문자열("")로 대체됩니다.
 */
void ptbl_row(PcvTable *t, ...) {
    va_list ap; va_start(ap, t);
    gchar **row = g_new0(gchar *, t->ncols + 1);
    for (size_t i = 0; i < t->ncols; i++) {
        const char *v = va_arg(ap, const char *);
        row[i] = g_strdup(v ? v : "");
    }
    va_end(ap);
    g_ptr_array_add(t->rows, row);
}

/** ptbl_print_plain — 탭 구분 텍스트로 테이블 출력 (헤더 없음, FMT_PLAIN용) */
void ptbl_print_plain(PcvTable *t) {
    for (guint r = 0; r < t->rows->len; r++) {
        gchar **row = g_ptr_array_index(t->rows, r);
        for (size_t c = 0; c < t->ncols; c++) {
            if (c) putchar('\t');
            fputs(row[c], stdout);
        }
        putchar('\n');
    }
}

/**
 * ptbl_print_csv — RFC 4180 준수 CSV 형식으로 테이블 출력 (헤더 포함)
 *
 * [동작] 헤더 행 출력 → 데이터 행 출력
 *        값에 쉼표/큰따옴표/개행이 있으면 큰따옴표로 감싸고, 내부 "는 ""로 이스케이프
 */
void ptbl_print_csv(PcvTable *t) {
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

/** ptbl_free — PcvTable 메모리 해제 (rows GPtrArray + PcvTable 구조체) */
void ptbl_free(PcvTable *t) {
    g_ptr_array_free(t->rows, TRUE);
    g_free(t);
}
