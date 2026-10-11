                                                                                              

                                                                                            

                                                                     

                                                             
#include <glib.h>

#include "../src/api/rest_server.h"
#include "../src/modules/dispatcher/rpc_utils.h"

typedef struct {
    guint cert_loads;
    guint tls_contexts;
    guint http_listens;
    guint https_listens;
    const gchar *http_host;
} FakeTransport;

static gboolean
fake_cert(gpointer data, const gchar *endpoint, GError **error)
{
    (void)endpoint; (void)error;
    ((FakeTransport *)data)->cert_loads++;
    return TRUE;
}

static gboolean
fake_context(gpointer data, const gchar *endpoint, GError **error)
{
    (void)endpoint; (void)error;
    ((FakeTransport *)data)->tls_contexts++;
    return TRUE;
}

static gboolean
fake_http(gpointer data, const gchar *endpoint, GError **error)
{
    (void)error;
    FakeTransport *fake = data;
    fake->http_listens++;
    fake->http_host = endpoint;
    return TRUE;
}

static gboolean
fake_https(gpointer data, const gchar *endpoint, GError **error)
{
    (void)endpoint; (void)error;
    ((FakeTransport *)data)->https_listens++;
    return TRUE;
}

static PcvRestTransportOps fake_ops = {
    .load_certificate = fake_cert,
    .create_tls_context = fake_context,
    .listen_plaintext = fake_http,
    .listen_https = fake_https,
};

static void
test_internal_mode_policy(void)
{
    PcvRestTlsMode mode = pcv_rest_tls_mode_from_config(TRUE);
    PcvRestTransportPlan plan = pcv_rest_transport_plan(mode, "all");

    g_assert_cmpint(mode, ==, PCV_REST_TLS_INTERNAL);
    g_assert_true(plan.initialize_tls);
    g_assert_true(plan.load_certificate);
    g_assert_true(plan.create_tls_context);
    g_assert_true(plan.listen_https);
    g_assert_cmpstr(plan.plaintext_bind_mode, ==, "all");
    g_assert_cmpstr(plan.plaintext_host, ==, "0.0.0.0");

    FakeTransport fake = {0};
    PcvRestTransportOutcome outcome = {0};
    g_assert_true(pcv_rest_transport_start(
        &plan, &fake_ops, &fake, &outcome, NULL));
    g_assert_cmpuint(fake.cert_loads, ==, 1);
    g_assert_cmpuint(fake.tls_contexts, ==, 1);
    g_assert_cmpuint(fake.http_listens, ==, 1);
    g_assert_cmpuint(fake.https_listens, ==, 1);
    g_assert_cmpstr(fake.http_host, ==, "0.0.0.0");
}

static void
test_external_mode_policy(void)
{
    PcvRestTlsMode mode = pcv_rest_tls_mode_from_config(FALSE);
    PcvRestTransportPlan plan = pcv_rest_transport_plan(mode, "all");

    g_assert_cmpint(mode, ==, PCV_REST_TLS_EXTERNAL_TERMINATION);
    g_assert_false(plan.initialize_tls);
    g_assert_false(plan.load_certificate);
    g_assert_false(plan.create_tls_context);
    g_assert_false(plan.listen_https);
    g_assert_cmpstr(plan.plaintext_bind_mode, ==, "loopback");
    g_assert_cmpstr(plan.plaintext_host, ==, "127.0.0.1");

    FakeTransport fake = {0};
    PcvRestTransportOutcome outcome = {0};
    g_assert_true(pcv_rest_transport_start(
        &plan, &fake_ops, &fake, &outcome, NULL));
    g_assert_cmpuint(fake.cert_loads, ==, 0);
    g_assert_cmpuint(fake.tls_contexts, ==, 0);
    g_assert_cmpuint(fake.http_listens, ==, 1);
    g_assert_cmpuint(fake.https_listens, ==, 0);
    g_assert_cmpstr(fake.http_host, ==, "127.0.0.1");
}

static void
test_external_mode_health(void)
{
    PcvRestTlsHealth health =
        pcv_rest_tls_health(PCV_REST_TLS_EXTERNAL_TERMINATION,
                            FALSE, TRUE, "ignored internal failure");

    g_assert_cmpstr(health.mode, ==, "external_termination");
    g_assert_false(health.enabled);
    g_assert_false(health.degraded);
    g_assert_cmpstr(health.status, ==, "disabled_by_config");
    g_assert_cmpstr(health.reason, ==,
                    "external TLS termination required");
}

static void
test_rest_transport_contract(void)
{
    test_internal_mode_policy();
    test_external_mode_policy();
    test_external_mode_health();
}

static void
test_audit_chain_health_impact(void)
{
    g_assert_cmpint(
        pcv_rest_audit_chain_health_impact(TRUE, FALSE), ==,
        PCV_REST_AUDIT_HEALTH_NONE);
    g_assert_cmpint(
        pcv_rest_audit_chain_health_impact(TRUE, TRUE), ==,
        PCV_REST_AUDIT_HEALTH_INFORMATIONAL);
    g_assert_cmpint(
        pcv_rest_audit_chain_health_impact(FALSE, FALSE), ==,
        PCV_REST_AUDIT_HEALTH_CRITICAL);
    g_assert_cmpint(
        pcv_rest_audit_chain_health_impact(FALSE, TRUE), ==,
        PCV_REST_AUDIT_HEALTH_CRITICAL);
}

