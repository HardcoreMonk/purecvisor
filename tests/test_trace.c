                                                                                  
                                                                                              
                                                                             
                                                           
                                        
                     
  
                                                                  
                                                      
                                                
   
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <utime.h>
#include <time.h>
#include "modules/daemons/pcv_trace.h"

                                                                        
                                                                  
                                                                        

static void test_filter_rejects_zero_timebox(void) {
    PcvTraceFilter f = { .vm = "vm1", .timebox_sec = 0 };
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));              
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_rejects_over_max(void) {
    PcvTraceFilter f = { .vm = "vm1", .timebox_sec = 7200 };
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));              
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_build_argv_includes_outdir(void) {
    PcvTraceFilter f = { .vm = "vm1", .timebox_sec = 30 };
    gchar **argv = pcv_trace_build_argv(&f, "/var/run/purecvisor/trace/t1");
    g_assert_nonnull(argv);
    g_assert_cmpstr(argv[0], ==, "retis");
    gboolean saw_outdir = FALSE;
    for (int i = 0; argv[i]; i++)
        if (g_strstr_len(argv[i], -1, "/var/run/purecvisor/trace/t1")) saw_outdir = TRUE;
    g_assert_true(saw_outdir);
    g_strfreev(argv);
}

                                                                        
                                                                 
                                                                        

static void test_filter_accepts_timebox_boundaries(void) {
    GError *e = NULL;
    PcvTraceFilter f1 = { .timebox_sec = 1 };
    g_assert_true(pcv_trace_filter_validate(&f1, &e));
    g_assert_no_error(e);

    PcvTraceFilter f2 = { .timebox_sec = PCV_TRACE_MAX_TIMEBOX_SEC };
    g_assert_true(pcv_trace_filter_validate(&f2, &e));
    g_assert_no_error(e);
}

                                                                        
                                                               
                                                               
                                                                        

static void test_filter_timebox_3601_rejected_3600_accepted(void) {
    PcvTraceFilter over = { .timebox_sec = 3601 };
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&over, &e));                    
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

    PcvTraceFilter at_max = { .timebox_sec = 3600 };
    g_assert_true(pcv_trace_filter_validate(&at_max, &e));                   
    g_assert_no_error(e);
}

                                                                        
                                                               
                                                              
                                                                        

static void test_filter_rejects_invalid_vm_name(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
                                                   
    g_strlcpy(f.vm, "vm1 and host 1.2.3.4", sizeof(f.vm));
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_rejects_invalid_tenant_name(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
                                     
    g_strlcpy(f.tenant, "acme; whoami", sizeof(f.tenant));
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

                                                                        
                                                                       
                                                                        

static void test_filter_rejects_invalid_proto(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.proto, "sctp", sizeof(f.proto));                
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_accepts_valid_proto(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.proto, "tcp", sizeof(f.proto));
    GError *e = NULL;
    g_assert_true(pcv_trace_filter_validate(&f, &e));
    g_assert_no_error(e);
}

                                                                        
                                                                 
                                                                        

static void test_filter_rejects_non_ip_dst(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.dst_ip, "not-an-ip", sizeof(f.dst_ip));
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_rejects_injection_string_ip(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
                                                             
    g_strlcpy(f.dst_ip, "1.2.3.4; rm", sizeof(f.dst_ip));
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_accepts_valid_ip(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.src_ip, "10.0.0.1", sizeof(f.src_ip));
    g_strlcpy(f.dst_ip, "fd00::1", sizeof(f.dst_ip));                 
    GError *e = NULL;
    g_assert_true(pcv_trace_filter_validate(&f, &e));
    g_assert_no_error(e);
}

                                                                        
                                                                 
                                                                    
                                                                        

static void test_filter_accepts_unspecified_port_zero(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .dst_port = 0 };
    GError *e = NULL;
    g_assert_true(pcv_trace_filter_validate(&f, &e));                       
    g_assert_no_error(e);
}

