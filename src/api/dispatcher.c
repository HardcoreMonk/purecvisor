#include "dispatcher.h"
#include "../modules/virt/vm_manager.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>

/* ------------------------------------------------------------------------
 * Internal Structures
 * ------------------------------------------------------------------------ */

struct _PureCVisorDispatcher {
    GObject parent_instance;
    PureCVisorVmManager *vm_manager;
};

G_DEFINE_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, G_TYPE_OBJECT)

typedef struct {
    PureCVisorDispatcher *dispatcher;
    GOutputStream *output_stream;
    JsonNode *request_id;
} DispatcherRequestContext;

/* ------------------------------------------------------------------------
 * Helper Functions
 * ------------------------------------------------------------------------ */

static void
dispatcher_request_context_free(DispatcherRequestContext *ctx) {
    if (ctx->output_stream) g_object_unref(ctx->output_stream);
    if (ctx->request_id) json_node_unref(ctx->request_id);
    g_free(ctx);
}

static void
send_json_response(DispatcherRequestContext *ctx, JsonNode *result, GError *error) {
    JsonBuilder *builder = json_builder_new();
    
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");

    if (error) {
        json_builder_set_member_name(builder, "error");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "code");
        json_builder_add_int_value(builder, error->code ? error->code : -32603);
        json_builder_set_member_name(builder, "message");
        json_builder_add_string_value(builder, error->message);
        json_builder_end_object(builder);
    } else {
        json_builder_set_member_name(builder, "result");
        if (result) {
            /* [FIX] add_node -> add_value */
            json_builder_add_value(builder, json_node_copy(result));
        } else {
            json_builder_add_string_value(builder, "OK");
        }
    }

    if (ctx->request_id) {
        json_builder_set_member_name(builder, "id");
        json_builder_add_value(builder, json_node_copy(ctx->request_id));
    } else {
        json_builder_set_member_name(builder, "id");
        json_builder_add_null_value(builder);
    }

    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    
    gsize len;
    gchar *data = json_generator_to_data(gen, &len);
    
    GError *write_err = NULL;
    g_output_stream_write_all(ctx->output_stream, data, len, NULL, NULL, &write_err);
    if (!write_err) {
        g_output_stream_write_all(ctx->output_stream, "\n", 1, NULL, NULL, NULL);
    } else {
        g_warning("Failed to write response: %s", write_err->message);
        g_error_free(write_err);
    }

    g_free(data);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(builder);
}

/* ------------------------------------------------------------------------
 * Callbacks & Handlers (No changes required in logic)
 * ------------------------------------------------------------------------ */

static void on_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = user_data;
    GError *error = NULL;
    if (!purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        send_json_response(ctx, NULL, error);
        g_error_free(error);
    } else {
        JsonNode *ret = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(ret, "created");
        send_json_response(ctx, ret, NULL);
        json_node_unref(ret);
    }
    dispatcher_request_context_free(ctx);
}

static void on_start_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = user_data;
    GError *error = NULL;
    if (!purecvisor_vm_manager_start_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        send_json_response(ctx, NULL, error);
        g_error_free(error);
    } else {
        JsonNode *ret = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(ret, "started");
        send_json_response(ctx, ret, NULL);
        json_node_unref(ret);
    }
    dispatcher_request_context_free(ctx);
}

static void on_stop_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = user_data;
    GError *error = NULL;
    if (!purecvisor_vm_manager_stop_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        send_json_response(ctx, NULL, error);
        g_error_free(error);
    } else {
        JsonNode *ret = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(ret, "stopped");
        send_json_response(ctx, ret, NULL);
        json_node_unref(ret);
    }
    dispatcher_request_context_free(ctx);
}

static void on_delete_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = user_data;
    GError *error = NULL;
    if (!purecvisor_vm_manager_delete_vm_finish(PURECVISOR_VM_MANAGER(source), res, &error)) {
        send_json_response(ctx, NULL, error);
        g_error_free(error);
    } else {
        JsonNode *ret = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(ret, "deleted");
        send_json_response(ctx, ret, NULL);
        json_node_unref(ret);
    }
    dispatcher_request_context_free(ctx);
}

static void on_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = user_data;
    GError *error = NULL;
    JsonNode *result = purecvisor_vm_manager_list_vms_finish(PURECVISOR_VM_MANAGER(source), res, &error);
    if (error) {
        send_json_response(ctx, NULL, error);
        g_error_free(error);
    } else {
        send_json_response(ctx, result, NULL);
        json_node_unref(result);
    }
    dispatcher_request_context_free(ctx);
}

