                                                                  
                                                                 
                                       
                                                 
                                                
#include "pcv_job_queue.h"
#include "utils/pcv_log.h"
#include <json-glib/json-glib.h>
#include <stdio.h>

void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *fallback)
{ (void)section; (void)key; return fallback; }

                                        
static void dump(void)
{
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, pcv_job_list(1000));
    gchar *text = json_to_string(node, FALSE);
    puts(text); g_free(text); json_node_free(node);
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    g_setenv("PCV_JOBS_DB_PATH", argv[2], TRUE);
    pcv_job_queue_init();
    if (g_str_equal(argv[1], "create")) {
        gchar *id = pcv_job_create("probe.previous", "previous-target", NULL);
        g_assert_nonnull(id);
        g_assert_true(pcv_job_set_result(id, PCV_JOB_COMPLETED, "previous-result"));
        g_free(id);
    } else if (g_str_equal(argv[1], "cleanup")) pcv_job_queue_cleanup_old(168);
    else if (!g_str_equal(argv[1], "read")) return 2;
    dump(); pcv_job_queue_shutdown();
    return 0;
}
