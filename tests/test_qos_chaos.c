                                                                                    
                                                                           
                                                                    
                                                             
                                     
                         
  
                                                     
                                                             
                                                        
                                              
                                                               
                                               
                                      
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/network/pcv_qos_chaos.h"

                                                                        
                                                                
                                                                        

static void test_chaos_profile_delay_valid(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 100ms", &e);
    g_assert_nonnull(tok);
    g_assert_no_error(e);
    g_assert_cmpstr(tok[0], ==, "delay");
    g_assert_cmpstr(tok[1], ==, "100ms");
    g_assert_null(tok[2]);
    g_strfreev(tok);
}

static void test_chaos_profile_delay_boundary_min(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 1ms", &e);
    g_assert_nonnull(tok);
    g_assert_no_error(e);
    g_strfreev(tok);
}

static void test_chaos_profile_delay_boundary_max(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 10000ms", &e);
    g_assert_nonnull(tok);
    g_assert_no_error(e);
    g_strfreev(tok);
}

static void test_chaos_profile_delay_zero_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 0ms", &e);
    g_assert_null(tok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

static void test_chaos_profile_delay_over_max_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 10001ms", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_loss_valid(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 5%", &e);
    g_assert_nonnull(tok);
    g_assert_cmpstr(tok[0], ==, "loss");
    g_assert_cmpstr(tok[1], ==, "5%");
    g_strfreev(tok);
}

static void test_chaos_profile_loss_boundary_min(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 1%", &e);
    g_assert_nonnull(tok);
    g_strfreev(tok);
}

static void test_chaos_profile_loss_boundary_max(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 100%", &e);
    g_assert_nonnull(tok);
    g_strfreev(tok);
}

static void test_chaos_profile_loss_zero_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 0%", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_loss_over_100_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 101%", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_reorder_valid(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("reorder 25% 50%", &e);
    g_assert_nonnull(tok);
    g_assert_cmpstr(tok[0], ==, "reorder");
    g_assert_cmpstr(tok[1], ==, "25%");
    g_assert_cmpstr(tok[2], ==, "50%");
    g_assert_null(tok[3]);
    g_strfreev(tok);
}

static void test_chaos_profile_reorder_boundary(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("reorder 1% 100%", &e);
    g_assert_nonnull(tok);
    g_strfreev(tok);
}

