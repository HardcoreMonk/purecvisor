                                                                                             
                                                                                        
                                                                                   
                                                               
                               
                         
  
                                                                     
  
                 
                                                                   
                                                              
                                                                
                                                              
                                                
                                                                        
                                                      
                                                     
                                                     
                      
                                                               
                                                        
  
                                  
   
#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>

#include "../src/api/rest_auth.h"
#include "../src/api/rest_client_identity.h"
#include "../src/api/rest_server.h"
#include "../src/utils/pcv_config.h"
#include "../src/utils/pcv_crypto.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    gchar *audit_address;
    gchar *rate_key;
    gboolean handled;
    gboolean identity_reused;
    guint identity_destroy_count;
    gboolean direct_local;
} ProxyConsumerCapture;

static void
captured_identity_destroyed(gpointer data,
                            GObject *where_the_object_was)
{
    guint *destroy_count = data;

    (void)where_the_object_was;
    (*destroy_count)++;
}

static void
capture_proxy_consumers(SoupServer *server,
                        SoupServerMessage *message,
                        const gchar *path,
                        GHashTable *query,
                        gpointer user_data)
{
    ProxyConsumerCapture *capture = user_data;
    const PcvRestClientIdentity *first =
        pcv_rest_client_identity_get(message);
    const PcvRestClientIdentity *again =
        pcv_rest_client_identity_get(message);

    (void)server;
    (void)query;
    capture->identity_reused = first == again;
    capture->direct_local = pcv_rest_client_identity_is_direct_local(first);
    g_object_weak_ref(G_OBJECT(first), captured_identity_destroyed,
                      &capture->identity_destroy_count);
    capture->audit_address =
        g_strdup(pcv_rest_auth_audit_address(message));
    capture->rate_key =
        pcv_rest_rate_limit_key_for_message(message, path, "POST");
    soup_server_message_set_status(message, 204, NULL);
    capture->handled = TRUE;
}

typedef struct {
    gchar *uri;
    const gchar *header;
    const gchar *value;
    const gchar *local_ip;
} ProxyClientRequest;

static gpointer
send_proxy_request(gpointer data)
{
    ProxyClientRequest *request = data;
    g_autoptr(GSocketAddress) local = request->local_ip
        ? g_inet_socket_address_new_from_string(request->local_ip, 0) : NULL;
    g_autoptr(SoupSession) session = local
        ? soup_session_new_with_options("local-address", local, NULL) : soup_session_new();
    g_autoptr(SoupMessage) message =
        soup_message_new("POST", request->uri);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) response = NULL;

    if (request->header)
        soup_message_headers_replace(
            soup_message_get_request_headers(message),
            request->header,
            request->value);
    response = soup_session_send_and_read(session, message, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(response);
    return NULL;
}

static gboolean
_capture_at(PcvRestProxyTrust *trust, const gchar *listen_ip, const gchar *local_ip,
            const gchar *header, const gchar *value, ProxyConsumerCapture *capture)
{
    if (trust)
        pcv_rest_proxy_trust_install(trust);
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketAddress) listen_addr = listen_ip
        ? g_inet_socket_address_new_from_string(listen_ip, 0) : NULL;
    GSList *uris;

    soup_server_add_handler(server, NULL, capture_proxy_consumers,
                            capture, NULL);
    if (!(listen_addr ? soup_server_listen(server, listen_addr, 0, &error)
                      : soup_server_listen_local(server, 0, 0, &error))) {
        g_test_message("listen %s 실패: %s", listen_ip ? listen_ip : "local", error ? error->message : "?");
        pcv_rest_proxy_trust_install(NULL);
        return FALSE;
    }
    uris = soup_server_get_uris(server);
    g_assert_nonnull(uris);

    ProxyClientRequest request = {
        .uri = g_uri_to_string(uris->data),
        .header = header,
        .value = value,
        .local_ip = local_ip,
    };
    g_autoptr(GThread) thread =
        g_thread_new("proxy-consumer-client", send_proxy_request, &request);
    while (!capture->handled)
        g_main_context_iteration(NULL, TRUE);
    g_thread_join(g_steal_pointer(&thread));
    soup_server_disconnect(server);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);

    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    g_assert_true(capture->identity_reused);
    g_assert_cmpuint(capture->identity_destroy_count, ==, 1);
    g_free(request.uri);
    pcv_rest_proxy_trust_install(NULL);
    return TRUE;
}

