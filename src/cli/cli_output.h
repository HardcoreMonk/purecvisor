/**
 * @file cli_output.h
 * @brief CLI 출력 포맷팅 함수 선언
 *
 * purecvisorctl.c에서 분리된 출력 포맷팅 계층.
 * 배너, 메트릭 바, 액션 응답, PcvTable 시스템을 제공한다.
 */
#ifndef CLI_OUTPUT_H
#define CLI_OUTPUT_H

#include "cli_rpc.h"
#include <stdarg.h>
#include <glib.h>

/* ── 배너 / 유틸 ──────────────────────────────────────────────────── */
void print_cyber_banner(void);
void print_metrics_bar(const char *label, int percent, const char *color);
void print_raw_response(const gchar *json_string);
void print_action_response(const gchar *json_string, const gchar *action_name);

/* ── PcvTable (PLAIN/CSV 출력용 경량 인메모리 테이블) ───────────────── */
typedef struct {
    const char **headers;
    size_t       ncols;
    GPtrArray   *rows;
} PcvTable;

PcvTable *ptbl_new(const char **hdrs, size_t n);
void ptbl_row(PcvTable *t, ...);
void ptbl_print_plain(PcvTable *t);
void ptbl_print_csv(PcvTable *t);
void ptbl_free(PcvTable *t);

#endif /* CLI_OUTPUT_H */
