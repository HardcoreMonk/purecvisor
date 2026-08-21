                                                                                              
                                                                                    
                                                                         
                                                               
                             
                                 
  
                                                     
  
                                                        
                                                
                                                                    
                     
  
          
                                                            
                                                                   
                         
                                                            
                                            
                                                                      
                                                
                                               
                                                              
                                                            
                                                        
                                                          
                                                 
                                                 
  
                                                           
                                                                
                                                   
                                                           
                                                
  
                                                  
                                   
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <unistd.h>                         
#include <stdlib.h>                       
#include "modules/daemons/pcv_trace.h"

                                                         
static const char *
_trace_gate_skip_reason(void)
{
    if (geteuid() != 0)
        return "root 필요 — retis 는 BPF 캡(SYS_ADMIN/BPF 등) 요구(.50 실측 §1)";
    gchar *p = g_find_program_in_path("retis");
    if (!p)
        return "retis 미설치 — 통합 왕복 스킵(이 개발 머신 기본 상태)";
    g_free(p);
    return NULL;
}

                                                               
static gboolean
_drive_until_idle(const char *trace_id, guint max_wait_sec)
{
    gint64 deadline = g_get_monotonic_time() + (gint64)max_wait_sec * G_USEC_PER_SEC;
    for (;;) {
        g_main_context_iteration(NULL, FALSE);

        JsonObject *st = pcv_trace_status(trace_id);
        const char *state = json_object_get_string_member(st, "state");
        gboolean idle = g_strcmp0(state, "idle") == 0;
        json_object_unref(st);
        if (idle)
            return TRUE;
        if (g_get_monotonic_time() >= deadline)
            return FALSE;
        g_usleep(20 * 1000);                        
    }
}

static void
_rm_out_dir(const char *trace_id)
{
    gchar *dir = g_build_filename(PCV_TRACE_OUT_ROOT, trace_id, NULL);
    gchar *cmd = g_strdup_printf("rm -rf '%s' >/dev/null 2>&1", dir);
    if (system(cmd) != 0) {                      }
    g_free(cmd);
    g_free(dir);
}

                          
static void
test_trace_timebox_roundtrip_root_retis(void)
{
    const char *skip = _trace_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

                                                  
    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

    PcvTraceFilter f = { .timebox_sec = 2 };                                 
    GError *e = NULL;
    gchar *id = pcv_trace_start(&f, "tester", &e);
    g_assert_no_error(e);
    g_assert_nonnull(id);

                                                                
    {
        JsonObject *st = pcv_trace_status(id);
        g_assert_cmpstr(json_object_get_string_member(st, "state"), ==, "running");
        json_object_unref(st);
        g_assert_false(pcv_trace_try_acquire());                 
    }

                                                            
    gboolean idle = _drive_until_idle(id, 2 + 10 + 8);
    g_assert_true(idle);

                                                               
    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

    gchar *marker = g_build_filename(PCV_TRACE_OUT_ROOT, id, PCV_TRACE_RUNNING_MARKER, NULL);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_free(marker);

    gchar *data = g_build_filename(PCV_TRACE_OUT_ROOT, id, "run.data", NULL);
    g_assert_true(g_file_test(data, G_FILE_TEST_EXISTS));
    g_free(data);

    _rm_out_dir(id);
    g_free(id);
}

                       
static void
test_trace_stop_root_retis(void)
{
    const char *skip = _trace_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

    PcvTraceFilter f = { .timebox_sec = 60 };                             
    GError *e = NULL;
    gchar *id = pcv_trace_start(&f, "tester", &e);
    g_assert_no_error(e);
    g_assert_nonnull(id);

    {
        JsonObject *st = pcv_trace_status(id);
        g_assert_cmpstr(json_object_get_string_member(st, "state"), ==, "running");
        json_object_unref(st);
    }

                                              
    g_assert_true(pcv_trace_stop(id, &e));
    g_assert_no_error(e);

                        
    {
        JsonObject *st = pcv_trace_status(id);
        g_assert_cmpstr(json_object_get_string_member(st, "state"), ==, "idle");
        json_object_unref(st);
    }
    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

                                          
    {
        GError *e2 = NULL;
        g_assert_false(pcv_trace_stop(id, &e2));
        g_assert_error(e2, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        g_clear_error(&e2);
    }

                                                        
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(10 * 1000);
    }

    _rm_out_dir(id);
    g_free(id);
}

                                                       
                                                                 
                              
  
                                                                     
                                                                
                                                      
                                                        
                                                        
                                                      
                          
  
                                                              
                    
static gboolean
_pgrep_has_match(const gchar *pattern)
{
    gchar *argv[] = { (gchar *)"pgrep", (gchar *)"-f", (gchar *)pattern, NULL };
    gint status = 0;

    if (!g_spawn_sync(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                          G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, NULL)) {
        return FALSE;                                     
    }
    return g_spawn_check_wait_status(status, NULL);                       
}

                                                                
static void
test_trace_restart_purge_root_retis(void)
{
    const char *skip = _trace_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

    PcvTraceFilter f = { .timebox_sec = 20 };                               
    GError *e = NULL;
    gchar *id = pcv_trace_start(&f, "tester", &e);
    g_assert_no_error(e);
    g_assert_nonnull(id);

    gchar *out_dir = g_build_filename(PCV_TRACE_OUT_ROOT, id, NULL);
    g_assert_true(g_file_test(out_dir, G_FILE_TEST_IS_DIR));

                                                          
                                                    
    g_assert_true(_pgrep_has_match(out_dir));

                                                        
                                      
    pcv_trace_purge_all();

                    
    g_assert_false(_pgrep_has_match(out_dir));

                                                        
    g_assert_false(g_file_test(out_dir, G_FILE_TEST_EXISTS));

                                                       
                                             
    g_assert_true(pcv_trace_stop(id, &e));
    g_assert_no_error(e);

                                                    
                                    
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(10 * 1000);
    }

    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();

    g_free(out_dir);
    g_free(id);
}

void
test_trace_integration_register(void)
{
    g_test_add_func("/trace/timebox_roundtrip_root_retis", test_trace_timebox_roundtrip_root_retis);
    g_test_add_func("/trace/stop_root_retis", test_trace_stop_root_retis);
    g_test_add_func("/trace/restart_purge", test_trace_restart_purge_root_retis);
}
