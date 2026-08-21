                                                                                              
                                                                                               
                                                                                           
                                                                   
                      
   
                              
                                                           
   
#include <glib.h>
#include "modules/dispatcher/handler_monitor.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"

static void
test_success_builder_is_observed(void)
{
    pcv_rpc_completion_begin("vm.info");
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, "ok");
    gchar *response = pure_rpc_build_success_response("1", node);
    g_assert_nonnull(response);
    g_free(response);

    PcvRpcCompletionResult result = pcv_rpc_completion_finish();
    g_assert_true(result.response_observed);
    g_assert_true(result.success);
    g_assert_cmpint(result.error_code, ==, 0);
}

static void
test_error_builder_preserves_code(void)
{
    pcv_rpc_completion_begin("device.nic.attach");
    gchar *response = pure_rpc_build_error_response(
        "2", PURE_RPC_ERR_ZFS_OPERATION, "No more available PCI slots");
    g_assert_nonnull(response);
    g_free(response);

    PcvRpcCompletionResult result = pcv_rpc_completion_finish();
    g_assert_true(result.response_observed);
    g_assert_false(result.success);
    g_assert_cmpint(result.error_code, ==, PURE_RPC_ERR_ZFS_OPERATION);
}

static void
test_direct_audit_requires_exact_method(void)
{
    pcv_rpc_completion_begin("suricata.policy.set");
    pcv_rpc_completion_note_audit("suricata.policy.changed");
    pcv_rpc_completion_note_audit("suricata.policy.set");
    PcvRpcCompletionResult result = pcv_rpc_completion_finish();
    g_assert_true(result.direct_audit);

    pcv_rpc_completion_begin("suricata.policy.set");
    pcv_rpc_completion_note_audit("suricata.policy.changed");
    result = pcv_rpc_completion_finish();
    g_assert_false(result.direct_audit);
}

static void
test_unobserved_scope_fails_safe_and_finish_clears(void)
{
    pcv_rpc_completion_begin("plugin.broken");
    PcvRpcCompletionResult result = pcv_rpc_completion_finish();
    g_assert_false(result.response_observed);
    g_assert_false(result.success);
    g_assert_cmpint(result.error_code, ==, PURE_RPC_ERR_INTERNAL_ERROR);

    result = pcv_rpc_completion_finish();
    g_assert_false(result.response_observed);
    g_assert_false(result.direct_audit);
}

static void
test_dispatch_result_preserves_async_pending_contract(void)
{
    PcvRpcCompletionResult pending = {0};
    g_assert_false(pcv_rpc_completion_dispatch_succeeded(pending, FALSE));
    g_assert_true(pcv_rpc_completion_dispatch_succeeded(pending, TRUE));

    PcvRpcCompletionResult immediate_error = {
        .response_observed = TRUE,
        .success = FALSE,
        .error_code = PURE_RPC_ERR_INVALID_PARAMS,
    };
    g_assert_false(pcv_rpc_completion_dispatch_succeeded(immediate_error, TRUE));

    PcvRpcCompletionResult immediate_success = {
        .response_observed = TRUE,
        .success = TRUE,
        .error_code = 0,
    };
    g_assert_true(pcv_rpc_completion_dispatch_succeeded(immediate_success, TRUE));
}

static void
test_monitor_fleet_preserves_id_and_records_success(void)
{
    const gchar *rpc_id = "client-monitor-request-47";
    JsonArray *fleet = json_array_new();
    JsonObject *vm = json_object_new();
    json_object_set_string_member(vm, "name", "fixture-vm");
    json_array_add_object_element(fleet, vm);
    JsonObject *host = json_object_new();
    json_object_set_int_member(host, "cpus", 8);

    pcv_rpc_completion_begin("monitor.fleet");
    gchar *response = pcv_monitor_fleet_build_success_response(rpc_id, fleet, host);
    g_assert_nonnull(response);

    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, response, -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(json_object_get_string_member(root, "jsonrpc"), ==, "2.0");
    g_assert_cmpstr(json_object_get_string_member(root, "id"), ==, rpc_id);
    g_assert_false(json_object_has_member(root, "error"));
    JsonObject *result_obj = json_object_get_object_member(root, "result");
    g_assert_cmpuint(json_array_get_length(
        json_object_get_array_member(result_obj, "fleet")), ==, 1);
    g_assert_cmpint(json_object_get_int_member(
        json_object_get_object_member(result_obj, "host"), "cpus"), ==, 8);
    g_object_unref(parser);
    g_free(response);

    PcvRpcCompletionResult completion = pcv_rpc_completion_finish();
    g_assert_true(completion.response_observed);
    g_assert_true(completion.success);
    g_assert_cmpint(completion.error_code, ==, 0);
                                                             
                                            
    g_assert_false(completion.direct_audit);
    gboolean success = pcv_rpc_completion_dispatch_succeeded(completion, FALSE);
    gint audit_code = success ? 0
        : (completion.response_observed
            ? completion.error_code : PURE_RPC_ERR_INTERNAL_ERROR);
    g_assert_true(success);
    g_assert_cmpstr(success ? "ok" : "fail", ==, "ok");
    g_assert_cmpint(audit_code, ==, 0);
}

void
test_rpc_completion_register(void)
{
    g_test_add_func("/rpc_completion/success_builder_observed",
                    test_success_builder_is_observed);
    g_test_add_func("/rpc_completion/error_builder_code",
                    test_error_builder_preserves_code);
    g_test_add_func("/rpc_completion/direct_audit_exact_method",
                    test_direct_audit_requires_exact_method);
    g_test_add_func("/rpc_completion/unobserved_fail_safe",
                    test_unobserved_scope_fails_safe_and_finish_clears);
    g_test_add_func("/rpc_completion/async_pending_dispatch_contract",
                    test_dispatch_result_preserves_async_pending_contract);
    g_test_add_func("/rpc_completion/monitor_fleet_exact_id_and_audit_success",
                    test_monitor_fleet_preserves_id_and_records_success);
}