static void
_capture(const gchar *header, const gchar *value, ProxyConsumerCapture *capture)
{
    g_assert_true(_capture_at(pcv_rest_proxy_trust_new("127.0.0.1,::1", NULL, NULL), NULL, NULL,
                              header, value, capture));
}

void
pcv_test_capture_proxy_consumers(const gchar *forwarded_ip,
                                 gchar **audit_address,
                                 gchar **rate_key)
{
    ProxyConsumerCapture capture = {0};

    _capture("X-Real-IP", forwarded_ip, &capture);
    *audit_address = g_steal_pointer(&capture.audit_address);
    *rate_key = g_steal_pointer(&capture.rate_key);
}

static gboolean
_capture_direct_local(const gchar *header, const gchar *value)
{
    ProxyConsumerCapture capture = {0};
    _capture(header, value, &capture);
    g_free(capture.audit_address);
    g_free(capture.rate_key);
    return capture.direct_local;
}

static void
test_direct_local_requires_no_forwarding_header(void)
{
    static const gchar *const headers[] = {
        "X-Real-IP", "X-Forwarded-For", "X-Forwarded-Proto", "Forwarded", "X-PCV-Proxy-Auth",
    };
    g_assert_true(_capture_direct_local(NULL, NULL));
    for (gsize i = 0; i < G_N_ELEMENTS(headers); i++) {
        g_assert_false(_capture_direct_local(headers[i], "198.51.100.30"));
        g_assert_false(_capture_direct_local(headers[i], ""));
    }
}

static gchar *
_audit_with_config(const gchar *conf, const gchar *peer_ip)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    pcv_rest_proxy_trust_install(NULL);
    if (conf) {
        dir = g_dir_make_tmp("pcv-trust-XXXXXX", NULL);
        g_assert_nonnull(dir);
        path = g_build_filename(dir, "daemon.conf", NULL);
        g_assert_true(g_file_set_contents(path, conf, -1, NULL));
        g_setenv("PCV_CONFIG_PATH", path, TRUE);
        pcv_config_init();
        pcv_rest_proxy_trust_load_config();
    }
    ProxyConsumerCapture capture = {0};
    gboolean sent = _capture_at(NULL, strchr(peer_ip, ':') ? "::1" : "127.0.0.1", peer_ip,
                                "X-Real-IP", "198.51.100.40", &capture);
    if (conf) {
        pcv_config_shutdown();

        g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
        g_assert_cmpint(g_remove(path), ==, 0);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
    }
    g_free(capture.rate_key);
    if (!sent)
        g_clear_pointer(&capture.audit_address, g_free);
    return capture.audit_address;
}

static void
_assert_default_trust_table(const gchar *conf)
{
    g_autofree gchar *v4 = _audit_with_config(conf, "127.0.0.1");
    g_autofree gchar *v4_other = _audit_with_config(conf, "127.0.0.2");
    g_autofree gchar *v6 = _audit_with_config(conf, "::1");
    g_assert_cmpstr(v4, ==, "198.51.100.40");
    g_assert_cmpstr(v4_other, ==, "127.0.0.2");
    if (v6)
        g_assert_cmpstr(v6, ==, "::1");
    else
        g_test_message("::1에 서버를 열 수 없어 IPv6 기본값 줄을 건너뛴다");
}

static void
test_trusted_proxies_config_path(void)
{
    _assert_default_trust_table(NULL);
    _assert_default_trust_table("[server]\n");
    g_autofree gchar *empty = _audit_with_config("[server]\ntrusted_proxies =\n", "127.0.0.1");
    g_assert_cmpstr(empty, ==, "127.0.0.1");
    g_autofree gchar *listed = _audit_with_config("[server]\ntrusted_proxies = 127.0.0.0/8\n", "127.0.0.2");
    g_assert_cmpstr(listed, ==, "198.51.100.40");
}

  
                                                                          
                                                                          
                                                                          
                                                             
   
