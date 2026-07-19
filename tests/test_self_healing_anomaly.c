
#include <glib.h>
#include "modules/ai/self_healing.h"

extern gboolean pcv_healing_should_trigger_agent_now(void);

#define HAMMER_THREADS 3
#define HAMMER_ITERS   30000

static const char *const MET_A = "aio1_hammer_alpha";
static const char *const MET_B = "aio1_hammer_beta";

static gpointer
_writer(gpointer d)
{
    const char *metric = d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        pcv_healing_on_anomaly(metric, 1.0, 5.0, 2.0, NULL);
    return NULL;
}

static gpointer
_reader(gpointer d)
{
    (void)d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        (void)pcv_healing_should_trigger_agent_now();
    return NULL;
}

static void
test_anomaly_track_race(void)
{
    pcv_healing_init();

    GThread *th[HAMMER_THREADS];
    th[0] = g_thread_new("aio1-w0", _writer, (gpointer)MET_A);
    th[1] = g_thread_new("aio1-w1", _writer, (gpointer)MET_B);
    th[2] = g_thread_new("aio1-rd", _reader, NULL);
    for (int i = 0; i < HAMMER_THREADS; i++)
        g_thread_join(th[i]);

    g_assert_true(pcv_healing_get_mode());

    (void)pcv_healing_should_trigger_agent_now();
    pcv_healing_on_anomaly(MET_A, 1.0, 5.0, 2.0, NULL);
}

void
test_self_healing_anomaly_register(void)
{
    g_test_add_func("/selfhealing/anomaly_track_race", test_anomaly_track_race);
}
