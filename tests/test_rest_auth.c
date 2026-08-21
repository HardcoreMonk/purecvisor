                                                                                             
                                                                                        
                                                                                   
                                                               
                               
                         
  
                                                                     
  
                 
                                                                   
                                                              
                                                                
                                                              
                                                
                                                                        
                                                      
                                                     
                                                     
                      
                                                               
                                                        
  
                                  
   
#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>

#include "../src/api/rest_auth.h"
#include "../src/api/rest_client_identity.h"
#include "../src/api/rest_server.h"
#include "../src/utils/pcv_crypto.h"

typedef struct {
    gchar *audit_address;
    gchar *rate_key;
    gboolean handled;
    gboolean identity_reused;
    guint identity_destroy_count;
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
    const gchar *forwarded_ip;
} ProxyClientRequest;

static gpointer
send_proxy_request(gpointer data)
{
    ProxyClientRequest *request = data;
    g_autoptr(SoupSession) session = soup_session_new();
    g_autoptr(SoupMessage) message =
        soup_message_new("POST", request->uri);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) response = NULL;

    soup_message_headers_replace(
        soup_message_get_request_headers(message),
        "X-Real-IP",
        request->forwarded_ip);
    response = soup_session_send_and_read(session, message, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(response);
    return NULL;
}

void
pcv_test_capture_proxy_consumers(const gchar *forwarded_ip,
                                 gchar **audit_address,
                                 gchar **rate_key)
{
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    g_autoptr(GError) error = NULL;
    GSList *uris;
    ProxyConsumerCapture capture = {0};

    soup_server_add_handler(server, NULL, capture_proxy_consumers,
                            &capture, NULL);
    g_assert_true(soup_server_listen_local(server, 0, 0, &error));
    g_assert_no_error(error);
    uris = soup_server_get_uris(server);
    g_assert_nonnull(uris);

    ProxyClientRequest request = {
        .uri = g_uri_to_string(uris->data),
        .forwarded_ip = forwarded_ip,
    };
    g_autoptr(GThread) thread =
        g_thread_new("proxy-consumer-client", send_proxy_request, &request);
    while (!capture.handled)
        g_main_context_iteration(NULL, TRUE);
    g_thread_join(g_steal_pointer(&thread));
    soup_server_disconnect(server);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);

    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    g_assert_true(capture.identity_reused);
    g_assert_cmpuint(capture.identity_destroy_count, ==, 1);
    *audit_address = g_steal_pointer(&capture.audit_address);
    *rate_key = g_steal_pointer(&capture.rate_key);
    g_free(request.uri);
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
}
