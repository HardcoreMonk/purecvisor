/* src/api/dispatcher.c */
#include "dispatcher.h"
#include "../modules/virt/vm_manager.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

/* --- Constants: JSON-RPC Error Codes --- */
#define ERR_PARSE_ERROR      -32700
#define ERR_INVALID_REQUEST  -32600
#define ERR_METHOD_NOT_FOUND -32601
#define ERR_INVALID_PARAMS   -32602
#define ERR_INTERNAL_ERROR   -32603
#define ERR_SERVER_ERROR     -32000 

struct _Dispatcher {
    GHashTable *registry;
    PureCVisorVmManager *vm_manager;
};

/* Function Pointer Type for Commands */
typedef void (*CommandHandler)(Dispatcher *self, JsonNode *params, GOutputStream *out);

/* --- Helper Functions (Response Generators) --- */

static void on_write_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GOutputStream *out = G_OUTPUT_STREAM(source);
    GError *error = NULL;
    g_output_stream_write_all_finish(out, res, NULL, &error);
    if (error) {
        g_warning("Response write failed: %s", error->message);
        g_error_free(error);
    }
    g_free(user_data); 
    
    /* [LIFECYCLE] 
     * 여기서 out이 unref될 때, attach된 부모 connection도 같이 unref되어 
     * 소켓이 안전하게 닫힙니다. 
     */
    g_object_unref(out); 
}

static void send_raw_json(GOutputStream *out, JsonBuilder *builder) {
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);
    gchar *final_msg = g_strdup_printf("%s\n", json_str); 

    /* Async write */
    g_output_stream_write_all_async(out, final_msg, strlen(final_msg),
                                    G_PRIORITY_DEFAULT, NULL,
                                    on_write_finished, final_msg);

    g_free(json_str);
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);
}

static void reply_success(GOutputStream *out, JsonNode *result_data) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "ok");
    
    if (result_data) {
        json_builder_set_member_name(builder, "result");
        json_builder_add_value(builder, result_data); 
    }
    
    json_builder_end_object(builder);
    send_raw_json(out, builder);
}

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

/* CMD: ping */
static void cmd_ping(Dispatcher *self, JsonNode *params, GOutputStream *out) {
    (void)self; (void)params;
    JsonNode *pong = json_node_alloc();
    json_node_init_string(pong, "pong");
    reply_success(out, pong); 
}

/* CMD: vm.create */
static void _on_vm_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GOutputStream *out = G_OUTPUT_STREAM(user_data);
    GError *error = NULL;
    
    if (purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        JsonNode *ret = json_node_alloc();
        json_node_init_string(ret, "VM Created Successfully");
        reply_success(out, ret);
    } else {
        reply_error(out, ERR_SERVER_ERROR, error->message);
        g_error_free(error);
    }
}

static void cmd_vm_create(Dispatcher *self, JsonNode *params, GOutputStream *out) {
    if (!self->vm_manager) {
        reply_error(out, ERR_INTERNAL_ERROR, "VmManager not initialized");
        return;
    }
    
    if (!params || !JSON_NODE_HOLDS_OBJECT(params)) {
        reply_error(out, ERR_INVALID_PARAMS, "Params must be a JSON object");
        return;
    }

    purecvisor_vm_manager_create_vm_async(self->vm_manager,
                                          params,
                                          _on_vm_create_finished,
                                          out); 
    /* Dispatcher owns 'out' ref passed from process_line, but we are async.
     * We need to keep 'out' alive. process_line did Ref it. 
     * We pass it as user_data, so we are good. */
}

/* CMD: vm.list */
static void _on_vm_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GOutputStream *out = G_OUTPUT_STREAM(user_data);
    GError *error = NULL;

    JsonNode *result = purecvisor_vm_manager_list_domains_finish(PURECVISOR_VM_MANAGER(source), res, &error);

    if (error) {
        reply_error(out, ERR_SERVER_ERROR, error->message);
        g_error_free(error);
    } else {
        reply_success(out, result); 
    }
}

static void cmd_vm_list(Dispatcher *self, JsonNode *params, GOutputStream *out) {
    (void)params;
    if (!self->vm_manager) {
        reply_error(out, ERR_INTERNAL_ERROR, "VmManager not initialized");
        return;
    }

    purecvisor_vm_manager_list_domains_async(self->vm_manager, 
                                            _on_vm_list_finished, 
                                            out); 
}

/* --- Dispatcher Core --- */

Dispatcher* dispatcher_new(void) {
    Dispatcher *self = g_new0(Dispatcher, 1);
    self->registry = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(self->registry, "ping", cmd_ping);
    g_hash_table_insert(self->registry, "vm.create", cmd_vm_create);
    g_hash_table_insert(self->registry, "vm.list", cmd_vm_list);
    
    return self;
}

void dispatcher_free(Dispatcher *self) {
    if (!self) return;
    if (self->vm_manager) {
        g_object_unref(self->vm_manager);
    }
    if (self->registry) g_hash_table_destroy(self->registry);
    g_free(self);
}

void dispatcher_set_vm_manager(Dispatcher *self, PureCVisorVmManager *mgr) {
    if (self->vm_manager) {
        g_object_unref(self->vm_manager);
    }
    if (mgr) {
        self->vm_manager = g_object_ref(mgr);
    } else {
        self->vm_manager = NULL;
    }
}

void dispatcher_process_line(Dispatcher *self, GIOStream *stream, const gchar *line) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    GOutputStream *out = g_io_stream_get_output_stream(stream);

    /* [CRITICAL FIX] 
     * 1. Output Stream 참조 (비동기 작업을 위해)
     * 2. 부모 Stream (Connection) 참조 및 Attachment
     * -> 이렇게 하면 'out'이 살아있는 동안 'stream'도 강제로 살아있게 됩니다.
     * -> on_write_finished에서 g_object_unref(out)을 하면,
     * attachment도 해제되면서 stream도 unref 되어 소켓이 닫힙니다.
     */
    g_object_ref(out);
    g_object_set_data_full(G_OBJECT(out), "keep_alive_connection", 
                           g_object_ref(stream), g_object_unref);

    if (!json_parser_load_from_data(parser, line, -1, &error)) {
        reply_error(out, ERR_PARSE_ERROR, "JSON Parse Error");
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        reply_error(out, ERR_INVALID_REQUEST, "Root must be an object");
        g_object_unref(parser);
        /* [Error Case Cleanup] 여기서도 out을 풀어줘야 연결이 해제됨 */
        g_object_unref(out);
        return;
    }

    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "method")) {
        reply_error(out, ERR_INVALID_REQUEST, "Missing 'method'");
        g_object_unref(parser);
        g_object_unref(out);
        return;
    }

    const gchar *method = json_object_get_string_member(root_obj, "method");
    JsonNode *params = NULL;
    if (json_object_has_member(root_obj, "params")) {
        params = json_object_get_member(root_obj, "params");
    }

    CommandHandler handler = g_hash_table_lookup(self->registry, method);
    if (handler) {
        handler(self, params, out);
    } else {
        reply_error(out, ERR_METHOD_NOT_FOUND, "Method not found");
        /* Handler가 호출되지 않았으므로 여기서 unref 해야 함? 
         * 아니오, reply_error 내부에서 send_raw_json -> on_write_finished -> unref 흐름을 탐.
         * 따라서 중복 unref 하지 않도록 주의.
         */
    }

    g_object_unref(parser);
}