static void test_filter_rejects_port_over_65535(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .dst_port = 65536 };
    GError *e = NULL;
    g_assert_false(pcv_trace_filter_validate(&f, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_filter_accepts_valid_port(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .src_port = 1, .dst_port = 65535 };
    GError *e = NULL;
    g_assert_true(pcv_trace_filter_validate(&f, &e));
    g_assert_no_error(e);
}

                                                                        
                                                         
                                                                 
                                                                        

static void test_filter_rejects_port_wraparound_and_overflow(void) {
    PcvTraceFilter wrap = { .timebox_sec = 30, .src_port = G_MAXUINT };
    GError *e = NULL;
                                                                   
    g_assert_false(pcv_trace_filter_validate(&wrap, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);

    PcvTraceFilter over = { .timebox_sec = 30, .dst_port = 65536 };
    g_assert_false(pcv_trace_filter_validate(&over, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

                                                                        
                                                                 
                                                                        

static void test_pcap_expr_null_when_no_fields(void) {
    PcvTraceFilter f = { .vm = "vm1", .tenant = "acme", .timebox_sec = 30 };
    gchar *expr = pcv_trace_build_pcap_expr(&f);
    g_assert_null(expr);                                    
}

static void test_pcap_expr_proto_dst_ip_dst_port(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .dst_port = 443 };
    g_strlcpy(f.proto, "tcp", sizeof(f.proto));
    g_strlcpy(f.dst_ip, "10.0.0.5", sizeof(f.dst_ip));
    gchar *expr = pcv_trace_build_pcap_expr(&f);
    g_assert_nonnull(expr);
    g_assert_cmpstr(expr, ==, "tcp and dst host 10.0.0.5 and tcp dst port 443");
    g_free(expr);
}

static void test_pcap_expr_full_5tuple_no_proto(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .src_port = 1234, .dst_port = 80 };
    g_strlcpy(f.src_ip, "10.0.0.1", sizeof(f.src_ip));
    g_strlcpy(f.dst_ip, "10.0.0.2", sizeof(f.dst_ip));
    gchar *expr = pcv_trace_build_pcap_expr(&f);
    g_assert_nonnull(expr);
    g_assert_cmpstr(expr, ==,
        "src host 10.0.0.1 and dst host 10.0.0.2 and src port 1234 and dst port 80");
    g_free(expr);
}

static void test_pcap_expr_dst_ip_only(void) {
    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.dst_ip, "192.168.122.107", sizeof(f.dst_ip));
    gchar *expr = pcv_trace_build_pcap_expr(&f);
    g_assert_nonnull(expr);
    g_assert_cmpstr(expr, ==, "dst host 192.168.122.107");
    g_free(expr);
}

                                                                        
                                                                   
                                                                        

static void test_build_argv_omits_f_flag_when_no_filter(void) {
    PcvTraceFilter f = { .vm = "vm1", .timebox_sec = 30 };
    gchar **argv = pcv_trace_build_argv(&f, "/var/run/purecvisor/trace/t2");
    g_assert_nonnull(argv);
    for (int i = 0; argv[i]; i++)
        g_assert_cmpstr(argv[i], !=, "-f");
    g_strfreev(argv);
}

static void test_build_argv_includes_f_flag_when_filter_present(void) {
    PcvTraceFilter f = { .timebox_sec = 30, .dst_port = 443 };
    g_strlcpy(f.proto, "tcp", sizeof(f.proto));
    g_strlcpy(f.dst_ip, "10.0.0.5", sizeof(f.dst_ip));
    gchar **argv = pcv_trace_build_argv(&f, "/var/run/purecvisor/trace/t3");
    g_assert_nonnull(argv);
    gboolean saw_f = FALSE, saw_expr = FALSE;
    for (int i = 0; argv[i]; i++) {
        if (g_strcmp0(argv[i], "-f") == 0) saw_f = TRUE;
        if (g_strcmp0(argv[i], "tcp and dst host 10.0.0.5 and tcp dst port 443") == 0) saw_expr = TRUE;
    }
    g_assert_true(saw_f);
    g_assert_true(saw_expr);
    g_strfreev(argv);
}

                                                                        
                                                              
                                                                        

static void test_build_argv_shape(void) {
    PcvTraceFilter f = { .timebox_sec = 42 };
    gchar **argv = pcv_trace_build_argv(&f, "/var/run/purecvisor/trace/t4");
    g_assert_nonnull(argv);

    gchar *joined = g_strjoinv(" ", argv);
                                                      
    g_assert_nonnull(g_strstr_len(joined, -1, "collect"));
                                                                         
                                                     
    g_assert_nonnull(g_strstr_len(joined, -1, "-c skb,skb-drop,nft,ct,skb-tracking"));
    g_assert_nonnull(g_strstr_len(joined, -1, "--nft-verdicts drop"));
    g_assert_nonnull(g_strstr_len(joined, -1, "--allow-system-changes"));
    g_assert_nonnull(g_strstr_len(joined, -1, "-o /var/run/purecvisor/trace/t4/run.data"));
                                          
    g_assert_nonnull(g_strstr_len(joined, -1, "--cmd sleep 42"));
    g_free(joined);
    g_strfreev(argv);
}

                                                                        
                                                            
                                                                        

static void test_selfcheck_absent_retis_yields_explicit_error(void) {
    gchar *path = g_find_program_in_path("retis");
    if (path) {
                                                     
                                                 
        g_free(path);
        g_test_skip("retis가 이 머신에 설치되어 있음 — 부재 경로 테스트 스킵");
        return;
    }
    GError *e = NULL;
    gboolean ok = pcv_trace_selfcheck(&e);
    g_assert_false(ok);
                                           
    g_assert_nonnull(e);
    g_clear_error(&e);
}

                                               
                                             
                                                          
                             
static void test_selfcheck_present_retis_succeeds(void) {
    gchar *path = g_find_program_in_path("retis");
    if (!path) {
        g_test_skip("retis 미설치 — 정상 경로 테스트 스킵(이 머신 기본 상태)");
        return;
    }
    g_free(path);
    GError *e = NULL;
    gboolean ok = pcv_trace_selfcheck(&e);
    g_assert_true(ok);
    g_assert_no_error(e);
}

                                                                        
                                                                   
                                                             
                                                                        

static void test_concurrent_start_rejected(void) {
                                                                
                                                                 
    g_assert_true(pcv_trace_try_acquire());
    g_assert_false(pcv_trace_try_acquire());                        
    pcv_trace_release();
    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();
}

                                                                        
                                                                  
                                                            
                                                         
                                                          
                                                                        

static void test_start_selfcheck_fail_leaves_guard_free(void) {
    gchar *path = g_find_program_in_path("retis");
    if (path) {
                                                      
                                                      
        g_free(path);
        g_test_skip("retis가 이 머신에 설치되어 있음 — selfcheck 실패 경로 테스트 스킵");
        return;
    }

    PcvTraceFilter f = { .timebox_sec = 30 };
    g_strlcpy(f.vm, "vm1", sizeof(f.vm));

    GError *e = NULL;
    gchar *trace_id = pcv_trace_start(&f, "test-admin", &e);
    g_assert_null(trace_id);                               
    g_assert_nonnull(e);                                          
    g_clear_error(&e);

                                                        
                      
    g_assert_true(pcv_trace_try_acquire());
    pcv_trace_release();
}

                                                                        
                                                           
                                                                
                                                                        

static void
_retention_entry_free(PcvTraceRetentionEntry *e) {
    if (!e) return;
    g_free(e->id);
    g_free(e);
}

static PcvTraceRetentionEntry *
_mk_entry(const gchar *id, gint64 mtime, guint64 bytes) {
    PcvTraceRetentionEntry *e = g_new0(PcvTraceRetentionEntry, 1);
    e->id = g_strdup(id);
    e->mtime = mtime;
    e->bytes = bytes;
    return e;
}

                                                 
static void test_retention_evicts_oldest_over_count(void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)_retention_entry_free);
    g_ptr_array_add(entries, _mk_entry("id0", 100, 10));          
    g_ptr_array_add(entries, _mk_entry("id1", 200, 10));
    g_ptr_array_add(entries, _mk_entry("id2", 300, 10));          

    GPtrArray *ev = pcv_trace_retention_evict(entries, 2, G_MAXUINT64);
    g_assert_cmpuint(ev->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(ev, 0), ==, "id0");

    g_ptr_array_unref(ev);
    g_ptr_array_unref(entries);
}

                                               
                                    
