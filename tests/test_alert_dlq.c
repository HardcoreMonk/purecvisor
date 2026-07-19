
#include <glib.h>
#include "modules/daemons/alert_dlq.h"

static gchar *entry_of(const gchar *url, const gchar *payload) {
    return g_strdup_printf("%s|%s", url, payload);
}

static void assert_at(GPtrArray *a, guint i, const gchar *want) {
    g_assert_cmpuint(i, <, a->len);
    g_assert_cmpstr((const gchar *)g_ptr_array_index(a, i), ==, want);
}

static void test_dlq_remove_by_value_survives_mutation(void) {
    pcv_alert_dlq_reset();

    gchar *e1 = entry_of("http://a", "1");
    gchar *e2 = entry_of("http://b", "2");
    gchar *e3 = entry_of("http://c", "3");
    gchar *e4 = entry_of("http://d", "4");

    pcv_alert_dlq_store("http://a", "1");
    pcv_alert_dlq_store("http://b", "2");
    pcv_alert_dlq_store("http://c", "3");

    GPtrArray *snap = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(snap->len, ==, 3);

    pcv_alert_dlq_store("http://d", "4");

    GPtrArray *ok = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ok, g_strdup(e2));
    pcv_alert_dlq_remove_matching(ok);
    g_ptr_array_unref(ok);

    GPtrArray *now = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(now->len, ==, 3);
    assert_at(now, 0, e1);
    assert_at(now, 1, e3);
    assert_at(now, 2, e4);
    g_ptr_array_unref(now);

    g_assert_cmpuint(snap->len, ==, 3);
    assert_at(snap, 0, e1);
    assert_at(snap, 1, e2);
    assert_at(snap, 2, e3);
    g_ptr_array_unref(snap);

    g_free(e1); g_free(e2); g_free(e3); g_free(e4);
    pcv_alert_dlq_reset();
}

void test_alert_dlq_register(void) {
    g_test_add_func("/alert_dlq/remove_by_value_survives_mutation",
                    test_dlq_remove_by_value_survives_mutation);
}
