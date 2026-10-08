

#include <glib.h>
#include "utils/pcv_log.h"
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *fallback)
{ (void)section; (void)key; return fallback; }
void test_job_queue_register(void);
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    test_job_queue_register();
    return g_test_run();
}