static void test_retention_evicts_over_bytes(void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)_retention_entry_free);
    g_ptr_array_add(entries, _mk_entry("id0", 100, 100));          
    g_ptr_array_add(entries, _mk_entry("id1", 200, 100));
    g_ptr_array_add(entries, _mk_entry("id2", 300, 100));          

    GPtrArray *ev = pcv_trace_retention_evict(entries, 100, 150);
    g_assert_cmpuint(ev->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(ev, 0), ==, "id0");
    g_assert_cmpstr(g_ptr_array_index(ev, 1), ==, "id1");

    g_ptr_array_unref(ev);
    g_ptr_array_unref(entries);
}

                                                     
                                                 
static void test_retention_evict_none_when_within_limits(void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)_retention_entry_free);
    g_ptr_array_add(entries, _mk_entry("id0", 100, 100));
    g_ptr_array_add(entries, _mk_entry("id1", 200, 100));
    g_ptr_array_add(entries, _mk_entry("id2", 300, 100));

    GPtrArray *ev = pcv_trace_retention_evict(entries, 10, 100000);
    g_assert_cmpuint(ev->len, ==, 0);

    g_ptr_array_unref(ev);
    g_ptr_array_unref(entries);
}

                                                                
                                                
                                                   
  
                                                        
                                                            
                                                     
                                                          
                                          
static void test_retention_apply_root_prunes_over_count_and_preserves_running(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-retention-XXXXXX", &err);
    g_assert_nonnull(root);
    g_assert_no_error(err);

    const guint total = 12;
    time_t base = time(NULL) - 100000;
    for (guint i = 0; i < total; i++) {
        gchar *dname = g_strdup_printf("id%02u", i);
        gchar *dpath = g_build_filename(root, dname, NULL);
        g_assert_cmpint(g_mkdir_with_parents(dpath, 0700), ==, 0);
        gchar *fpath = g_build_filename(dpath, "run.data", NULL);
        g_assert_true(g_file_set_contents(fpath, "x", 1, NULL));
                                            
        struct utimbuf ut = { .actime = base + i, .modtime = base + i };
        g_assert_cmpint(g_utime(dpath, &ut), ==, 0);
        g_free(fpath); g_free(dpath); g_free(dname);
    }

                                                       
                                         
    gchar *running_path = g_build_filename(root, "orphan-running", NULL);
    g_assert_cmpint(g_mkdir_with_parents(running_path, 0700), ==, 0);
    gchar *marker = g_build_filename(running_path, PCV_TRACE_RUNNING_MARKER, NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    struct utimbuf ut_old = { .actime = base - 100, .modtime = base - 100 };
    g_assert_cmpint(g_utime(running_path, &ut_old), ==, 0);

    pcv_trace_retention_apply_root(root);

                                                                         
    gchar *evicted0 = g_build_filename(root, "id00", NULL);
    gchar *evicted1 = g_build_filename(root, "id01", NULL);
    g_assert_false(g_file_test(evicted0, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(evicted1, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(running_path, G_FILE_TEST_IS_DIR));
    g_free(evicted0); g_free(evicted1);

    for (guint i = 2; i < total; i++) {
        gchar *dname = g_strdup_printf("id%02u", i);
        gchar *dpath = g_build_filename(root, dname, NULL);
        g_assert_true(g_file_test(dpath, G_FILE_TEST_IS_DIR));
                
        gchar *fpath = g_build_filename(dpath, "run.data", NULL);
        g_remove(fpath);
        g_rmdir(dpath);
        g_free(fpath); g_free(dpath); g_free(dname);
    }

    g_remove(marker);
    g_rmdir(running_path);
    g_free(marker); g_free(running_path);
    g_rmdir(root);
    g_free(root);
}

                                                                        
                                                                   
                                                                      
                                                                        

                                                                   
                                                                    
                                                       
static const char *REPORT_FIXTURE =
    "[k] __nft_trace_packet 192.168.122.1.21426 > 192.168.122.107.12399 proto TCP flags [S] table d10test (20) chain out (1) handle 2 drop\n"
    "[k] __nft_trace_packet 192.168.122.1.30000 > 192.168.122.107.12399 proto TCP flags [S] table d10test (20) chain out (1) handle 2 drop\n"
    "[k] __nft_trace_packet 10.0.0.1.5555 > 10.0.0.2.443 proto TCP flags [S] table pcv_sg (30) chain in (2) handle 5 drop\n"
    "[tp] skb:kfree_skb drop (reason NETFILTER_DROP) 192.168.122.1.21426 > 192.168.122.107.12399\n"
    "[tp] skb:kfree_skb drop (reason NETFILTER_DROP) 10.0.0.1.5555 > 10.0.0.2.443\n"
    "[tp] net:net_dev_xmit some unrelated event line to be skipped\n"
    "\n";

static void test_report_parse_aggregates_fixture(void) {
    JsonObject *r = pcv_trace_report_parse(REPORT_FIXTURE);
    g_assert_nonnull(r);

                                              
    g_assert_cmpint(json_object_get_int_member(r, "total_events"), ==, 5);
    g_assert_false(json_object_get_boolean_member(r, "truncated"));

                                                                         
    JsonArray *nft = json_object_get_array_member(r, "nft_drops");
    g_assert_cmpuint(json_array_get_length(nft), ==, 2);
    JsonObject *n0 = json_array_get_object_element(nft, 0);
    g_assert_cmpstr(json_object_get_string_member(n0, "table"),  ==, "d10test");
    g_assert_cmpstr(json_object_get_string_member(n0, "chain"),  ==, "out");
    g_assert_cmpstr(json_object_get_string_member(n0, "handle"), ==, "2");
    g_assert_cmpint(json_object_get_int_member(n0, "count"), ==, 2);
    JsonObject *n1 = json_array_get_object_element(nft, 1);
    g_assert_cmpstr(json_object_get_string_member(n1, "table"),  ==, "pcv_sg");
    g_assert_cmpstr(json_object_get_string_member(n1, "chain"),  ==, "in");
    g_assert_cmpstr(json_object_get_string_member(n1, "handle"), ==, "5");
    g_assert_cmpint(json_object_get_int_member(n1, "count"), ==, 1);

                                               
    JsonArray *skb = json_object_get_array_member(r, "skb_drop_reasons");
    g_assert_cmpuint(json_array_get_length(skb), ==, 1);
    JsonObject *s0 = json_array_get_object_element(skb, 0);
    g_assert_cmpstr(json_object_get_string_member(s0, "reason"), ==, "NETFILTER_DROP");
    g_assert_cmpint(json_object_get_int_member(s0, "count"), ==, 2);

    json_object_unref(r);
}

static void test_report_parse_empty_input(void) {
    JsonObject *r = pcv_trace_report_parse("");
    g_assert_nonnull(r);
    g_assert_cmpint(json_object_get_int_member(r, "total_events"), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "nft_drops")), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "skb_drop_reasons")), ==, 0);
    g_assert_false(json_object_get_boolean_member(r, "truncated"));
    json_object_unref(r);

                           
    JsonObject *rn = pcv_trace_report_parse(NULL);
    g_assert_nonnull(rn);
    g_assert_cmpint(json_object_get_int_member(rn, "total_events"), ==, 0);
    json_object_unref(rn);
}

