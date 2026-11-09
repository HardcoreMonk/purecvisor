                                                                                           
                                                    
                                            
                                                    
                                                          
#include <glib.h>
#include "../src/api/vm_batch_queue.h"

typedef struct {
    GPtrArray *submitted;                        
    GPtrArray *reports;                         
    GPtrArray *actors;                                                           
    PcvVmBatchQueue *pending;                       
    const gchar *reject_vm;                     
    gboolean reject_all;                         
    gboolean terminating;
    guint active;                                               
    guint max_active;
} QueueFixture;

static QueueFixture *current;

static gboolean fake_submit(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue, gpointer user_data, gchar **reason)
{
    QueueFixture *f = user_data;
    g_ptr_array_add(f->submitted, g_strdup(vm));
    g_ptr_array_add(f->actors, g_strdup_printf("submit %s=%s", vm, actor ? actor : "(null)"));
    if (f->reject_all || g_strcmp0(vm, f->reject_vm) == 0) {
        *reason = g_strdup("VM busy");
        return FALSE;
    }
    f->pending = queue;
    f->active++;
    if (f->active > f->max_active)
        f->max_active = f->active;
    return TRUE;
}

static void fake_report(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data)
{
    QueueFixture *f = user_data;
    g_ptr_array_add(f->actors, g_strdup_printf("report %s=%s", vm, actor ? actor : "(null)"));
    g_ptr_array_add(f->reports, g_strdup_printf("%s:%s", vm, reason));
}

static gboolean fake_terminating(void) { return current->terminating; }

static void fixture_init(QueueFixture *f, PcvVmBatchQueueOps *ops)
{
    *f = (QueueFixture){0};
    f->submitted = g_ptr_array_new_with_free_func(g_free);
    f->reports = g_ptr_array_new_with_free_func(g_free);
    f->actors = g_ptr_array_new_with_free_func(g_free);
    *ops = (PcvVmBatchQueueOps){ fake_submit, fake_report, fake_terminating, f, 0, NULL, NULL };
    current = f;
}

static void fixture_clear(QueueFixture *f)
{
    g_ptr_array_unref(f->submitted);
    g_ptr_array_unref(f->reports);
    g_ptr_array_unref(f->actors);
    current = NULL;
}

static void assert_strings(GPtrArray *actual, const gchar *const *expected)
{
    guint n = g_strv_length((gchar **)expected);
    g_assert_cmpuint(actual->len, ==, n);
    for (guint i = 0; i < n; i++)
        g_assert_cmpstr(g_ptr_array_index(actual, i), ==, expected[i]);
}

                                                                                              
static void test_queue_owns_actor_copy(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    f.reject_vm = "c";
    gchar *alice = g_strdup("alice");
    ops.actor = alice;
    const gchar *vms[] = { "a", "b", "c" };
    pcv_vm_batch_queue_run(vms, 3, &ops);
    g_free(alice);                                                                       
    PcvVmBatchQueue *q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);                            
    q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);                                    
    assert_strings(f.actors, (const gchar *[]){ "submit a=alice", "submit b=alice", "submit c=alice", "report c=alice", NULL });
    fixture_clear(&f);
}

                                                        
static void test_sequential_submission(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    const gchar *vms[] = { "a", "b", "c" };
    pcv_vm_batch_queue_run(vms, 3, &ops);
    assert_strings(f.submitted, (const gchar *[]){ "a", NULL });
    PcvVmBatchQueue *q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", NULL });
    q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", "c", NULL });
    q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    g_assert_null(f.pending);
    g_assert_cmpuint(f.reports->len, ==, 0);
    fixture_clear(&f);
}

                                       
static void test_sync_rejection_continues(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    f.reject_vm = "b";
    const gchar *vms[] = { "a", "b", "c" };
    pcv_vm_batch_queue_run(vms, 3, &ops);
    PcvVmBatchQueue *q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", "c", NULL });
    assert_strings(f.reports, (const gchar *[]){ "b:VM busy", NULL });
    q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    fixture_clear(&f);
}

                                                               
static void test_completion_advances_regardless_of_result(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    const gchar *vms[] = { "gone", "next" };
    pcv_vm_batch_queue_run(vms, 2, &ops);
    PcvVmBatchQueue *q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);                                        
    assert_strings(f.submitted, (const gchar *[]){ "gone", "next", NULL });
    q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    fixture_clear(&f);
}

                                      
static void test_terminating_reports_remaining(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    const gchar *vms[] = { "a", "b", "c" };
    pcv_vm_batch_queue_run(vms, 3, &ops);
    f.terminating = TRUE;
    PcvVmBatchQueue *q = f.pending; f.pending = NULL;
    pcv_vm_batch_queue_item_done(q);
    assert_strings(f.submitted, (const gchar *[]){ "a", NULL });
    assert_strings(f.reports, (const gchar *[]){ "b:daemon shutting down", "c:daemon shutting down", NULL });
    g_assert_null(f.pending);
    fixture_clear(&f);
}

                                              
static void test_empty_and_all_rejected(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    pcv_vm_batch_queue_run(NULL, 0, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 0);
    f.reject_vm = "only";
    const gchar *vms[] = { "only" };
    pcv_vm_batch_queue_run(vms, 1, &ops);
    assert_strings(f.reports, (const gchar *[]){ "only:VM busy", NULL });
    g_assert_null(f.pending);
    fixture_clear(&f);
}

