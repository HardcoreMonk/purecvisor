


















#include <glib.h>
#include "../src/utils/pcv_jwt.h"


static void
jwt_setup(gpointer *fixture __attribute__((unused)), gconstpointer data __attribute__((unused)))
{
    pcv_jwt_init("test-secret-key-for-unit-tests-32b");
}

static void
jwt_teardown(gpointer *fixture __attribute__((unused)), gconstpointer data __attribute__((unused)))
{
    pcv_jwt_shutdown();
}




static void
test_jwt_sign_verify(gpointer *fixture __attribute__((unused)),
                     gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);

    g_assert_no_error(err);
    g_assert_nonnull(token);
    g_assert_true(g_str_has_prefix(token, "eyJ") ||
                  strlen(token) > 32);

    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_no_error(verr);
    g_assert_nonnull(subject);
    g_assert_cmpstr(subject, ==, "admin");

    g_free(token);
    g_free(subject);
}


static void
test_jwt_bearer_prefix(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("user1", 3600, &err);
    g_assert_no_error(err);

    gchar  *bearer  = g_strdup_printf("Bearer %s", token);
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(bearer, &verr);

    g_assert_no_error(verr);
    g_assert_cmpstr(subject, ==, "user1");

    g_free(token);
    g_free(bearer);
    g_free(subject);
}


static void
test_jwt_expired(gpointer *fixture __attribute__((unused)),
                 gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;





    gchar *token = pcv_jwt_sign("admin", 1, &err);
    g_assert_no_error(err);
    g_assert_nonnull(token);


    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(subject);

    g_free(token);
    g_free(subject);
}


static void
test_jwt_bad_signature(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);


    gchar *dot = g_strrstr(token, ".");
    g_assert_nonnull(dot);
    *(dot + 1) = 'X';
    *(dot + 2) = 'X';
    *(dot + 3) = 'X';

    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_null(subject);
    g_assert_nonnull(verr);
    g_error_free(verr);
    g_free(token);
}


static void
test_jwt_malformed(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify("not.a.valid.token.at.all", &verr);


    if (subject) {

        g_free(subject);
    }
    if (verr) g_error_free(verr);

}


static void
test_jwt_null_input(gpointer *fixture __attribute__((unused)),
                     gconstpointer data __attribute__((unused)))
{

    GError *err  = NULL;
    gchar  *tok1 = pcv_jwt_sign(NULL, 3600, &err);
    g_assert_null(tok1);
    if (err) g_error_free(err);

    gchar  *tok2 = pcv_jwt_sign("", 3600, &err);
    g_assert_null(tok2);
    if (err) g_error_free(err);
}


static void
test_jwt_wrong_key(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{

    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);


    pcv_jwt_shutdown();
    pcv_jwt_init("completely-different-secret-key!!");


    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_null(subject);
    if (verr) g_error_free(verr);


    pcv_jwt_shutdown();
    pcv_jwt_init("test-secret-key-for-unit-tests-32b");
    g_free(token);
}


static void
test_jwt_structure(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("testuser", 3600, &err);
    g_assert_no_error(err);
    g_assert_nonnull(token);


    gchar **parts = g_strsplit(token, ".", -1);
    guint   count = g_strv_length(parts);
    g_assert_cmpuint(count, ==, 3);


    for (guint i = 0; i < 3; i++)
        g_assert_true(strlen(parts[i]) > 0);

    g_strfreev(parts);
    g_free(token);
}


static void
test_jwt_blacklist_add_and_check(gpointer *fixture __attribute__((unused)),
                                  gconstpointer data __attribute__((unused)))
{

    const gchar *jti_a = "aabbccddeeff00112233445566778899";
    const gchar *jti_b = "deadbeefdeadbeefdeadbeefdeadbeef";

    gint64 future_expiry = (gint64)(g_get_real_time() / G_USEC_PER_SEC) + 3600;


    g_assert_false(pcv_jwt_blacklist_check(jti_a));


    pcv_jwt_blacklist_add(jti_a, future_expiry);
    g_assert_true(pcv_jwt_blacklist_check(jti_a));


    g_assert_false(pcv_jwt_blacklist_check(jti_b));
}


static void
test_jwt_blacklist_sweep(gpointer *fixture __attribute__((unused)),
                          gconstpointer data __attribute__((unused)))
{
    const gchar *jti = "11223344556677889900aabbccddeeff";


    gint64 past_expiry = (gint64)(g_get_real_time() / G_USEC_PER_SEC) - 100;
    pcv_jwt_blacklist_add(jti, past_expiry);



    pcv_jwt_blacklist_sweep();


    g_assert_false(pcv_jwt_blacklist_check(jti));
}


static void
test_jwt_update_secret(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{

    GError *err      = NULL;
    gchar  *old_token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);
    g_assert_nonnull(old_token);


    pcv_jwt_update_secret("new-test-secret-key-32bytes!!");


    GError *err2      = NULL;
    gchar  *new_token = pcv_jwt_sign("admin", 3600, &err2);
    g_assert_no_error(err2);
    g_assert_nonnull(new_token);


    GError *verr = NULL;
    gchar  *sub  = pcv_jwt_verify(new_token, &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(sub);
    g_assert_cmpstr(sub, ==, "admin");
    g_free(sub);


    GError *verr2 = NULL;
    gchar  *sub2  = pcv_jwt_verify(old_token, &verr2);
    g_assert_null(sub2);
    g_assert_nonnull(verr2);
    g_error_free(verr2);


    pcv_jwt_update_secret("test-secret-key-for-unit-tests-32b");

    g_free(old_token);
    g_free(new_token);
}


static void
test_jwt_sign_with_ip(gpointer *fixture __attribute__((unused)),
                       gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign_with_ip("testuser", 300, "127.0.0.1", &err);

    g_assert_no_error(err);
    g_assert_nonnull(token);


    GError *verr = NULL;
    gchar  *sub  = pcv_jwt_verify_with_ip(token, "127.0.0.1", &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(sub);
    g_assert_cmpstr(sub, ==, "testuser");
    g_free(sub);


    GError *verr2 = NULL;
    gchar  *sub2  = pcv_jwt_verify_with_ip(token, "10.0.0.1", &verr2);
    g_assert_null(sub2);
    g_assert_nonnull(verr2);
    g_error_free(verr2);

    g_free(token);
}


void
test_jwt_register(void)
{
#define ADD(name, func) \
    g_test_add("/jwt/" name, gpointer, NULL, jwt_setup, func, jwt_teardown)

    ADD("sign_verify",           test_jwt_sign_verify);
    ADD("bearer_prefix",         test_jwt_bearer_prefix);
    ADD("not_expired_yet",       test_jwt_expired);
    ADD("bad_signature",         test_jwt_bad_signature);
    ADD("malformed",             test_jwt_malformed);
    ADD("null_input",            test_jwt_null_input);
    ADD("wrong_key",             test_jwt_wrong_key);
    ADD("structure",             test_jwt_structure);
    ADD("blacklist_add_check",   test_jwt_blacklist_add_and_check);
    ADD("blacklist_sweep",       test_jwt_blacklist_sweep);
    ADD("update_secret",         test_jwt_update_secret);
    ADD("sign_with_ip",          test_jwt_sign_with_ip);

#undef ADD
}
