
#include "cli_output.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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

void print_raw_response(const gchar *json_string) {
    if (json_string) printf("%s\n", json_string);
}

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

PcvTable *ptbl_new(const char **hdrs, size_t n) {
    PcvTable *t = g_new0(PcvTable, 1);
    t->headers = hdrs;
    t->ncols   = n;
    t->rows    = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    return t;
}

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

void ptbl_free(PcvTable *t) {
    g_ptr_array_free(t->rows, TRUE);
    g_free(t);
}
