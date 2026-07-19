
#include <glib.h>
#include "modules/daemons/alert_silence.h"

static void test_silence_case_insensitive_suppresses(void) {
    pcv_alert_silence_reset();
    pcv_alert_add_silence("cpu", 60, "maint");
    g_assert_true (pcv_alert_is_silenced("CPU"));
    g_assert_false(pcv_alert_is_silenced("NeverSilencedXYZ"));
}

static gpointer silence_writer_worker(gpointer u) {
    (void)u;
    for (int i = 0; i < 200; i++)
        pcv_alert_add_silence("stress", 60, "concurrent");
    return NULL;
}

static gpointer silence_reader_worker(gpointer u) {
    (void)u;
    for (int i = 0; i < 200; i++) {
        pcv_alert_is_silenced("stress");
        JsonArray *arr = pcv_alert_get_silences();
        json_array_unref(arr);
    }
    return NULL;
}

static void test_silence_concurrent_writer_reader_safe(void) {
    pcv_alert_silence_reset();
    GThread *w[4], *r[4];
    for (int i = 0; i < 4; i++) {
        w[i] = g_thread_new("silence-w", silence_writer_worker, NULL);
        r[i] = g_thread_new("silence-r", silence_reader_worker, NULL);
    }
    for (int i = 0; i < 4; i++) {
        g_thread_join(w[i]);
        g_thread_join(r[i]);
    }

    g_assert_true(pcv_alert_is_silenced("stress"));
    pcv_alert_silence_reset();
}

void test_alert_silence_register(void) {
    g_test_add_func("/alert_silence/case_insensitive_suppresses",
                    test_silence_case_insensitive_suppresses);
    g_test_add_func("/alert_silence/concurrent_writer_reader_safe",
                    test_silence_concurrent_writer_reader_safe);
}
