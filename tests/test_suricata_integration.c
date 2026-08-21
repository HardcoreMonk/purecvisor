                                                                                             
                                                                                 
                                                                                   
                                                                      
                                                             
                                    
  
                                          
  
                                                          
                                                      
                                                                   
                           
  
                                      
                                                                       
                                                           
                                                 
                                              
                                                       
                                                   
                                                              
                                                                              
                                                                
                     
  
                        
                                                                       
                                                      
                                                     
                                                               
                                                       
                                                           
                                                     
                                                     
                                   
                                                                 
                                                   
                                                                  
                                        
  
                                                
                                     
   
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>                                        
#include <unistd.h>                       
#include <stdlib.h>                      
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"
#include "modules/security/security_store.h"
#include "modules/security/security_event.h"

                                                         
static const char *
_suricata_gate_skip_reason(void)
{
    if (geteuid() != 0)
        return "root 필요 — systemctl kill/restart + /var/log/suricata 쓰기(.50 실측 §3)";
    gchar *p = g_find_program_in_path("suricata");
    if (!p)
        return "suricata 미설치 — 통합 실경로 스킵(이 개발 머신 기본 상태)";
    g_free(p);
    return NULL;
}

                                                                        

#define EVE_SYN_SRC  "198.51.100.7"                                          
#define EVE_SYN_DST  "198.51.100.8"
#define EVE_SYN_SID  8675309
#define EVE_SYN_SIG  "PCV-D13-E2E-SYNTHETIC"
#define EVE_SYN_TARGET  EVE_SYN_SRC "->" EVE_SYN_DST

static gchar *
_store_tmp_path(void)
{
    return g_strdup_printf("%s/pcv-suricata-int-%u.db", g_get_tmp_dir(), g_random_int());
}

static void
_unlink_store(const gchar *db)
{
    g_unlink(db);
    gchar *wal = g_strdup_printf("%s-wal", db); g_unlink(wal); g_free(wal);
    gchar *shm = g_strdup_printf("%s-shm", db); g_unlink(shm); g_free(shm);
}

                                                             
static gboolean
_synthetic_event_present(void)
{
    JsonArray *list = pcv_security_store_list_events(0, 500, NULL, "suricata", NULL);
    if (!list)
        return FALSE;
    gboolean found = FALSE;
    guint n = json_array_get_length(list);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(list, i);
        const gchar *target = json_object_get_string_member_with_default(o, "target", "");
        if (g_strcmp0(target, EVE_SYN_TARGET) == 0) {
            found = TRUE;
            break;
        }
    }
    json_array_unref(list);
    return found;
}

static void
test_suricata_eve_ingest_root(void)
{
    const char *skip = _suricata_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

                                                       
    gchar *eve_dir = g_path_get_dirname(PCV_SURICATA_EVE_PATH);
    gboolean have_dir = g_file_test(eve_dir, G_FILE_TEST_IS_DIR);
    g_free(eve_dir);
    if (!have_dir) {
        g_test_skip("/var/log/suricata 부재 — suricata 가 아직 기동 안 함(eve tail 주입 불가)");
        return;
    }

                                                          
    gchar *db = _store_tmp_path();
    g_assert_true(pcv_security_store_open(db));

                                       
    g_assert_false(_synthetic_event_present());

                                                           
    pcv_suricata_eve_tail_start();

                                                                 
                                                  
    gchar *line = g_strdup_printf(
        "{\"timestamp\":\"2026-07-11T00:00:00.000000+0000\",\"event_type\":\"alert\","
        "\"in_iface\":\"lo\",\"src_ip\":\"%s\",\"src_port\":1234,\"dest_ip\":\"%s\","
        "\"dest_port\":80,\"proto\":\"TCP\",\"app_proto\":\"http\","
        "\"alert\":{\"signature_id\":%d,\"rev\":1,\"signature\":\"%s\","
        "\"category\":\"Misc\",\"severity\":2}}\n",
        EVE_SYN_SRC, EVE_SYN_DST, EVE_SYN_SID, EVE_SYN_SIG);

                                                              
                                                                      
                                                       
                                                      
                                                  
      
                                                   
                                                                   
                                                                
                                
    gboolean appended = FALSE;
    gboolean arrived  = FALSE;
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (g_get_monotonic_time() < deadline) {
        FILE *fp = g_fopen(PCV_SURICATA_EVE_PATH, "a");
        if (fp) {
            if (fputs(line, fp) >= 0) appended = TRUE;
            fclose(fp);
        }
        if (!appended) break;                             

                                                  
        for (int i = 0; i < 6 && !arrived; i++) {
            if (_synthetic_event_present()) { arrived = TRUE; break; }
            g_usleep(200 * 1000);
        }
        if (arrived) break;
    }

    g_free(line);
    if (!appended) {
        pcv_suricata_eve_tail_stop();
        pcv_security_store_close();
        _unlink_store(db); g_free(db);
        g_test_skip("eve.json append 실패 — 주입 불가(권한/디스크)");
        return;
    }

    pcv_suricata_eve_tail_stop();

    g_assert_true(arrived);                                           

    pcv_security_store_close();
    _unlink_store(db);
    g_free(db);
}

                                                              

