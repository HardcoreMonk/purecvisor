
#include <glib.h>
#include <glib-object.h>
#include "../src/modules/virt/vm_manager.h"

typedef struct {
    gint      started_count;
    gint      stopped_count;
    gint      metrics_count;
    gchar    *last_vm_name;
    GHashTable *last_metrics;
} SignalLog;

static void
on_vm_started(PureCVisorVmManager *mgr __attribute__((unused)),
              const gchar         *vm_name,
              gpointer             user_data)
{
    SignalLog *log = user_data;
    log->started_count++;
    g_free(log->last_vm_name);
    log->last_vm_name = g_strdup(vm_name);
}

static void
on_vm_stopped(PureCVisorVmManager *mgr __attribute__((unused)),
              const gchar         *vm_name,
              gpointer             user_data)
{
    SignalLog *log = user_data;
    log->stopped_count++;
    g_free(log->last_vm_name);
    log->last_vm_name = g_strdup(vm_name);
}

static void
on_metrics_updated(PureCVisorVmManager *mgr __attribute__((unused)),
                   GHashTable          *cache,
                   gpointer             user_data)
{
    SignalLog *log = user_data;
    log->metrics_count++;
    log->last_metrics = cache;
}

static PureCVisorVmManager *
make_manager(void)
{

    return purecvisor_vm_manager_new(NULL);
}

static void
test_signals_registered(void)
{
    PureCVisorVmManager *mgr = make_manager();

    guint id_started = g_signal_lookup(PCV_VM_SIGNAL_STARTED,
                                       PURECVISOR_TYPE_VM_MANAGER);
    guint id_stopped = g_signal_lookup(PCV_VM_SIGNAL_STOPPED,
                                       PURECVISOR_TYPE_VM_MANAGER);
    guint id_metrics = g_signal_lookup(PCV_VM_SIGNAL_METRICS_UPDATED,
                                       PURECVISOR_TYPE_VM_MANAGER);

    g_assert_cmpuint(id_started, !=, 0);
    g_assert_cmpuint(id_stopped, !=, 0);
    g_assert_cmpuint(id_metrics, !=, 0);

    g_object_unref(mgr);
}

static void
test_vm_started_emit(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log = {0};

    g_signal_connect(mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(on_vm_started), &log);

    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "test-vm-01");

    g_assert_cmpint(log.started_count, ==, 1);
    g_assert_cmpstr(log.last_vm_name, ==, "test-vm-01");

    g_free(log.last_vm_name);
    g_object_unref(mgr);
}

static void
test_vm_stopped_emit(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log = {0};

    g_signal_connect(mgr, PCV_VM_SIGNAL_STOPPED,
                     G_CALLBACK(on_vm_stopped), &log);

    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STOPPED, "test-vm-02");

    g_assert_cmpint(log.stopped_count, ==, 1);
    g_assert_cmpstr(log.last_vm_name, ==, "test-vm-02");

    g_free(log.last_vm_name);
    g_object_unref(mgr);
}

static void
test_metrics_updated_emit(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log = {0};

    g_signal_connect(mgr, PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_CALLBACK(on_metrics_updated), &log);

    GHashTable *cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(cache, g_strdup("uuid-abc"), g_strdup("metrics-data"));

    purecvisor_vm_manager_emit_metrics_updated(mgr, cache);

    g_assert_cmpint(log.metrics_count, ==, 1);
    g_assert_nonnull(log.last_metrics);

    g_assert_true(log.last_metrics == cache);

    g_hash_table_destroy(cache);
    g_object_unref(mgr);
}

static void
test_emit_no_handler(void)
{
    PureCVisorVmManager *mgr = make_manager();

    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "no-handler-vm");
    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STOPPED, "no-handler-vm");
    purecvisor_vm_manager_emit_metrics_updated(mgr, NULL);

    g_object_unref(mgr);
}

static void
test_multiple_handlers(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log_a = {0}, log_b = {0};

    g_signal_connect(mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(on_vm_started), &log_a);
    g_signal_connect(mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(on_vm_started), &log_b);

    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "multi-vm");

    g_assert_cmpint(log_a.started_count, ==, 1);
    g_assert_cmpint(log_b.started_count, ==, 1);

    g_free(log_a.last_vm_name);
    g_free(log_b.last_vm_name);
    g_object_unref(mgr);
}

static void
test_disconnect_handler(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log = {0};

    gulong hid = g_signal_connect(mgr, PCV_VM_SIGNAL_STARTED,
                                  G_CALLBACK(on_vm_started), &log);

    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "before-disconnect");
    g_assert_cmpint(log.started_count, ==, 1);

    g_signal_handler_disconnect(mgr, hid);
    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "after-disconnect");

    g_assert_cmpint(log.started_count, ==, 1);

    g_free(log.last_vm_name);
    g_object_unref(mgr);
}

void
test_vm_signals_register(void)
{
    g_test_add_func("/vm_signals/registered",        test_signals_registered);
    g_test_add_func("/vm_signals/started_emit",      test_vm_started_emit);
    g_test_add_func("/vm_signals/stopped_emit",      test_vm_stopped_emit);
    g_test_add_func("/vm_signals/metrics_emit",      test_metrics_updated_emit);
    g_test_add_func("/vm_signals/emit_no_handler",   test_emit_no_handler);
    g_test_add_func("/vm_signals/multiple_handlers", test_multiple_handlers);
    g_test_add_func("/vm_signals/disconnect",        test_disconnect_handler);
}