static void
test_fallback_user_absent_recovers(void)
{
                   
    g_assert_true(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                          "purecvisor",
                                                          "admin",
                                                          "purecvisor",
                                                          FALSE));
}

static void
test_fallback_user_present_denied_sec2(void)
{
                                 
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           TRUE));
}

static void
test_fallback_nonmatching_creds_denied(void)
{
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "wrong",
                                                           "admin",
                                                           "purecvisor",
                                                           FALSE));
}

static void
test_fallback_null_denied(void)
{
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap(NULL,
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           FALSE));
}

  
                                                   
                                          
   
static void
test_secret_str_eq_identical_true(void)
{
    g_assert_true(pcv_secret_str_eq("purecvisor", "purecvisor"));
}

static void
test_secret_str_eq_different_false(void)
{
    g_assert_false(pcv_secret_str_eq("purecvisor", "wrongpass"));
}

static void
test_secret_str_eq_length_diff_false(void)
{
    g_assert_false(pcv_secret_str_eq("a", "aa"));
}

static void
test_secret_str_eq_null_denied(void)
{
    g_assert_false(pcv_secret_str_eq(NULL, "purecvisor"));
    g_assert_false(pcv_secret_str_eq("purecvisor", NULL));
    g_assert_false(pcv_secret_str_eq(NULL, NULL));
}

static void
test_secret_str_eq_empty_identical_true(void)
{
    g_assert_true(pcv_secret_str_eq("", ""));
}

static void
test_forwarded_audit_addresses_are_distinct(void)
{
    g_autofree gchar *first = NULL;
    g_autofree gchar *first_key = NULL;
    g_autofree gchar *second = NULL;
    g_autofree gchar *second_key = NULL;

    pcv_test_capture_proxy_consumers(
        "198.51.100.10", &first, &first_key);
    pcv_test_capture_proxy_consumers(
        "198.51.100.11", &second, &second_key);

    g_assert_cmpstr(first, ==, "198.51.100.10");
    g_assert_cmpstr(second, ==, "198.51.100.11");
    g_assert_cmpstr(first, !=, second);
    g_assert_cmpstr(first_key, !=, second_key);
}

static void
test_rest_auth_exact_path_proxy_consumers(void)
{
    test_forwarded_audit_addresses_are_distinct();
}

void
test_rest_auth_register(void)
{
    g_test_add_func("/rest_auth",
                    test_rest_auth_exact_path_proxy_consumers);
    g_test_add_func("/rest_auth/bootstrap_fallback/user_absent_recovers",
                    test_fallback_user_absent_recovers);
    g_test_add_func("/rest_auth/bootstrap_fallback/user_present_denied_sec2",
                    test_fallback_user_present_denied_sec2);
    g_test_add_func("/rest_auth/bootstrap_fallback/nonmatching_creds_denied",
                    test_fallback_nonmatching_creds_denied);
    g_test_add_func("/rest_auth/bootstrap_fallback/null_denied",
                    test_fallback_null_denied);
    g_test_add_func("/rest_auth/secret_str_eq/identical_true",
                    test_secret_str_eq_identical_true);
    g_test_add_func("/rest_auth/secret_str_eq/different_false",
                    test_secret_str_eq_different_false);
    g_test_add_func("/rest_auth/secret_str_eq/length_diff_false",
                    test_secret_str_eq_length_diff_false);
    g_test_add_func("/rest_auth/secret_str_eq/null_denied",
                    test_secret_str_eq_null_denied);
    g_test_add_func("/rest_auth/secret_str_eq/empty_identical_true",
                    test_secret_str_eq_empty_identical_true);
    g_test_add_func("/rest_auth/proxy_identity/distinct_audit_addresses",
                    test_forwarded_audit_addresses_are_distinct);
    g_test_add_func("/rest_auth/trusted_proxies_config_path",
                    test_trusted_proxies_config_path);
    g_test_add_func("/rest_auth/direct_local_requires_no_forwarding_header",
                    test_direct_local_requires_no_forwarding_header);
}