static void test_report_parse_unknown_lines_only(void) {
    JsonObject *r = pcv_trace_report_parse(
        "[tp] net:net_dev_xmit foo bar\n"
        "some random text without keywords\n"
        "another line\n");
    g_assert_nonnull(r);
    g_assert_cmpint(json_object_get_int_member(r, "total_events"), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "nft_drops")), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "skb_drop_reasons")), ==, 0);
    json_object_unref(r);
}

                                                                        
                                                                     
                                                            
                                                                   
                                                                        

static void _assert_report_root_invalid(const gchar *root, const char *id) {
    GError *e = NULL;
    JsonObject *r = pcv_trace_report_root(root, id, &e);
    g_assert_null(r);
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&e);
}

static void test_report_trace_id_rejects_injection(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-report-id-XXXXXX", &err);
    g_assert_nonnull(root);

    _assert_report_root_invalid(root, "../etc");                      
    _assert_report_root_invalid(root, "..");                          
    _assert_report_root_invalid(root, "a/b");                       
    _assert_report_root_invalid(root, "/etc/passwd");                 
    _assert_report_root_invalid(root, "");                            
    _assert_report_root_invalid(root, "123456-ABCD");                   
    _assert_report_root_invalid(root, "123456-abc");                    
    _assert_report_root_invalid(root, "123456-abcde");                  
    _assert_report_root_invalid(root, "abc-1234");                      
    _assert_report_root_invalid(root, "-abcd");                        
               
    gchar *huge = g_strnfill(200, '1');
    _assert_report_root_invalid(root, huge);
    g_free(huge);

    g_rmdir(root);
    g_free(root);
}