static void
test_suricata_rules_rollback_root(void)
{
    const char *skip = _suricata_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

    GError *e = NULL;
    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", &e);
    g_assert_no_error(e);
    g_assert_nonnull(dir);

                                        
    gchar *rules = g_build_filename(dir, PCV_SURICATA_RULES_FILE, NULL);
    const gchar *seed =
        "alert tcp any any -> any any (msg:\"PCV-D13-SENTINEL\"; sid:4200001; rev:1;)\n";
    g_assert_true(g_file_set_contents(rules, seed, -1, &e));
    g_assert_no_error(e);

                                                              
                                                                   
                                                    
    GError *uerr = NULL;
    gboolean ok = pcv_suricata_rules_update_at(
        dir, "http://127.0.0.1:1/nonexistent.rules", "e2e-tester", NULL, &uerr);

    g_assert_false(ok);                                    
    g_assert_nonnull(uerr);                                   
    g_clear_error(&uerr);

                                    
    gchar *after = NULL; gsize alen = 0;
    g_assert_true(g_file_get_contents(rules, &after, &alen, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(after, ==, seed);
    g_free(after);

                                                      
    gchar *tmp = g_strconcat(rules, ".tmp", NULL);
    gchar *bak = g_strconcat(rules, ".bak", NULL);
    g_assert_false(g_file_test(tmp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(bak, G_FILE_TEST_EXISTS));
    g_free(tmp);
    g_free(bak);

    g_unlink(rules);
    g_rmdir(dir);
    g_free(rules);
    g_free(dir);
}

                                                                   

static void
test_suricata_engine_down_restart_root(void)
{
    const char *skip = _suricata_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

                                                   
    if (pcv_suricata_probe() != PCV_SURICATA_ACTIVE) {
        g_test_skip("suricata.service 비활성 — 재기동 관측 불가(먼저 systemctl start suricata)");
        return;
    }

                                                               
                                                          
                                                        
                          
    if (system("systemctl kill --signal=SIGKILL suricata >/dev/null 2>&1") != 0) {
        g_test_skip("systemctl kill 실패 — 크래시 주입 불가");
        return;
    }

                                               
    gboolean went_down = FALSE;
    gint64 d1 = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < d1) {
        if (pcv_suricata_probe() == PCV_SURICATA_FAILED) { went_down = TRUE; break; }
        g_usleep(200 * 1000);
    }
    g_assert_true(went_down);                                             

                                                      
                                                     
    pcv_suricata_health_start();

                                        
    gboolean recovered = FALSE;
    gint64 d2 = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < d2) {
        if (pcv_suricata_probe() == PCV_SURICATA_ACTIVE) { recovered = TRUE; break; }
        g_usleep(500 * 1000);
    }

    pcv_suricata_health_stop();

    g_assert_true(recovered);                         
}

void
test_suricata_integration_register(void)
{
    g_test_add_func("/suricata/integration/eve_ingest", test_suricata_eve_ingest_root);
    g_test_add_func("/suricata/integration/rules_rollback", test_suricata_rules_rollback_root);
    g_test_add_func("/suricata/integration/engine_down_restart", test_suricata_engine_down_restart_root);
}