static void done_one(QueueFixture *f)
{
    g_assert_cmpuint(f->active, >, 0);
    f->active--;
    pcv_vm_batch_queue_item_done(f->pending);
}

                                                               
static void test_concurrent_window(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    ops.max_in_flight = 3;
    const gchar *vms[] = { "a", "b", "c", "d", "e" };
    pcv_vm_batch_queue_run(vms, 5, &ops);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", "c", NULL });
    done_one(&f);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", "c", "d", NULL });
    done_one(&f); done_one(&f); done_one(&f); done_one(&f);
    g_assert_cmpuint(f.submitted->len, ==, 5);
    g_assert_cmpuint(f.max_active, ==, 3);
    g_assert_cmpuint(f.active, ==, 0);
    fixture_clear(&f);                                 
}

                                           
static void test_concurrent_all_rejected_frees(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    ops.max_in_flight = 2; f.reject_all = TRUE;
    const gchar *vms[] = { "a", "b", "c" };
    pcv_vm_batch_queue_run(vms, 3, &ops);
    assert_strings(f.reports, (const gchar *[]){ "a:VM busy", "b:VM busy", "c:VM busy", NULL });
    g_assert_cmpuint(f.active, ==, 0);
    fixture_clear(&f);
}

                                                    
static void test_concurrent_terminating_reports_remaining(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    ops.max_in_flight = 2;
    const gchar *vms[] = { "a", "b", "c", "d" };
    pcv_vm_batch_queue_run(vms, 4, &ops);
    f.terminating = TRUE;
    done_one(&f);
    assert_strings(f.reports, (const gchar *[]){ "c:daemon shutting down", "d:daemon shutting down", NULL });
    done_one(&f);
    g_assert_cmpuint(f.submitted->len, ==, 2);
    fixture_clear(&f);
}

                                             
static void test_zero_means_sequential(void)
{
    QueueFixture f; PcvVmBatchQueueOps ops; fixture_init(&f, &ops);
    ops.max_in_flight = 0;
    const gchar *vms[] = { "a", "b" };
    pcv_vm_batch_queue_run(vms, 2, &ops);
    assert_strings(f.submitted, (const gchar *[]){ "a", NULL });
    done_one(&f);
    assert_strings(f.submitted, (const gchar *[]){ "a", "b", NULL });
    done_one(&f);
    fixture_clear(&f);
}

                                                                        
                                                     
                                                            
typedef struct {
    GPtrArray *submitted;                  
    GPtrArray *reports;                      
    GQueue     inflight;                              
    guint      active, max_active;
    gboolean   terminating;
    PcvVmBatchSlots *slots;
} SlotsFixture;

