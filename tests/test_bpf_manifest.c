                                                                                        
                                                                                          
                                        
                                                                  
                        
  
                                                            
  
                                                       
                                     
   
#include <glib.h>
#include <glib/gstdio.h>                   
#include <string.h>                                           
#include "utils/pcv_bpf.h"

static void test_caps_lsm_detects_bpf_token(void) {
                                  
    gchar *p = g_build_filename(g_get_tmp_dir(), "pcv_lsm_test", NULL);
    g_file_set_contents(p, "lockdown,capability,landlock,yama,bpf,apparmor", -1, NULL);
    g_assert_true(pcv_bpf_caps_lsm_path(p));
    g_file_set_contents(p, "lockdown,capability,landlock,yama,apparmor", -1, NULL);
    g_assert_false(pcv_bpf_caps_lsm_path(p));               
    g_remove(p); g_free(p);
}

static void test_caps_lsm_no_substring_false_positive(void) {
                                                  
    gchar *p = g_build_filename(g_get_tmp_dir(), "pcv_lsm_test2", NULL);
    g_file_set_contents(p, "lockdown,bpfilter,apparmor", -1, NULL);
    g_assert_false(pcv_bpf_caps_lsm_path(p));
    g_remove(p); g_free(p);
}

static void test_caps_btf_missing_path_false(void) {
    g_assert_false(pcv_bpf_caps_btf_path("/nonexistent/vmlinux"));
}

                                                     
static void test_rehydrate_fresh(void) {
    g_assert_cmpint(pcv_bpf_rehydrate_decide(NULL, FALSE, FALSE, "abc"),
                    ==, PCV_BPF_REHYDRATE_FRESH);
}
static void test_rehydrate_reattach(void) {
    g_assert_cmpint(pcv_bpf_rehydrate_decide("abc", TRUE, TRUE, "abc"),
                    ==, PCV_BPF_REHYDRATE_REATTACH);
}
static void test_rehydrate_upgrade(void) {
    g_assert_cmpint(pcv_bpf_rehydrate_decide("old", TRUE, TRUE, "new"),
                    ==, PCV_BPF_REHYDRATE_UPGRADE);
}
static void test_rehydrate_orphan(void) {
    g_assert_cmpint(pcv_bpf_rehydrate_decide(NULL, TRUE, FALSE, "abc"),
                    ==, PCV_BPF_REHYDRATE_ORPHAN);
}
static void test_rehydrate_state_no_pin(void) {
                                       
    g_assert_cmpint(pcv_bpf_rehydrate_decide("abc", FALSE, TRUE, "abc"),
                    ==, PCV_BPF_REHYDRATE_FRESH);
}

                                                               
static void test_manifest_load_parses_sha(void) {
    gchar *dir = g_dir_make_tmp("pcv_bpf_XXXXXX", NULL);
    gchar *mpath = g_build_filename(dir, "manifest.json", NULL);
    g_file_set_contents(mpath,
        "[{\"name\":\"pcv_lsm\",\"file\":\"pcv_lsm.bpf.o\",\"sha256\":\"deadbeef\","
        "\"min_daemon_version\":\"2.0\",\"requires\":[\"btf\",\"lsm-bpf\"],"
        "\"hooks\":[\"bprm_check_security\"],\"loader\":\"network-tc\"}]", -1, NULL);
    GPtrArray *m = pcv_bpf_manifest_load(dir, NULL);
    g_assert_nonnull(m); g_assert_cmpuint(m->len, ==, 1);
    PcvBpfManifestEntry *e = g_ptr_array_index(m, 0);
    g_assert_cmpstr(e->sha256, ==, "deadbeef");
    g_assert_true(e->req_lsm);
    g_assert_cmpstr(e->loader, ==, "network-tc");
    g_ptr_array_unref(m);
    g_remove(mpath); g_free(mpath);
    g_rmdir(dir); g_free(dir);
}

                                                
                                                                         
                                                    
static void test_manifest_rejects_path_traversal_name(void) {
    gchar *dir = g_dir_make_tmp("pcv_bpf_trav_XXXXXX", NULL);
    gchar *mpath = g_build_filename(dir, "manifest.json", NULL);
                                                            
    g_file_set_contents(mpath,
        "[{\"name\":\"../../etc\",\"file\":\"a.o\",\"sha256\":\"aa\"},"
        "{\"name\":\"sub/dir\",\"file\":\"b.o\",\"sha256\":\"bb\"},"
        "{\"name\":\"\",\"file\":\"c.o\",\"sha256\":\"cc\"},"
        "{\"name\":\"pcv_lsm\",\"file\":\"pcv_lsm.bpf.o\",\"sha256\":\"dd\"}]", -1, NULL);
    GPtrArray *m = pcv_bpf_manifest_load(dir, NULL);
    g_assert_nonnull(m);
    g_assert_cmpuint(m->len, ==, 1);                           
    PcvBpfManifestEntry *e = g_ptr_array_index(m, 0);
    g_assert_cmpstr(e->name, ==, "pcv_lsm");                    
    g_ptr_array_unref(m);
    g_remove(mpath); g_free(mpath);
    g_rmdir(dir); g_free(dir);
}

                                                                          
                                              
                                                    
                                                                   
                                                                  
                                                     