static void test_report_wellformed_id_passes_validation_then_not_found(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-report-nf-XXXXXX", &err);
    g_assert_nonnull(root);

                                                          
                                                             
    GError *e = NULL;
    JsonObject *r = pcv_trace_report_root(root, "1720000000000000-abcd", &e);
    g_assert_null(r);
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&e);

    g_rmdir(root);
    g_free(root);
}

static void test_report_running_marker_yields_busy(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-report-busy-XXXXXX", &err);
    g_assert_nonnull(root);

    const char *id = "1720000000000000-beef";
    gchar *dir = g_build_filename(root, id, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    gchar *data = g_build_filename(dir, "run.data", NULL);
    g_assert_true(g_file_set_contents(data, "x", 1, NULL));
    gchar *marker = g_build_filename(dir, PCV_TRACE_RUNNING_MARKER, NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));

                                                                
                    
    GError *e = NULL;
    JsonObject *r = pcv_trace_report_root(root, id, &e);
    g_assert_null(r);
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_clear_error(&e);

    g_remove(marker); g_remove(data); g_rmdir(dir); g_rmdir(root);
    g_free(marker); g_free(data); g_free(dir); g_free(root);
}

                                                          
                                                        
                                    
static void test_report_marker_without_data_yields_busy(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-report-busy2-XXXXXX", &err);
    g_assert_nonnull(root);

    const char *id = "1720000000000000-cafe";
    gchar *dir = g_build_filename(root, id, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
                             
    gchar *marker = g_build_filename(dir, PCV_TRACE_RUNNING_MARKER, NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));

    GError *e = NULL;
    JsonObject *r = pcv_trace_report_root(root, id, &e);
    g_assert_null(r);
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_BUSY);                    
    g_clear_error(&e);

    g_remove(marker); g_rmdir(dir); g_rmdir(root);
    g_free(marker); g_free(dir); g_free(root);
}

                                                                        
                                                                          
                                                                        

