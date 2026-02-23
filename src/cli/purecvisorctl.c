/**
 * @file purecvisorctl.c
 * @brief PureCVisor 데몬 제어를 위한 C11 기반 커맨드라인 인터페이스 (CLI)
 * @note Command Table Routing 패턴 적용
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

#define DAEMON_SOCK_PATH "/tmp/purecvisor.sock"

// ANSI 256-Color 네온 색상 팔레트
#define CYBER_CYAN    "\x1b[38;5;51m"
#define CYBER_PINK    "\x1b[38;5;198m"
#define CYBER_YELLOW  "\x1b[38;5;226m"
#define CYBER_GREEN   "\x1b[38;5;46m"
#define CYBER_RED     "\x1b[38;5;196m"
#define CYBER_BLUE    "\x1b[38;5;33m"
#define CYBER_DIM     "\x1b[38;5;240m"
#define CYBER_RESET   "\x1b[0m"
#define CYBER_BOLD    "\x1b[1m"

// =================================================================
// [UI 포매터] 사이버펑크(Cyberpunk) 테마
// =================================================================
void print_cyber_banner() {
    printf(CYBER_BOLD);
    printf(CYBER_BLUE " ___  " CYBER_PINK "_   _  ___  ___  " CYBER_BLUE "___  _ _  " CYBER_PINK "_  ___  ___  ___ \n");
    printf(CYBER_BLUE "| . \\" CYBER_PINK "| | | || . \\| __>" CYBER_BLUE "|  _>| | |" CYBER_PINK "| |/ __>/ . \\| . \\\n");
    printf(CYBER_BLUE "|  _/" CYBER_PINK "| |_| ||   /| _> " CYBER_BLUE "| <__| V |" CYBER_PINK "| |\\__ \\| | ||   /\n");
    printf(CYBER_BLUE "|_|  " CYBER_PINK "\\___/ |_|_\\<___>" CYBER_BLUE "\\___/ \\_/ " CYBER_PINK "|_|<___/\\___/|_|_\\\n");
    printf(CYBER_CYAN "            [ NEURAL LINK ESTABLISHED ]            \n" CYBER_RESET "\n");
}

// =================================================================
// [UI 포매터] 사이버펑크 프로그레스 바 렌더링
// =================================================================
void print_metrics_bar(const char* label, int percent, const char* color) {
    printf(CYBER_CYAN "[ %-8s ] " CYBER_RESET "%s", label, color);
    for(int i = 0; i < 20; i++) {
        if(i < percent / 5) printf("▰");
        else printf(CYBER_DIM "▱" CYBER_RESET "%s", color);
    }
    printf(" %3d%%\n" CYBER_RESET, percent);
}

// =================================================================
// [통신 엔진] JSON-RPC Request/Response
// =================================================================
gchar* purectl_send_request(const gchar *method, JsonObject *params_obj, GError **error) {
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(DAEMON_SOCK_PATH);
    GSocketConnection *connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, error);
    
    g_object_unref(client);
    g_object_unref(addr);
    if (!connection) return NULL;

    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");
    json_object_set_string_member(root_obj, "method", method);
    json_object_set_object_member(root_obj, "params", params_obj ? params_obj : json_object_new());
    json_object_set_int_member(root_obj, "id", 1);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);
    
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root_node);
    gchar *payload = json_generator_to_data(gen, NULL);
    
    json_node_free(root_node);
    g_object_unref(gen);

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gsize bytes_written;
    g_output_stream_write_all(out, payload, strlen(payload), &bytes_written, NULL, error);
    g_free(payload);

    if (*error) {
        g_object_unref(connection);
        return NULL;
    }

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gchar buffer[65536] = {0};
    gssize bytes_read = g_input_stream_read(in, buffer, sizeof(buffer) - 1, NULL, error);
    g_object_unref(connection);

    return (bytes_read > 0) ? g_strdup(buffer) : NULL;
}

// =================================================================
// [파서 엔진] 응답 출력
// =================================================================

void print_action_response(const gchar *json_string, const gchar *action_name) {
    if (!json_string) return; // 🚀 안전 장치: 빈 응답 처리

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    
    if (!json_parser_load_from_data(parser, json_string, -1, &error)) {
        g_printerr(CYBER_RED "[!] SYS_FAULT: %s\n" CYBER_RESET, error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root) { g_object_unref(parser); return; }

    JsonObject *root_obj = json_node_get_object(root);

    if (json_object_has_member(root_obj, "error")) {
        JsonObject *err_obj = json_object_get_object_member(root_obj, "error");
        g_printerr(CYBER_RED "[!] COMMAND REJECTED [%ld]: %s\n" CYBER_RESET, 
            json_object_get_int_member(err_obj, "code"),
            json_object_get_string_member(err_obj, "message"));
    } else if (json_object_has_member(root_obj, "result")) {
        JsonNode *res_node = json_object_get_member(root_obj, "result");
        
        // 🚀 수정: 노드 타입 체크를 더 엄격하게 진행
        if (res_node && JSON_NODE_HOLDS_OBJECT(res_node)) {
            JsonObject *res_obj = json_node_get_object(res_node);
            const gchar *status = json_object_has_member(res_obj, "status") ? 
                                  json_object_get_string_member(res_obj, "status") : "SUCCESS";
            
            printf(CYBER_GREEN CYBER_BOLD "[+] %s COMMAND ACCEPTED: " CYBER_RESET, action_name);
            printf(CYBER_CYAN "Entity state transitioned to " CYBER_RESET);
            printf(CYBER_YELLOW "[ %s ]\n" CYBER_RESET, status);
        } else {
            // Boolean(true/false) 등 단순 값 응답 처리
            printf(CYBER_GREEN CYBER_BOLD "[+] %s SEQUENCE INITIATED SUCCESSFULLY.\n" CYBER_RESET, action_name);
        }
    }

    // 🚀 핵심: parser만 해제하면 그에 딸린 node와 object는 자동으로 관리됩니다.
    g_object_unref(parser);
}


// =================================================================
// [핸들러 구현] 개별 명령어 실행 로직
// =================================================================
void cmd_vm_list(int argc, char *argv[]) {
    GError *error = NULL;
    gchar *response = purectl_send_request("vm.list", json_object_new(), &error);
    if (error) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, error->message); g_error_free(error); return; }

    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, response, -1, NULL);
    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
    JsonArray *result_arr = json_object_get_array_member(root_obj, "result");
    
    print_cyber_banner();
    printf(CYBER_CYAN CYBER_BOLD " %-38s │ %-18s │ %-10s\n" CYBER_RESET, "SYS_UUID", "ENTITY_ID", "LIFELINE");
    printf(CYBER_CYAN "────────────────────────────────────────┼────────────────────┼────────────\n" CYBER_RESET);
    
    if (!result_arr || json_array_get_length(result_arr) == 0) {
        printf(CYBER_DIM " [ NO ACTIVE ENTITIES FOUND IN MAINFRAME ]\n" CYBER_RESET);
    } else {
        for (guint i = 0; i < json_array_get_length(result_arr); i++) {
            JsonObject *vm_obj = json_array_get_object_element(result_arr, i);
            const gchar *uuid = json_object_get_string_member(vm_obj, "uuid");
            const gchar *name = json_object_get_string_member(vm_obj, "name");
            const gchar *state = json_object_get_string_member(vm_obj, "state");
            
            const gchar *sc = CYBER_DIM;
            if (g_strcmp0(state, "running") == 0) sc = CYBER_GREEN;
            else if (g_strcmp0(state, "shutoff") == 0) sc = CYBER_RED;
            else if (g_strcmp0(state, "paused") == 0) sc = CYBER_YELLOW;

            printf(CYBER_DIM " %-38s" CYBER_RESET " │ " CYBER_YELLOW "%-18s" CYBER_RESET " │ %s%-10s" CYBER_RESET "\n", 
                uuid ? uuid : "-", name ? name : "-", sc, state ? state : "unknown");
        }
    }
    printf(CYBER_CYAN "────────────────────────────────────────┴────────────────────┴────────────\n" CYBER_RESET "\n");
    g_object_unref(parser); g_free(response);
}

void cmd_vm_action(int argc, char *argv[], const gchar *method, const gchar *action_name) {
    if (argc < 4) { printf(CYBER_YELLOW "Usage: purecvisorctl vm %s <uuid_or_name>\n" CYBER_RESET, argv[2]); return; }
    
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[3]);
    
    GError *error = NULL;
    gchar *response = purectl_send_request(method, params, &error);
    if (error) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, error->message); g_error_free(error); return; }

    print_action_response(response, action_name);
    g_free(response);
}

void cmd_vm_start(int argc, char *argv[]) { cmd_vm_action(argc, argv, "vm.start", "START"); }
void cmd_vm_stop(int argc, char *argv[]) { cmd_vm_action(argc, argv, "vm.stop", "STOP"); }
void cmd_vm_pause(int argc, char *argv[]) { cmd_vm_action(argc, argv, "vm.pause", "PAUSE"); }

void cmd_net_create(int argc, char *argv[]) {
    if (argc < 4) { printf(CYBER_YELLOW "Usage: purecvisorctl network create <name> [--mode nat|bridge] [--cidr IP] [--iface eth0]\n" CYBER_RESET); return; }
    
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "bridge_name", argv[3]);
    for (int i = 4; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mode") == 0 && i+1 < argc) json_object_set_string_member(params, "mode", argv[++i]);
        else if (g_strcmp0(argv[i], "--cidr") == 0 && i+1 < argc) json_object_set_string_member(params, "cidr", argv[++i]);
        else if (g_strcmp0(argv[i], "--iface") == 0 && i+1 < argc) json_object_set_string_member(params, "physical_if", argv[++i]);
    }
    
    GError *err = NULL;
    gchar *res = purectl_send_request("network.create", params, &err);
    if (err) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, err->message); g_error_free(err); return; }
    print_action_response(res, "NET_CREATE"); g_free(res);
}

void cmd_net_delete(int argc, char *argv[]) {
    if (argc < 4) { printf(CYBER_YELLOW "Usage: purecvisorctl network delete <name>\n" CYBER_RESET); return; }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "bridge_name", argv[3]);
    
    GError *err = NULL;
    gchar *res = purectl_send_request("network.delete", params, &err);
    if (err) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, err->message); g_error_free(err); return; }
    print_action_response(res, "NET_DELETE"); g_free(res);
}

void cmd_vm_limit(int argc, char *argv[]) {
    if (argc < 6) { 
        printf(CYBER_YELLOW "Usage: purecvisorctl vm limit <uuid_or_name> --cpu <percent>\n" CYBER_RESET);
        printf(CYBER_DIM "  * Note: Use -1 to UNSET/REMOVE the limit.\n" CYBER_RESET); // 🚀 힌트 추가
        printf("Example: purecvisorctl vm limit big-vm --cpu 50 --mem 2048\n");
        printf("Example: purecvisorctl vm limit big-vm --cpu -1 --mem -1\n");         
        return; 
    }
    
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[3]);
    
    for (int i = 4; i < argc; i++) {
        if (g_strcmp0(argv[i], "--cpu") == 0 && i+1 < argc) {
            json_object_set_int_member(params, "cpu", atoi(argv[++i]));
        } else if (g_strcmp0(argv[i], "--mem") == 0 && i+1 < argc) {
            json_object_set_int_member(params, "mem", atoi(argv[++i]));
        }
    }
// 파라미터 세팅이 끝난 params 객체를 비로소 데몬으로 전송합니다.
    GError *error = NULL;
    gchar *response = purectl_send_request("vm.limit", params, &error);
    if (error) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, error->message); g_error_free(error); return; }

    print_action_response(response, "RESOURCE_LIMIT");
    g_free(response);
    
}
// =================================================================
// [커맨드 라우팅] 모니터링 (Metrics)
// =================================================================
void cmd_monitor_metrics(int argc, char *argv[]) {
    if (argc < 4) { 
        printf(CYBER_YELLOW "Usage: purecvisorctl monitor metrics <uuid_or_name>\n" CYBER_RESET); return; 
    }
    
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[3]);
    
    GError *error = NULL;
    // 데몬에 vm.metrics API를 요청합니다.
    gchar *response = purectl_send_request("vm.metrics", params, &error);
    if (error) { g_printerr(CYBER_RED "[!] LINK_SEVERED: %s\n" CYBER_RESET, error->message); g_error_free(error); return; }

    // 🚀 JSON 파싱해서 실제 데이터 추출
    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, response, -1, NULL);
    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
    
    // 에러 발생 시 처리
    if (json_object_has_member(root_obj, "error")) {
        g_printerr(CYBER_RED "[!] TELEMETRY FAILED.\n" CYBER_RESET);
        g_object_unref(parser); g_free(response); return;
    }

    // 진짜 퍼센티지 획득!
    JsonObject *res_obj = json_object_get_object_member(root_obj, "result");
    int cpu_pct = json_object_get_int_member(res_obj, "cpu");
    int mem_pct = json_object_get_int_member(res_obj, "mem");

    // UI 데이터 렌더링
    print_cyber_banner();
    printf(CYBER_YELLOW CYBER_BOLD ">>> REALTIME TELEMETRY: %s <<<\n\n" CYBER_RESET, argv[3]);
    
    // 나중에는 response JSON을 파싱해서 실제 퍼센티지를 넣을 것입니다.
    print_metrics_bar("CPU", cpu_pct, CYBER_GREEN);
    print_metrics_bar("MEMORY", mem_pct, CYBER_RED);
    printf("\n");

    g_free(response);
}

// =================================================================
// 🚀 [라우팅 테이블] 구조체 배열 기반 우아한 명령어 라우터
// =================================================================
typedef void (*CmdHandler)(int argc, char *argv[]);

typedef struct {
    const char *object;     // 예: "vm", "network"
    const char *action;     // 예: "start", "create"
    CmdHandler handler;     // 실행할 함수 포인터
    const char *help_text;  // 도움말 출력용
} CommandRoute;

CommandRoute routes[] = {
    {"vm", "list", cmd_vm_list, "List all virtual machines"},
    {"vm", "start", cmd_vm_start, "Start a VM by UUID or Name"},
    {"vm", "stop", cmd_vm_stop, "Stop a VM forcefully"},
    {"vm", "pause", cmd_vm_pause, "Pause a running VM"},
    {"vm", "limit", cmd_vm_limit, "Dynamically limit cgroup resources"}, // 🚀 새로 추가된 줄!
    {"monitor", "metrics", cmd_monitor_metrics, "Show realtime VM resource usage"}, // 🚀 신규 추가
    {"network", "create", cmd_net_create, "Create a network (nat/bridge)"},
    {"network", "delete", cmd_net_delete, "Delete a network"},
    // 💡 새로운 기능이 생기면 이 배열에 한 줄만 추가하면 끝입니다!
    {NULL, NULL, NULL, NULL}
};

void print_help() {
    print_cyber_banner();
    printf(CYBER_YELLOW "Usage: purecvisorctl <object> <action> [args...]\n\n" CYBER_RESET);
    printf(CYBER_CYAN "Available Commands:\n" CYBER_RESET);
    printf("──────────────────────────────────────────────────────────────\n");
    for (int i = 0; routes[i].object != NULL; i++) {
        printf("  %-10s %-10s │ %s\n", routes[i].object, routes[i].action, routes[i].help_text);
    }
    printf("──────────────────────────────────────────────────────────────\n");
}



// =================================================================
// [메인 함수] 라우팅 테이블 검색
// =================================================================
int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_help();
        return EXIT_FAILURE;
    }

    const char *obj = argv[1];
    const char *act = argv[2];

    // 라우팅 테이블을 순회하며 일치하는 명령어를 찾습니다.
    for (int i = 0; routes[i].object != NULL; i++) {
        if (g_strcmp0(obj, routes[i].object) == 0 && g_strcmp0(act, routes[i].action) == 0) {
            routes[i].handler(argc, argv);
            return EXIT_SUCCESS;
        }
    }

    // 일치하는 명령어가 없을 경우
    printf(CYBER_RED "\n[!] UNKNOWN COMMAND: %s %s\n\n" CYBER_RESET, obj, act);
    print_help();
    return EXIT_FAILURE;
}