






#ifndef CLI_OUTPUT_H
#define CLI_OUTPUT_H

#include "cli_rpc.h"
#include <stdarg.h>
#include <glib.h>


void print_cyber_banner(void);
void print_metrics_bar(const char *label, int percent, const char *color);
void print_raw_response(const gchar *json_string);
void print_action_response(const gchar *json_string, const gchar *action_name);


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

#endif