static void test_chaos_profile_reorder_second_pct_over_100_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("reorder 25% 101%", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_reorder_first_pct_zero_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("reorder 0% 50%", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_null_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate(NULL, &e);
    g_assert_null(tok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

static void test_chaos_profile_empty_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_unknown_format_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("corrupt 10%", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_case_sensitive_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("DELAY 100ms", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_negative_number_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay -5ms", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

                                               
                                                  
                                           
                                          

static void test_chaos_profile_injection_semicolon_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 100ms; rm -rf /", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_injection_and_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 5% && evil", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_injection_newline_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("loss 5%\ntc qdisc del dev pcvqos0", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_injection_extra_reorder_token_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("reorder 25% 50%; cat /etc/passwd", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_injection_trailing_junk_on_number_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay 100$(whoami)ms", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

static void test_chaos_profile_too_long_rejected(void) {
    GError *e = NULL;
    gchar *huge = g_strnfill(200, 'a');
    gchar **tok = pcv_qos_chaos_profile_validate(huge, &e);
    g_assert_null(tok);
    g_clear_error(&e);
    g_free(huge);
}

static void test_chaos_profile_consecutive_spaces_rejected(void) {
    GError *e = NULL;
    gchar **tok = pcv_qos_chaos_profile_validate("delay  100ms", &e);
    g_assert_null(tok);
    g_clear_error(&e);
}

                                                                        
                                                                       
                                                                        

static void test_chaos_timebox_zero_rejected(void) {
    g_assert_false(pcv_qos_chaos_timebox_valid(0));
}
static void test_chaos_timebox_one_accepted(void) {
    g_assert_true(pcv_qos_chaos_timebox_valid(1));
}
static void test_chaos_timebox_max_accepted(void) {
    g_assert_true(pcv_qos_chaos_timebox_valid(3600));
}
static void test_chaos_timebox_over_max_rejected(void) {
    g_assert_false(pcv_qos_chaos_timebox_valid(3601));
}

                                                                        
                                                                       
                                                                        

static void test_chaos_dry_run_default_null_params(void) {
    g_assert_true(pcv_qos_chaos_resolve_dry_run(NULL));
}
static void test_chaos_dry_run_default_missing_member(void) {
    JsonObject *o = json_object_new();
    g_assert_true(pcv_qos_chaos_resolve_dry_run(o));
    json_object_unref(o);
}
static void test_chaos_dry_run_explicit_true(void) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "dry_run", TRUE);
    g_assert_true(pcv_qos_chaos_resolve_dry_run(o));
    json_object_unref(o);
}
static void test_chaos_dry_run_explicit_false(void) {
    JsonObject *o = json_object_new();
    json_object_set_boolean_member(o, "dry_run", FALSE);
    g_assert_false(pcv_qos_chaos_resolve_dry_run(o));
    json_object_unref(o);
}
static void test_chaos_dry_run_non_boolean_defaults_true(void) {
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "dry_run", "false");                        
    g_assert_true(pcv_qos_chaos_resolve_dry_run(o));
    json_object_unref(o);
}

                                                                        
                                                                        
                                                                        

static void test_chaos_parse_netem_parents_finds_all(void) {
    const gchar *sample =
        "qdisc hfsc 1: root refcnt 2 default 0xfffe\n"
        "qdisc netem 8001: parent 1:1a2b limit 1000 delay 100.0ms\n"
        "qdisc cake 8002: parent 1:1a2c limit 1000 besteffort\n"
        "qdisc netem 8003: parent 1:1a2d limit 1000 loss 5%\n";
    GPtrArray *out = pcv_qos_chaos_parse_netem_parents(sample);
    g_assert_cmpuint(out->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(out, 0), ==, "1:1a2b");
    g_assert_cmpstr(g_ptr_array_index(out, 1), ==, "1:1a2d");
    g_ptr_array_unref(out);
}

static void test_chaos_parse_netem_parents_no_netem(void) {
    const gchar *sample = "qdisc hfsc 1: root refcnt 2 default 0xfffe\n";
    GPtrArray *out = pcv_qos_chaos_parse_netem_parents(sample);
    g_assert_cmpuint(out->len, ==, 0);
    g_ptr_array_unref(out);
}

static void test_chaos_parse_netem_parents_null_input(void) {
    GPtrArray *out = pcv_qos_chaos_parse_netem_parents(NULL);
    g_assert_nonnull(out);
    g_assert_cmpuint(out->len, ==, 0);
    g_ptr_array_unref(out);
}

static void test_chaos_parse_netem_parents_empty_input(void) {
    GPtrArray *out = pcv_qos_chaos_parse_netem_parents("");
    g_assert_nonnull(out);
    g_assert_cmpuint(out->len, ==, 0);
    g_ptr_array_unref(out);
}

                                                                        
                                                               
                                                       
                                                       
                                            
                                                                            

static void test_chaos_start_untracked_vm_rejected_dry_run(void) {
    GError *e = NULL;
    gboolean ok = pcv_qos_chaos_start("t7-chaos-untracked-1", "delay 100ms", 5,
                                       "admin1", TRUE, &e);
    g_assert_false(ok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

static void test_chaos_start_untracked_vm_rejected_real(void) {
    GError *e = NULL;
    gboolean ok = pcv_qos_chaos_start("t7-chaos-untracked-2", "delay 100ms", 5,
                                       "admin1", FALSE, &e);
    g_assert_false(ok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

                                                      
                                                   
static void test_chaos_start_invalid_timebox_rejected_before_vm_lookup(void) {
    GError *e = NULL;
    gboolean ok = pcv_qos_chaos_start("t7-chaos-any-vm", "delay 100ms", 0,
                                       "admin1", TRUE, &e);
    g_assert_false(ok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

                                         
static void test_chaos_start_invalid_profile_rejected_before_vm_lookup(void) {
    GError *e = NULL;
    gboolean ok = pcv_qos_chaos_start("t7-chaos-any-vm", "bogus", 5,
                                       "admin1", TRUE, &e);
    g_assert_false(ok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

static void test_chaos_stop_no_active_rejected(void) {
    GError *e = NULL;
    gboolean ok = pcv_qos_chaos_stop("t7-chaos-no-active-vm", "admin1", &e);
    g_assert_false(ok);
    g_assert_nonnull(e);
    g_clear_error(&e);
}

static void test_chaos_status_empty_when_none_active(void) {
    pcv_qos_chaos_clear();
    GPtrArray *st = pcv_qos_chaos_status();
    g_assert_nonnull(st);
    g_assert_cmpuint(st->len, ==, 0);
    g_ptr_array_unref(st);
}

void
test_qos_chaos_register(void)
{
    g_test_add_func("/qos/chaos_profile_delay_valid", test_chaos_profile_delay_valid);
    g_test_add_func("/qos/chaos_profile_delay_boundary_min", test_chaos_profile_delay_boundary_min);
    g_test_add_func("/qos/chaos_profile_delay_boundary_max", test_chaos_profile_delay_boundary_max);
    g_test_add_func("/qos/chaos_profile_delay_zero_rejected", test_chaos_profile_delay_zero_rejected);
    g_test_add_func("/qos/chaos_profile_delay_over_max_rejected", test_chaos_profile_delay_over_max_rejected);
    g_test_add_func("/qos/chaos_profile_loss_valid", test_chaos_profile_loss_valid);
    g_test_add_func("/qos/chaos_profile_loss_boundary_min", test_chaos_profile_loss_boundary_min);
    g_test_add_func("/qos/chaos_profile_loss_boundary_max", test_chaos_profile_loss_boundary_max);
    g_test_add_func("/qos/chaos_profile_loss_zero_rejected", test_chaos_profile_loss_zero_rejected);
    g_test_add_func("/qos/chaos_profile_loss_over_100_rejected", test_chaos_profile_loss_over_100_rejected);
    g_test_add_func("/qos/chaos_profile_reorder_valid", test_chaos_profile_reorder_valid);
    g_test_add_func("/qos/chaos_profile_reorder_boundary", test_chaos_profile_reorder_boundary);
    g_test_add_func("/qos/chaos_profile_reorder_second_pct_over_100_rejected", test_chaos_profile_reorder_second_pct_over_100_rejected);
    g_test_add_func("/qos/chaos_profile_reorder_first_pct_zero_rejected", test_chaos_profile_reorder_first_pct_zero_rejected);
    g_test_add_func("/qos/chaos_profile_null_rejected", test_chaos_profile_null_rejected);
    g_test_add_func("/qos/chaos_profile_empty_rejected", test_chaos_profile_empty_rejected);
    g_test_add_func("/qos/chaos_profile_unknown_format_rejected", test_chaos_profile_unknown_format_rejected);
    g_test_add_func("/qos/chaos_profile_case_sensitive_rejected", test_chaos_profile_case_sensitive_rejected);
    g_test_add_func("/qos/chaos_profile_negative_number_rejected", test_chaos_profile_negative_number_rejected);
    g_test_add_func("/qos/chaos_profile_injection_semicolon_rejected", test_chaos_profile_injection_semicolon_rejected);
    g_test_add_func("/qos/chaos_profile_injection_and_rejected", test_chaos_profile_injection_and_rejected);
    g_test_add_func("/qos/chaos_profile_injection_newline_rejected", test_chaos_profile_injection_newline_rejected);
    g_test_add_func("/qos/chaos_profile_injection_extra_reorder_token_rejected", test_chaos_profile_injection_extra_reorder_token_rejected);
    g_test_add_func("/qos/chaos_profile_injection_trailing_junk_on_number_rejected", test_chaos_profile_injection_trailing_junk_on_number_rejected);
    g_test_add_func("/qos/chaos_profile_too_long_rejected", test_chaos_profile_too_long_rejected);
    g_test_add_func("/qos/chaos_profile_consecutive_spaces_rejected", test_chaos_profile_consecutive_spaces_rejected);

    g_test_add_func("/qos/chaos_timebox_zero_rejected", test_chaos_timebox_zero_rejected);
    g_test_add_func("/qos/chaos_timebox_one_accepted", test_chaos_timebox_one_accepted);
    g_test_add_func("/qos/chaos_timebox_max_accepted", test_chaos_timebox_max_accepted);
    g_test_add_func("/qos/chaos_timebox_over_max_rejected", test_chaos_timebox_over_max_rejected);

    g_test_add_func("/qos/chaos_dry_run_default_null_params", test_chaos_dry_run_default_null_params);
    g_test_add_func("/qos/chaos_dry_run_default_missing_member", test_chaos_dry_run_default_missing_member);
    g_test_add_func("/qos/chaos_dry_run_explicit_true", test_chaos_dry_run_explicit_true);
    g_test_add_func("/qos/chaos_dry_run_explicit_false", test_chaos_dry_run_explicit_false);
    g_test_add_func("/qos/chaos_dry_run_non_boolean_defaults_true", test_chaos_dry_run_non_boolean_defaults_true);

    g_test_add_func("/qos/chaos_parse_netem_parents_finds_all", test_chaos_parse_netem_parents_finds_all);
    g_test_add_func("/qos/chaos_parse_netem_parents_no_netem", test_chaos_parse_netem_parents_no_netem);
    g_test_add_func("/qos/chaos_parse_netem_parents_null_input", test_chaos_parse_netem_parents_null_input);
    g_test_add_func("/qos/chaos_parse_netem_parents_empty_input", test_chaos_parse_netem_parents_empty_input);

    g_test_add_func("/qos/chaos_start_untracked_vm_rejected_dry_run", test_chaos_start_untracked_vm_rejected_dry_run);
    g_test_add_func("/qos/chaos_start_untracked_vm_rejected_real", test_chaos_start_untracked_vm_rejected_real);
    g_test_add_func("/qos/chaos_start_invalid_timebox_rejected_before_vm_lookup", test_chaos_start_invalid_timebox_rejected_before_vm_lookup);
    g_test_add_func("/qos/chaos_start_invalid_profile_rejected_before_vm_lookup", test_chaos_start_invalid_profile_rejected_before_vm_lookup);
    g_test_add_func("/qos/chaos_stop_no_active_rejected", test_chaos_stop_no_active_rejected);
    g_test_add_func("/qos/chaos_status_empty_when_none_active", test_chaos_status_empty_when_none_active);
}
