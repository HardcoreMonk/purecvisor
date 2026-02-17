/* src/api/dispatcher.c */
#include "dispatcher.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

/* --- Constants: JSON-RPC Error Codes --- */
#define ERR_PARSE_ERROR      -32700
#define ERR_INVALID_REQUEST  -32600
#define ERR_METHOD_NOT_FOUND -32601
#define ERR_INVALID_PARAMS   -32602
#define ERR_INTERNAL_ERROR   -32603
#define ERR_SERVER_ERROR     -32000 // Implementation defined

/* --- Internal Structures --- */

struct _Dispatcher {
    GHashTable *registry;
    VmManager *vm_manager;
};

typedef void (*CommandHandler)(Dispatcher *self, JsonObject *params, GOutputStream *out);

/* --- Helper: Standardized Response Generator --- */

// 비동기 전송 완료 콜백 (리소스 해제용)
static void on_write_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GOutputStream *out = G_OUTPUT_STREAM(source);
    GError *error = NULL;
    g_output_stream_write_all_finish(out, res, NULL, &error);
    if (error) {
        g_warning("Response write failed: %s", error->message);
        g_error_free(error);
    }
    g_free(user_data); // JSON 문자열 해제
    g_object_unref(out); // Stream 참조 해제 (전송 끝)
}

// 실제 전송 로직
static void send_raw_json(GOutputStream *out, JsonBuilder *builder) {
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);
    gchar *final_msg = g_strdup_printf("%s\n", json_str); // Line-delimited

    // Fire-and-forget async write
    // 여기서 out의 참조 카운트를 하나 가져갑니다 (on_write_finished에서 해제)
    g_output_stream_write_all_async(out, final_msg, strlen(final_msg),
                                    G_PRIORITY_DEFAULT, NULL,
                                    on_write_finished, final_msg);

    g_free(json_str);
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);
}

// [Refactoring] 성공 응답 헬퍼
static void reply_success(GOutputStream *out, JsonNode *result_data) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "ok");
    
    json_builder_set_member_name(builder, "result");
    if (result_data) {
        json_builder_add_value(builder, result_data); // Ownership transfer
    } else {
        json_builder_add_null_value(builder);
    }
    
    json_builder_end_object(builder);
    send_raw_json(out, builder);
}

// [Refactoring] 에러 응답 헬퍼
static void reply_error(GOutputStream *out, int code, const gchar *msg) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "error");
    
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, msg);
    json_builder_end_object(builder);
    
    json_builder_end_object(builder);
    send_raw_json(out, builder);
}

/* --- Command Handlers --- */

// CMD: ping
static void cmd_ping(Dispatcher *self, JsonObject *params, GOutputStream *out) {
    (void)self; (void)params;
    
    // 단순 문자열 결과는 Node로 감싸서 전달
    JsonNode *pong = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(pong, "pong");
    
    reply_success(g_object_ref(out), pong);
}

// CMD: vm.list
static void on_vm_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GOutputStream *out = G_OUTPUT_STREAM(user_data);
    GError *error = NULL;
    (void)source; // Unused

    JsonNode *result = vm_manager_list_domains_finish(NULL, res, &error);

    if (error) {
        reply_error(out, ERR_SERVER_ERROR, error->message);
        g_error_free(error);
    } else {
        reply_success(out, result);
    }
    // Note: out is unref-ed inside reply_* via on_write_finished logic 
    // BUT wait, reply_* logic takes ownership via send_raw_json? 
    // Let's simplify: on_vm_list_finished owns 'out' passed via user_data.
    // reply_* functions will perform async write and unref eventually.
    // However, to be safe and consistent with previous code:
    // The previous code did manual unref. 
    // Let's make sure reply_* takes a NEW ref or CONSUMES the current one.
    // For simplicity: send_raw_json takes ownership. We just pass 'out'.
}

static void cmd_vm_list(Dispatcher *self, JsonObject *params, GOutputStream *out) {
    (void)params;

    if (!self->vm_manager) {
        reply_error(g_object_ref(out), ERR_INTERNAL_ERROR, "VmManager not initialized");
        return;
    }

    // Async Call
    // out의 참조를 하나 늘려서 콜백으로 전달
    vm_manager_list_domains_async(self->vm_manager, 
                                  on_vm_list_finished, 
                                  g_object_ref(out));
}

/* --- Dispatcher Core --- */

Dispatcher* dispatcher_new(void) {
    Dispatcher *self = g_new0(Dispatcher, 1);
    self->registry = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(self->registry, "ping", cmd_ping);
    g_hash_table_insert(self->registry, "vm.list", cmd_vm_list);
    
    return self;
}

void dispatcher_free(Dispatcher *self) {
    if (!self) return;
    if (self->registry) g_hash_table_destroy(self->registry);
    g_free(self);
}

void dispatcher_set_vm_manager(Dispatcher *self, VmManager *mgr) {
    self->vm_manager = mgr;
}

void dispatcher_process_line(Dispatcher *self, GIOStream *stream, const gchar *line) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    GOutputStream *out = g_io_stream_get_output_stream(stream);

    if (!json_parser_load_from_data(parser, line, -1, &error)) {
        reply_error(g_object_ref(out), ERR_PARSE_ERROR, "JSON Parse Error");
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        reply_error(g_object_ref(out), ERR_INVALID_REQUEST, "Root must be an object");
        g_object_unref(parser);
        return;
    }

    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "method")) {
        reply_error(g_object_ref(out), ERR_INVALID_REQUEST, "Missing 'method'");
        g_object_unref(parser);
        return;
    }

    const gchar *method = json_object_get_string_member(root_obj, "method");
    JsonObject *params = NULL;
    if (json_object_has_member(root_obj, "params")) {
        JsonNode *pnode = json_object_get_member(root_obj, "params");
        if (JSON_NODE_HOLDS_OBJECT(pnode)) {
            params = json_object_get_object_member(root_obj, "params");
        }
    }

    CommandHandler handler = g_hash_table_lookup(self->registry, method);
    if (handler) {
        handler(self, params, out);
    } else {
        reply_error(g_object_ref(out), ERR_METHOD_NOT_FOUND, "Method not found");
    }

    g_object_unref(parser);
}