static JsonObject *_find_trace_in_list(JsonArray *arr, const char *trace_id) {
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (g_strcmp0(json_object_get_string_member(o, "trace_id"), trace_id) == 0)
            return o;
    }
    return NULL;
}

static void test_list_root_smoke(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-list-XXXXXX", &err);
    g_assert_nonnull(root);

                                                         
    gchar *d1 = g_build_filename(root, "111-aaaa", NULL);
    gchar *d2 = g_build_filename(root, "222-bbbb", NULL);
    g_assert_cmpint(g_mkdir_with_parents(d1, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(d2, 0700), ==, 0);
    gchar *f1 = g_build_filename(d1, "run.data", NULL);
    gchar *f2 = g_build_filename(d2, "run.data", NULL);
    g_assert_true(g_file_set_contents(f1, "abcde", 5, NULL));
    g_assert_true(g_file_set_contents(f2, "xy", 2, NULL));
    gchar *m2 = g_build_filename(d2, PCV_TRACE_RUNNING_MARKER, NULL);
    g_assert_true(g_file_set_contents(m2, "", 0, NULL));

    JsonArray *arr = pcv_trace_list_root(root);
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 2);

    JsonObject *o1 = _find_trace_in_list(arr, "111-aaaa");
    JsonObject *o2 = _find_trace_in_list(arr, "222-bbbb");
    g_assert_nonnull(o1);
    g_assert_nonnull(o2);

                                                          
    g_assert_false(json_object_get_boolean_member(o1, "running"));
    g_assert_false(json_object_get_boolean_member(o1, "active"));                    
    g_assert_cmpint(json_object_get_int_member(o1, "bytes"), >=, 5);
    g_assert_true(json_object_has_member(o1, "mtime"));

                                                                       
    g_assert_true(json_object_get_boolean_member(o2, "running"));
    g_assert_false(json_object_get_boolean_member(o2, "active"));

    json_array_unref(arr);
    g_remove(m2); g_remove(f1); g_remove(f2);
    g_rmdir(d1); g_rmdir(d2); g_rmdir(root);
    g_free(m2); g_free(f1); g_free(f2); g_free(d1); g_free(d2); g_free(root);
}