static SlotsFixture *slots_current;

static gboolean slots_submit(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue, gpointer user_data, gchar **reason)
{
    SlotsFixture *f = user_data;
    (void)actor;
    g_ptr_array_add(f->submitted, g_strdup(vm));
    if (g_str_has_prefix(vm, "x")) {                       
        *reason = g_strdup("VM busy");
        return FALSE;
    }
    if (g_str_has_prefix(vm, "s")) {                              
        pcv_vm_batch_queue_item_done(queue);
        return TRUE;
    }
    g_queue_push_tail(&f->inflight, queue);
    f->active++;
    if (f->active > f->max_active) f->max_active = f->active;
    return TRUE;
}

static void slots_report(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data)
{
    SlotsFixture *f = user_data;
    (void)actor;
    g_ptr_array_add(f->reports, g_strdup_printf("%s:%s", vm, reason));
}

static gboolean slots_terminating(void) { return slots_current->terminating; }

static void slots_init(SlotsFixture *f, guint capacity)
{
    *f = (SlotsFixture){0};
    f->submitted = g_ptr_array_new_with_free_func(g_free);
    f->reports = g_ptr_array_new_with_free_func(g_free);
    g_queue_init(&f->inflight);
    f->slots = pcv_vm_batch_slots_new(capacity);
    slots_current = f;
}

static PcvVmBatchQueueOps slots_ops(SlotsFixture *f, guint max_in_flight)
{
    return (PcvVmBatchQueueOps){ .submit = slots_submit, .report = slots_report,
                                 .terminating = slots_terminating, .user_data = f,
                                 .max_in_flight = max_in_flight, .slots = f->slots };
}

                             
static void slots_done_oldest(SlotsFixture *f)
{
    PcvVmBatchQueue *q = g_queue_pop_head(&f->inflight);
    g_assert_nonnull(q);
    f->active--;
    pcv_vm_batch_queue_item_done(q);
}