static void handle_vm_create(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    if (!params || !json_object_has_member(params, "name")) {
        GError *err = g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing parameter: name");
        send_json_response(ctx, NULL, err);
        g_error_free(err);
        dispatcher_request_context_free(ctx);
        return;
    }
    PureCVisorVmConfig config = {0};
    config.name = g_strdup(json_object_get_string_member(params, "name"));
    
    if (json_object_has_member(params, "vcpu")) config.vcpu = json_object_get_int_member(params, "vcpu");
    else config.vcpu = 1;
    if (json_object_has_member(params, "memory_mb")) config.memory_mb = json_object_get_int_member(params, "memory_mb");
    else config.memory_mb = 1024;
    if (json_object_has_member(params, "disk_size_gb")) config.disk_size_gb = json_object_get_int_member(params, "disk_size_gb");
    else config.disk_size_gb = 10;
    if (json_object_has_member(params, "iso_path")) config.iso_path = g_strdup(json_object_get_string_member(params, "iso_path"));

    purecvisor_vm_manager_create_vm_async(self->vm_manager, &config, NULL, on_create_finished, ctx);
    g_free(config.name);
    g_free(config.iso_path);
}

static void handle_vm_start(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    const gchar *name = json_object_get_string_member(params, "name");
    purecvisor_vm_manager_start_vm_async(self->vm_manager, name, NULL, on_start_finished, ctx);
}

static void handle_vm_stop(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    const gchar *name = json_object_get_string_member(params, "name");
    gboolean force = FALSE;
    if (json_object_has_member(params, "force")) force = json_object_get_boolean_member(params, "force");
    purecvisor_vm_manager_stop_vm_async(self->vm_manager, name, force, NULL, on_stop_finished, ctx);
}

static void handle_vm_delete(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    const gchar *name = json_object_get_string_member(params, "name");
    purecvisor_vm_manager_delete_vm_async(self->vm_manager, name, NULL, on_delete_finished, ctx);
}

static void handle_vm_list(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    purecvisor_vm_manager_list_vms_async(self->vm_manager, NULL, on_list_finished, ctx);
}

void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self, JsonNode *request_node, GOutputStream *output) {
    if (json_node_get_node_type(request_node) != JSON_NODE_OBJECT) return;
    JsonObject *root_obj = json_node_get_object(request_node);
    if (!json_object_has_member(root_obj, "method")) return;

    const gchar *method = json_object_get_string_member(root_obj, "method");
    JsonObject *params = NULL;
    if (json_object_has_member(root_obj, "params")) {
        JsonNode *p_node = json_object_get_member(root_obj, "params");
        if (json_node_get_node_type(p_node) == JSON_NODE_OBJECT) params = json_node_get_object(p_node);
    }

    DispatcherRequestContext *ctx = g_new0(DispatcherRequestContext, 1);
    ctx->dispatcher = self;
    ctx->output_stream = g_object_ref(output);
    if (json_object_has_member(root_obj, "id")) ctx->request_id = json_node_copy(json_object_get_member(root_obj, "id"));

    if (g_strcmp0(method, "vm.create") == 0) handle_vm_create(self, params, ctx);
    else if (g_strcmp0(method, "vm.start") == 0) handle_vm_start(self, params, ctx);
    else if (g_strcmp0(method, "vm.stop") == 0) handle_vm_stop(self, params, ctx);
    else if (g_strcmp0(method, "vm.delete") == 0) handle_vm_delete(self, params, ctx);
    else if (g_strcmp0(method, "vm.list") == 0) handle_vm_list(self, params, ctx);
    else {
        GError *err = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Method not found");
        send_json_response(ctx, NULL, err);
        g_error_free(err);
        dispatcher_request_context_free(ctx);
    }
}

static void purecvisor_dispatcher_dispose(GObject *object) {
    PureCVisorDispatcher *self = PURECVISOR_DISPATCHER(object);
    if (self->vm_manager) { g_object_unref(self->vm_manager); self->vm_manager = NULL; }
    G_OBJECT_CLASS(purecvisor_dispatcher_parent_class)->dispose(object);
}
static void purecvisor_dispatcher_class_init(PureCVisorDispatcherClass *klass) { G_OBJECT_CLASS(klass)->dispose = purecvisor_dispatcher_dispose; }
static void purecvisor_dispatcher_init(PureCVisorDispatcher *self) { self->vm_manager = purecvisor_vm_manager_new(); }
PureCVisorDispatcher *purecvisor_dispatcher_new(void) { return g_object_new(PURECVISOR_TYPE_DISPATCHER, NULL); }