static void test_seal_rejects_load_and_rehydrate(void) {
    if (g_test_subprocess()) {
        pcv_bpf_seal();
        g_assert_true(pcv_bpf_is_sealed());

        gchar *dir = g_dir_make_tmp("pcv_bpf_seal_XXXXXX", NULL);

                                                                   
        PcvBpfManifestEntry e = {0};
        g_strlcpy(e.name, "pcv_lsm", sizeof e.name);
        g_strlcpy(e.file, "pcv_lsm.bpf.o", sizeof e.file);
        GError *err = NULL;
        g_assert_false(pcv_bpf_load_and_pin(dir, &e, &err));
        g_assert_error(err, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD);
        g_clear_error(&err);

                                                                   
                                                            
        g_assert_false(pcv_bpf_rehydrate(dir, &err));
        g_assert_error(err, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD);
        g_clear_error(&err);

        g_rmdir(dir); g_free(dir);
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

                                                 
                                                           
                                                                        
                                                  
                                                   
                                                       
                                                                 
                     

                                                                 
static gboolean
_valid_metric_name(const gchar *name)
{
    if (!name || name[0] == '\0') return FALSE;
    if (!g_ascii_isalpha(name[0]) && name[0] != '_' && name[0] != ':') return FALSE;
    for (gsize i = 1; name[i] != '\0'; i++)
        if (!g_ascii_isalnum(name[i]) && name[i] != '_' && name[i] != ':') return FALSE;
    return TRUE;
}

                                                             
static gboolean
_valid_single_label(const gchar *label)
{
    if (!label) return FALSE;
    if (label[0] == '\0') return TRUE;                    
    const gchar *eq = strchr(label, '=');
    if (!eq || eq == label) return FALSE;
    if (eq[1] != '"' || label[strlen(label) - 1] != '"') return FALSE;
    for (const gchar *p = label; p < eq; p++)
        if (!g_ascii_isalnum(*p) && *p != '_') return FALSE;
    return TRUE;
}

static void test_metrics_tick_no_crash(void) {
                                                        
                                                  
    pcv_bpf_metrics_tick();
}

static void test_metrics_gauge_names_valid(void) {
    const gchar *names[] = {
        "pcv_bpf_programs_loaded",
        "pcv_bpf_degraded",
        "pcv_bpf_lsm_events_total",
        "pcv_bpf_ringbuf_dropped_total",
    };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        g_assert_true(_valid_metric_name(names[i]));
}

static void test_metrics_degraded_labels_valid(void) {
                                                             
    const gchar *btf_label = "subsystem=\"btf\"";
    const gchar *lsm_label = "subsystem=\"lsm\"";
    g_assert_true(_valid_single_label(btf_label));
    g_assert_true(_valid_single_label(lsm_label));
    g_assert_true(g_str_has_prefix(btf_label, "subsystem=\""));
    g_assert_true(g_str_has_prefix(lsm_label, "subsystem=\""));
}

                                                        
                                                                 
                                                               
                                                        
                                                                      
                                                         
                                                                   
static void test_count_pinned_links_counts_link_prefix_only(void) {
    gchar *dir = g_dir_make_tmp("pcv_bpf_links_XXXXXX", NULL);
                                                                
    gchar *p;
    p = g_build_filename(dir, "link_pcv_bprm", NULL);
    g_file_set_contents(p, "", 0, NULL); g_free(p);
    p = g_build_filename(dir, "link_pcv_file_open", NULL);
    g_file_set_contents(p, "", 0, NULL); g_free(p);
    p = g_build_filename(dir, "pcv_lsm_events", NULL);
    g_file_set_contents(p, "", 0, NULL); g_free(p);
    p = g_build_filename(dir, "pcv_daemon_cgroup", NULL);
    g_file_set_contents(p, "", 0, NULL); g_free(p);

    g_assert_cmpuint(pcv_bpf_count_pinned_links_path(dir), ==, 2);

    p = g_build_filename(dir, "link_pcv_bprm", NULL); g_remove(p); g_free(p);
    p = g_build_filename(dir, "link_pcv_file_open", NULL); g_remove(p); g_free(p);
    p = g_build_filename(dir, "pcv_lsm_events", NULL); g_remove(p); g_free(p);
    p = g_build_filename(dir, "pcv_daemon_cgroup", NULL); g_remove(p); g_free(p);
    g_rmdir(dir); g_free(dir);
}

static void test_count_pinned_links_empty_dir_zero(void) {
    gchar *dir = g_dir_make_tmp("pcv_bpf_links_empty_XXXXXX", NULL);
    g_assert_cmpuint(pcv_bpf_count_pinned_links_path(dir), ==, 0);
    g_rmdir(dir); g_free(dir);
}

static void test_count_pinned_links_missing_dir_zero(void) {
                                                    
    g_assert_cmpuint(pcv_bpf_count_pinned_links_path("/nonexistent/pcv_bpf_pindir"), ==, 0);
}

void test_bpf_manifest_register(void) {
    g_test_add_func("/bpf/caps_lsm_token", test_caps_lsm_detects_bpf_token);
    g_test_add_func("/bpf/caps_lsm_no_fp", test_caps_lsm_no_substring_false_positive);
    g_test_add_func("/bpf/caps_btf_missing", test_caps_btf_missing_path_false);
    g_test_add_func("/bpf/rehydrate_fresh", test_rehydrate_fresh);
    g_test_add_func("/bpf/rehydrate_reattach", test_rehydrate_reattach);
    g_test_add_func("/bpf/rehydrate_upgrade", test_rehydrate_upgrade);
    g_test_add_func("/bpf/rehydrate_orphan", test_rehydrate_orphan);
    g_test_add_func("/bpf/rehydrate_state_no_pin", test_rehydrate_state_no_pin);
    g_test_add_func("/bpf/manifest_load_sha", test_manifest_load_parses_sha);
    g_test_add_func("/bpf/manifest_rejects_traversal", test_manifest_rejects_path_traversal_name);
    g_test_add_func("/bpf/seal_rejects_load", test_seal_rejects_load_and_rehydrate);
    g_test_add_func("/bpf/metrics_tick_no_crash", test_metrics_tick_no_crash);
    g_test_add_func("/bpf/metrics_gauge_names_valid", test_metrics_gauge_names_valid);
    g_test_add_func("/bpf/metrics_degraded_labels_valid", test_metrics_degraded_labels_valid);
    g_test_add_func("/bpf/count_pinned_links_link_prefix_only", test_count_pinned_links_counts_link_prefix_only);
    g_test_add_func("/bpf/count_pinned_links_empty_dir", test_count_pinned_links_empty_dir_zero);
    g_test_add_func("/bpf/count_pinned_links_missing_dir", test_count_pinned_links_missing_dir_zero);
}