static void slots_clear(SlotsFixture *f)
{
    g_assert_cmpuint(pcv_vm_batch_slots_in_use(f->slots), ==, 0);
    pcv_vm_batch_slots_free(f->slots);                                                  
    g_ptr_array_unref(f->submitted);
    g_ptr_array_unref(f->reports);
    slots_current = NULL;
}

                                                                                                                                           
static void test_slots_shared_cap_alternates(void)
{
    SlotsFixture f; slots_init(&f, 2);
    PcvVmBatchQueueOps oa = slots_ops(&f, 2), ob = slots_ops(&f, 2);
    pcv_vm_batch_queue_run((const gchar *[]){ "a1", "a2", "a3", "a4", NULL }, 4, &oa);
    pcv_vm_batch_queue_run((const gchar *[]){ "b1", "b2", "b3", "b4", NULL }, 4, &ob);
    assert_strings(f.submitted, (const gchar *[]){ "a1", "a2", NULL });
    while (f.active > 0) slots_done_oldest(&f);
    assert_strings(f.submitted, (const gchar *[]){ "a1", "a2", "b1", "a3", "b2", "a4", "b3", "b4", NULL });
    g_assert_cmpuint(f.max_active, ==, 2);
    slots_clear(&f);                                     
}

                                      
static void test_slots_sync_reject_returns_slot(void)
{
    SlotsFixture f; slots_init(&f, 1);
    PcvVmBatchQueueOps o = slots_ops(&f, 4);
    pcv_vm_batch_queue_run((const gchar *[]){ "x1", "a2", NULL }, 2, &o);
    assert_strings(f.reports, (const gchar *[]){ "x1:VM busy", NULL });
    assert_strings(f.submitted, (const gchar *[]){ "x1", "a2", NULL });
    g_assert_cmpuint(pcv_vm_batch_slots_in_use(f.slots), ==, 1);
    slots_done_oldest(&f);
    slots_clear(&f);
}

                                          
static void test_slots_terminating_uses_no_slot(void)
{
    SlotsFixture f; slots_init(&f, 1);
    PcvVmBatchQueueOps oa = slots_ops(&f, 2), ob = slots_ops(&f, 2);
    pcv_vm_batch_queue_run((const gchar *[]){ "a1", "a2", NULL }, 2, &oa);
    pcv_vm_batch_queue_run((const gchar *[]){ "b1", "b2", NULL }, 2, &ob);
    f.terminating = TRUE;
    slots_done_oldest(&f);
    assert_strings(f.reports, (const gchar *[]){ "a2:daemon shutting down", "b1:daemon shutting down",
                                                 "b2:daemon shutting down", NULL });                        
    g_assert_cmpuint(f.submitted->len, ==, 1);
    slots_clear(&f);
}

                                                            
static void test_slots_sync_completion_does_not_strand_waiter(void)
{
    SlotsFixture f; slots_init(&f, 1);
    PcvVmBatchQueueOps o1 = slots_ops(&f, 1), o2 = slots_ops(&f, 1), o3 = slots_ops(&f, 1);
    pcv_vm_batch_queue_run((const gchar *[]){ "a1", NULL }, 1, &o1);
    pcv_vm_batch_queue_run((const gchar *[]){ "s1", "b2", NULL }, 2, &o2);
    pcv_vm_batch_queue_run((const gchar *[]){ "c1", NULL }, 1, &o3);
    assert_strings(f.submitted, (const gchar *[]){ "a1", NULL });
    slots_done_oldest(&f);                                             
    assert_strings(f.submitted, (const gchar *[]){ "a1", "s1", "c1", NULL });
    slots_done_oldest(&f);                       
    assert_strings(f.submitted, (const gchar *[]){ "a1", "s1", "c1", "b2", NULL });
    slots_done_oldest(&f);
    slots_clear(&f);
}

void test_vm_batch_queue_register(void)
{
    g_test_add_func("/vm_batch_queue/sequential_submission", test_sequential_submission);
    g_test_add_func("/vm_batch_queue/queue_owns_actor_copy", test_queue_owns_actor_copy);
    g_test_add_func("/vm_batch_queue/sync_rejection_continues", test_sync_rejection_continues);
    g_test_add_func("/vm_batch_queue/completion_advances_regardless_of_result",
                    test_completion_advances_regardless_of_result);
    g_test_add_func("/vm_batch_queue/terminating_reports_remaining", test_terminating_reports_remaining);
    g_test_add_func("/vm_batch_queue/empty_and_all_rejected", test_empty_and_all_rejected);
    g_test_add_func("/vm_batch_queue/concurrent_window", test_concurrent_window);
    g_test_add_func("/vm_batch_queue/concurrent_all_rejected_frees", test_concurrent_all_rejected_frees);
    g_test_add_func("/vm_batch_queue/concurrent_terminating_reports_remaining",
                    test_concurrent_terminating_reports_remaining);
    g_test_add_func("/vm_batch_queue/zero_means_sequential", test_zero_means_sequential);
    g_test_add_func("/vm_batch_queue/slots_shared_cap_alternates", test_slots_shared_cap_alternates);
    g_test_add_func("/vm_batch_queue/slots_sync_reject_returns_slot", test_slots_sync_reject_returns_slot);
    g_test_add_func("/vm_batch_queue/slots_terminating_uses_no_slot", test_slots_terminating_uses_no_slot);
    g_test_add_func("/vm_batch_queue/slots_sync_completion_does_not_strand_waiter",
                    test_slots_sync_completion_does_not_strand_waiter);
}
