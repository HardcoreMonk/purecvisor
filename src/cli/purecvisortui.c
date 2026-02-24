// =================================================================
// src/cli/purecvisortui.c
// PureCVisor Neural Telemetry TUI Dashboard (Standalone)
// =================================================================
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

// UI Colors
#define CYBER_RED     "\x1b[38;5;196m"
#define CYBER_GREEN   "\x1b[38;5;46m"
#define CYBER_YELLOW  "\x1b[38;5;226m"
#define CYBER_CYAN    "\x1b[38;5;51m"
#define CYBER_DIM     "\x1b[2m"
#define CYBER_RESET   "\x1b[0m"

#define SOCKET_PATH "/tmp/purecvisor.sock"

static volatile int keep_tui_running = 1;

void tui_sigint_handler(int dummy) {
    keep_tui_running = 0; // Ctrl+C 감지
}

// 🚀 독립 모듈 전용 초경량 UDS 통신 엔진
gchar *tui_send_request(const gchar *method, JsonObject *params, GError **error) {
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(SOCKET_PATH);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, error);
    
    if (!conn) {
        g_object_unref(client); g_object_unref(addr);
        return NULL;
    }

    // JSON RPC 조립
    JsonObject *req = json_object_new();
    json_object_set_string_member(req, "jsonrpc", "2.0");
    json_object_set_string_member(req, "method", method);
    if (params) json_object_set_object_member(req, "params", params);
    else json_object_set_object_member(req, "params", json_object_new());
    json_object_set_string_member(req, "id", "tui-req");

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, req);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gsize len;
    gchar *data = json_generator_to_data(gen, &len);

    // 송수신
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_output_stream_write_all(out, data, len, NULL, NULL, error);
    g_output_stream_write_all(out, "\n", 1, NULL, NULL, error);

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *din = g_data_input_stream_new(in);
    gchar *response = g_data_input_stream_read_line(din, NULL, NULL, error);

    // 메모리 정리
    g_free(data); g_object_unref(gen); json_node_free(root);
    g_object_unref(din); g_object_unref(conn); g_object_unref(addr); g_object_unref(client);

    return response;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf(CYBER_YELLOW "Usage: purecvisortui <vm_id_or_name>\n" CYBER_RESET);
        return 1;
    }

    const gchar *vm_id = argv[1];
    signal(SIGINT, tui_sigint_handler);
    printf("\e[?25l"); // 커서 숨김

    while (keep_tui_running) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "vm_id", vm_id);

        GError *error = NULL;
        gchar *response = tui_send_request("monitor.metrics", params, &error);

        printf("\e[2J\e[H"); // 화면 100% 클리어

        if (error) {
            printf(CYBER_RED "[!] TELEMETRY LINK SEVERED: %s\n" CYBER_RESET, error->message);
            g_error_free(error);
            break;
        }

        JsonParser *parser = json_parser_new();
        json_parser_load_from_data(parser, response, -1, NULL);
        JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));

        if (json_object_has_member(root_obj, "error")) {
            JsonObject *err_obj = json_object_get_object_member(root_obj, "error");
            printf(CYBER_RED "[!] SENSOR FAULT: %s\n" CYBER_RESET, json_object_get_string_member(err_obj, "message"));
        } else if (json_object_has_member(root_obj, "result")) {
            JsonObject *res_obj = json_object_get_object_member(root_obj, "result");
            
            const gchar *state = json_object_get_string_member(res_obj, "state");
            gint vcpu = json_object_get_int_member(res_obj, "vcpu");
            gdouble mem_max = json_object_get_double_member(res_obj, "mem_max_mb");
            gdouble mem_used = json_object_get_double_member(res_obj, "mem_used_mb");
            gint64 cpu_time = json_object_get_int_member(res_obj, "cpu_time_ns");
            
            double mem_percent = (mem_max > 0) ? (mem_used / mem_max) * 100.0 : 0.0;
            gint is_running = (g_strcmp0(state, "RUNNING") == 0);

            // TUI 렌더링 시작
            printf(CYBER_CYAN " ┌──────────────────────────────────────────────────┐\n");
            printf(" │" CYBER_YELLOW "    PURECVISOR NEURAL TELEMETRY LINK ACTIVE       " CYBER_CYAN "│\n");
            printf(" └──────────────────────────────────────────────────┘\n\n" CYBER_RESET);
            
            printf(CYBER_DIM " [ TARGET ]  " CYBER_RESET "%s\n", vm_id);
            printf(CYBER_DIM " [ STATUS ]  " CYBER_RESET "%s%s\n\n" CYBER_RESET, is_running ? CYBER_GREEN : CYBER_RED, state);
            
            printf(CYBER_CYAN " >> CORE & MEMORY MATRIX\n" CYBER_RESET);
            printf("  VCPU ALLOC : " CYBER_GREEN "%d Cores\n" CYBER_RESET, vcpu);
            printf("  MEM USAGE  : " CYBER_YELLOW "%.1f MB" CYBER_RESET " / %.1f MB ( %.1f%% )\n", mem_used, mem_max, mem_percent);
            
            printf("  MEM MATRIX : [");
            int bar_len = 32;
            int filled = (int)((mem_percent / 100.0) * bar_len);
            for (int i = 0; i < bar_len; i++) {
                if (i < filled) printf((mem_percent > 80.0) ? CYBER_RED "█" CYBER_RESET : CYBER_GREEN "█" CYBER_RESET);
                else printf(CYBER_DIM "-" CYBER_RESET);
            }
            printf("]\n\n");

            printf(CYBER_CYAN " >> NARRATIVE ENGINE ANALYSIS\n" CYBER_RESET);
            if (is_running) {
                printf("  > " CYBER_GREEN "[SYSTEM] OS Kernel is breathing normally." CYBER_RESET "\n");
                printf("  > " CYBER_DIM "[PULSE]  Accumulated %ld ns of compute cycles." CYBER_RESET "\n", cpu_time);
                if (mem_percent > 90.0) {
                    printf("  > " CYBER_RED "[DANGER] CRITICAL MEMORY SATURATION. OOM Killer imminent." CYBER_RESET "\n");
                } else if (mem_percent > 70.0) {
                    printf("  > " CYBER_YELLOW "[WARN]   Memory pressure elevating. Monitoring swap." CYBER_RESET "\n");
                } else {
                    printf("  > " CYBER_GREEN "[INFO]   Resource overhead is stable and optimal." CYBER_RESET "\n");
                }
            } else {
                printf("  > " CYBER_RED "[SYSTEM] Entity is dormant. Cold storage engaged." CYBER_RESET "\n");
                printf("  > " CYBER_DIM "[INFO]   Awaiting ignition sequence (vm start)." CYBER_RESET "\n");
            }
        }
        
        g_object_unref(parser);
        g_free(response);
        fflush(stdout);

        usleep(1000000); // 1초 대기
    }

    printf("\e[?25h"); // 커서 복구
    printf("\n" CYBER_DIM " [ SYSTEM ] TELEMETRY LINK SEVERED MANUALLY." CYBER_RESET "\n");
    return 0;
}