static void test_list_root_absent_returns_empty(void) {
                                   
    JsonArray *arr = pcv_trace_list_root("/nonexistent/pcv/trace/root/xyz");
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 0);
    json_array_unref(arr);
}

                                                                        
                                                                        
                                                   
                                                                        

static void test_purge_all_root_removes_only_running_marked_dirs(void) {
    GError *err = NULL;
    gchar *root = g_dir_make_tmp("pcv-trace-purge-XXXXXX", &err);
    g_assert_nonnull(root);
    g_assert_no_error(err);

                                       
    const char *orphan_ids[] = { "111-aaaa", "222-bbbb" };
    for (guint i = 0; i < G_N_ELEMENTS(orphan_ids); i++) {
        gchar *dpath = g_build_filename(root, orphan_ids[i], NULL);
        g_assert_cmpint(g_mkdir_with_parents(dpath, 0700), ==, 0);
        gchar *marker = g_build_filename(dpath, PCV_TRACE_RUNNING_MARKER, NULL);
        g_assert_true(g_file_set_contents(marker, "", 0, NULL));
        g_free(marker);
        g_free(dpath);
    }

                                                
    gchar *done_path = g_build_filename(root, "333-cccc", NULL);
    g_assert_cmpint(g_mkdir_with_parents(done_path, 0700), ==, 0);
    gchar *done_data = g_build_filename(done_path, "run.data", NULL);
    g_assert_true(g_file_set_contents(done_data, "x", 1, NULL));

    pcv_trace_purge_all_root(root);

    for (guint i = 0; i < G_N_ELEMENTS(orphan_ids); i++) {
        gchar *dpath = g_build_filename(root, orphan_ids[i], NULL);
        g_assert_false(g_file_test(dpath, G_FILE_TEST_EXISTS));
        g_free(dpath);
    }
    g_assert_true(g_file_test(done_path, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(done_data, G_FILE_TEST_EXISTS));

                                                    
    g_remove(done_data);
    g_rmdir(done_path);
    g_free(done_data); g_free(done_path);
    g_rmdir(root);
    g_free(root);
}

static void test_purge_all_root_absent_root_is_noop(void) {
                                                  
    pcv_trace_purge_all_root("/nonexistent/pcv/trace/purge/xyz");
}

void test_trace_register(void) {
    g_test_add_func("/trace/concurrent_start_rejected", test_concurrent_start_rejected);
    g_test_add_func("/trace/reject_zero_timebox", test_filter_rejects_zero_timebox);
    g_test_add_func("/trace/reject_over_max", test_filter_rejects_over_max);
    g_test_add_func("/trace/argv_outdir", test_build_argv_includes_outdir);
    g_test_add_func("/trace/timebox_boundaries_accepted", test_filter_accepts_timebox_boundaries);
    g_test_add_func("/trace/timebox_3601_rejected_3600_accepted", test_filter_timebox_3601_rejected_3600_accepted);
    g_test_add_func("/trace/reject_invalid_vm_name", test_filter_rejects_invalid_vm_name);
    g_test_add_func("/trace/reject_invalid_tenant_name", test_filter_rejects_invalid_tenant_name);
    g_test_add_func("/trace/reject_invalid_proto", test_filter_rejects_invalid_proto);
    g_test_add_func("/trace/accept_valid_proto", test_filter_accepts_valid_proto);
    g_test_add_func("/trace/reject_non_ip_dst", test_filter_rejects_non_ip_dst);
    g_test_add_func("/trace/reject_injection_string_ip", test_filter_rejects_injection_string_ip);
    g_test_add_func("/trace/accept_valid_ip", test_filter_accepts_valid_ip);
    g_test_add_func("/trace/accept_unspecified_port_zero", test_filter_accepts_unspecified_port_zero);
    g_test_add_func("/trace/reject_port_over_65535", test_filter_rejects_port_over_65535);
    g_test_add_func("/trace/accept_valid_port", test_filter_accepts_valid_port);
    g_test_add_func("/trace/reject_port_wraparound_and_overflow", test_filter_rejects_port_wraparound_and_overflow);
    g_test_add_func("/trace/pcap_expr_null_when_no_fields", test_pcap_expr_null_when_no_fields);
    g_test_add_func("/trace/pcap_expr_proto_dst_ip_dst_port", test_pcap_expr_proto_dst_ip_dst_port);
    g_test_add_func("/trace/pcap_expr_full_5tuple_no_proto", test_pcap_expr_full_5tuple_no_proto);
    g_test_add_func("/trace/pcap_expr_dst_ip_only", test_pcap_expr_dst_ip_only);
    g_test_add_func("/trace/argv_omits_f_when_no_filter", test_build_argv_omits_f_flag_when_no_filter);
    g_test_add_func("/trace/argv_includes_f_when_filter_present", test_build_argv_includes_f_flag_when_filter_present);
    g_test_add_func("/trace/argv_shape", test_build_argv_shape);
    g_test_add_func("/trace/selfcheck_absent_retis", test_selfcheck_absent_retis_yields_explicit_error);
    g_test_add_func("/trace/selfcheck_present_retis", test_selfcheck_present_retis_succeeds);
    g_test_add_func("/trace/start_selfcheck_fail_leaves_guard_free", test_start_selfcheck_fail_leaves_guard_free);
    g_test_add_func("/trace/retention_evicts_oldest_over_count", test_retention_evicts_oldest_over_count);
    g_test_add_func("/trace/retention_evicts_over_bytes", test_retention_evicts_over_bytes);
    g_test_add_func("/trace/retention_evict_none_when_within_limits", test_retention_evict_none_when_within_limits);
    g_test_add_func("/trace/retention_apply_root_prunes_over_count_and_preserves_running",
                     test_retention_apply_root_prunes_over_count_and_preserves_running);
                                                              
    g_test_add_func("/trace/report_parse_aggregates_fixture", test_report_parse_aggregates_fixture);
    g_test_add_func("/trace/report_parse_empty_input", test_report_parse_empty_input);
    g_test_add_func("/trace/report_parse_unknown_lines_only", test_report_parse_unknown_lines_only);
    g_test_add_func("/trace/report_trace_id_rejects_injection", test_report_trace_id_rejects_injection);
    g_test_add_func("/trace/report_wellformed_id_passes_validation_then_not_found",
                     test_report_wellformed_id_passes_validation_then_not_found);
    g_test_add_func("/trace/report_running_marker_yields_busy", test_report_running_marker_yields_busy);
    g_test_add_func("/trace/report_marker_without_data_yields_busy", test_report_marker_without_data_yields_busy);
    g_test_add_func("/trace/list_root_smoke", test_list_root_smoke);
    g_test_add_func("/trace/list_root_absent_returns_empty", test_list_root_absent_returns_empty);
                                                          
    g_test_add_func("/trace/purge_all_root_removes_only_running_marked_dirs",
                     test_purge_all_root_removes_only_running_marked_dirs);
    g_test_add_func("/trace/purge_all_root_absent_root_is_noop",
                     test_purge_all_root_absent_root_is_noop);
}