static void
test_rpc_error_http_status(void)
{
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_INVALID_PARAMS), ==, 400);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_METHOD_NOT_FOUND), ==, 404);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_VM_NOT_FOUND), ==, 404);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_NOT_FOUND), ==, 404);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_CONFLICT), ==, 409);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_BUSY), ==, 409);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_TIMEOUT), ==, 504);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_FORBIDDEN), ==, 403);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_TOTP_INVALID_CODE), ==, 401);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_TOTP_LOCKED), ==, 429);
    g_assert_cmpuint(
        pcv_rest_http_status_for_rpc_error(PURE_RPC_ERR_ZFS_OPERATION), ==, 500);
}


static void
assert_rpc_method(const gchar *rpc_json,
                  PcvRestRpcMethodStatus expected_status,
                  const gchar *expected_method)
{
    gchar *method = (gchar *)"sentinel";
    PcvRestRpcMethodStatus status = pcv_rest_rpc_envelope_method(rpc_json, &method);
    g_assert_cmpint(status, ==, expected_status);
    g_assert_cmpstr(method, ==, expected_method);
    g_free(method);
}





static void
test_rpc_method_gate_reads_top_level_member(void)
{

    assert_rpc_method("{\"jsonrpc\":\"2.0\",\"method\":\"vm.delete\",\"params\":{},\"id\":1}",
                      PCV_REST_RPC_METHOD_OK, "vm.delete");

    assert_rpc_method("{\"jsonrpc\":\"2.0\",\"params\":{\"method\":\"vm.list\"},"
                      "\"method\":\"auth.user.create\",\"id\":1}",
                      PCV_REST_RPC_METHOD_OK, "auth.user.create");

    assert_rpc_method("{\"params\":{\"note\":\"\\\"method\\\":\\\"vm.list\\\"\"},"
                      "\"method\":\"vm.stop\"}",
                      PCV_REST_RPC_METHOD_OK, "vm.stop");

    assert_rpc_method("{\"method\":\"vm.de\\u006cete\"}", PCV_REST_RPC_METHOD_OK, "vm.delete");

    assert_rpc_method("{\"params\":{\"method\":\"vm.list\"},\"id\":1}",
                      PCV_REST_RPC_METHOD_MISSING, NULL);
    assert_rpc_method("{\"method\":7,\"params\":{\"method\":\"vm.list\"}}",
                      PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method("{\"method\":null}", PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method("{\"method\":{\"method\":\"vm.list\"}}",
                      PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method("{\"method\":\"\"}", PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method("[{\"method\":\"vm.list\"}]", PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method("{\"method\":\"vm.list\"", PCV_REST_RPC_METHOD_MALFORMED, NULL);
    assert_rpc_method(NULL, PCV_REST_RPC_METHOD_MALFORMED, NULL);
}


static void
assert_volume_detach_params(const gchar *const *segments, JsonObject *body,
                            const gchar *expected_path)
{
    JsonObject *params = pcv_rest_container_volume_detach_params(
        "c1", (gchar **)segments, body);
    g_assert_nonnull(params);
    g_assert_cmpstr(json_object_get_string_member(params, "name"), ==, "c1");


    g_assert_false(json_object_has_member(params, "volume"));
    if (expected_path)
        g_assert_cmpstr(json_object_get_string_member(params, "container_path"), ==,
                        expected_path);
    else
        g_assert_false(json_object_has_member(params, "container_path"));
    json_object_unref(params);
}




static void
test_container_volume_detach_contract(void)
{
    const gchar *one[] = { "mnt", NULL };
    const gchar *nested[] = { "mnt", "data", NULL };
    const gchar *none[] = { NULL };
    assert_volume_detach_params(one, NULL, "/mnt");
    assert_volume_detach_params(nested, NULL, "/mnt/data");

    JsonObject *body = json_object_new();
    json_object_set_string_member(body, "container_path", "/srv/share");
    json_object_set_string_member(body, "name", "other");
    assert_volume_detach_params(none, body, "/srv/share");

    assert_volume_detach_params(nested, body, "/mnt/data");
    json_object_unref(body);


    assert_volume_detach_params(none, NULL, NULL);

    const gchar *gap[] = { "mnt", "", "data", NULL };
    assert_volume_detach_params(gap, NULL, NULL);
}

void
test_rest_transport_register(void)
{
    g_test_add_func("/rest_transport/container_volume_detach_contract",
                    test_container_volume_detach_contract);
    g_test_add_func("/rest_transport", test_rest_transport_contract);
    g_test_add_func("/rest_transport/rpc_method_gate",
                    test_rpc_method_gate_reads_top_level_member);
    g_test_add_func("/rest_transport/rpc_error_http_status", test_rpc_error_http_status);
    g_test_add_func("/rest_transport/audit_chain_health_impact",
                    test_audit_chain_health_impact);
}
