/* tests/test_vm_signals.c
 *
 * 대상 모듈: src/modules/virt/vm_manager.c — GObject 시그널 (vm-started/stopped/metrics-updated)
 *
 * 이 테스트가 검증하는 것:
 *   PureCVisorVmManager GObject에 등록된 3개 시그널의 등록 여부,
 *   emit 시 핸들러 호출과 파라미터 전달, 복수 핸들러 동시 호출,
 *   disconnect 후 호출 차단, 핸들러 미연결 시 안전 통과를 검사한다.
 *   libvirt 연결 없이 conn=NULL로 시그널 메커니즘만 격리 테스트.
 *
 * 실행: sudo ./test_runner -p /vm_signals
 *
 * 외부 의존: 없음 (libvirt 연결 불필요, GObject 시그널만 테스트)
 */

#include <glib.h>
#include <glib-object.h>
#include "../src/modules/virt/vm_manager.h"

/* ── 신호 수신 카운터 ───────────────────────────────── */
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

/* ── 헬퍼 ──────────────────────────────────────────── */
static PureCVisorVmManager *
make_manager(void)
{
    /* conn=NULL: 신호 테스트에는 libvirt 커넥션 불필요 */
    return purecvisor_vm_manager_new(NULL);
}

/* ── 테스트 케이스 ──────────────────────────────────── */

/* 1. 신호 3개가 정상 등록됐는지 확인 */
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

/* 2. vm-started 신호 emit → 핸들러 호출 + vm_name 전달 확인 */
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

/* 3. vm-stopped 신호 emit → 핸들러 호출 + vm_name 전달 확인 */
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

/* 4. vm-metrics-updated 신호 emit → GHashTable* 전달 확인 */
static void
test_metrics_updated_emit(void)
{
    PureCVisorVmManager *mgr = make_manager();
    SignalLog log = {0};

    g_signal_connect(mgr, PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_CALLBACK(on_metrics_updated), &log);

    /* 더미 캐시 생성 */
    GHashTable *cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(cache, g_strdup("uuid-abc"), g_strdup("metrics-data"));

    purecvisor_vm_manager_emit_metrics_updated(mgr, cache);

    g_assert_cmpint(log.metrics_count, ==, 1);
    g_assert_nonnull(log.last_metrics);
    /* 핸들러가 받은 포인터가 emit 시 전달한 포인터와 동일한지 확인 */
    g_assert_true(log.last_metrics == cache);

    g_hash_table_destroy(cache);
    g_object_unref(mgr);
}

/* 5. 핸들러 미연결 시 emit 은 무해하게 통과 (crash 없음) */
static void
test_emit_no_handler(void)
{
    PureCVisorVmManager *mgr = make_manager();

    /* 핸들러 연결 없이 emit — SIGABRT/crash 없어야 함 */
    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STARTED, "no-handler-vm");
    g_signal_emit_by_name(mgr, PCV_VM_SIGNAL_STOPPED, "no-handler-vm");
    purecvisor_vm_manager_emit_metrics_updated(mgr, NULL);

    /* 여기까지 도달하면 통과 */
    g_object_unref(mgr);
}

/* 6. 복수 핸들러 연결 → 모두 호출 확인 */
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

/* 7. g_signal_disconnect 후 emit → 핸들러 미호출 확인 */
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

    /* disconnect 이후 emit 은 무시 */
    g_assert_cmpint(log.started_count, ==, 1);

    g_free(log.last_vm_name);
    g_object_unref(mgr);
}

/* ── 등록 함수 ──────────────────────────────────────── */
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
