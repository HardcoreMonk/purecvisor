                                                                                                      
                                                                       
                                                                              
#include <glib.h>
#include <json-glib/json-glib.h>
#include "../src/modules/dispatcher/vm_delete_status.h"

                                                                               
static JsonObject *job_row(const gchar *status, gboolean outcome_known, const gchar *detail,
                           const gchar *reason, const gchar *result)
{
    JsonObject *job = json_object_new();
    json_object_set_string_member(job, "job_id", "job-0000beef");
    json_object_set_string_member(job, "status", status);
    json_object_set_int_member(job, "progress", 0);
    if (detail) json_object_set_string_member(job, "detail", detail);
    if (reason) json_object_set_string_member(job, "recovery_reason", reason);
    if (result) json_object_set_string_member(job, "result", result);
    json_object_set_boolean_member(job, "outcome_known", outcome_known);
    json_object_set_int_member(job, "created_at", 1760000000);
    json_object_set_int_member(job, "updated_at", 1760000042);
    return job;
}

static JsonObject *parse_object(const gchar *text)
{
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, text, -1, NULL));
    JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    return o;
}

static void test_job_result_shapes(void)
{
    gchar *plain = pcv_vm_delete_job_result("web", FALSE, NULL, NULL);
    g_assert_cmpstr(plain, ==, "{\"name\":\"web\"}");
    gchar *absent = pcv_vm_delete_job_result("web", TRUE, NULL, NULL);
    g_assert_cmpstr(absent, ==, "{\"name\":\"web\",\"already_absent\":true}");
    GPtrArray *w = g_ptr_array_new_with_free_func(g_free);
    gchar *empty = pcv_vm_delete_job_result("web", FALSE, w, NULL);                 
    g_assert_cmpstr(empty, ==, "{\"name\":\"web\"}");
    g_ptr_array_add(w, g_strdup("NVRAM cleanup failed: /var/lib/libvirt/qemu/nvram/web_VARS.fd (Is a directory); manual cleanup is required"));
    gchar *warned = pcv_vm_delete_job_result("web", FALSE, w, NULL);
    JsonObject *o = parse_object(warned);
    JsonArray *a = json_object_get_array_member(o, "warnings");
    g_assert_cmpuint(json_array_get_length(a), ==, 1);
    g_assert_true(g_str_has_prefix(json_array_get_string_element(a, 0), "NVRAM cleanup failed: "));
    g_assert_false(json_object_has_member(o, "error"));
    json_object_unref(o);
    gchar *failed = pcv_vm_delete_job_result("web", FALSE, NULL, "ZFS destroy failed (VM definition rolled back): busy");
    g_assert_cmpstr(failed, ==, "{\"name\":\"web\",\"error\":\"ZFS destroy failed (VM definition rolled back): busy\"}");
    g_free(plain); g_free(absent); g_free(empty); g_free(warned); g_free(failed);
    g_ptr_array_unref(w);
}

static void test_status_not_found(void)
{
    JsonObject *s = pcv_vm_delete_status_build("web", NULL);
    g_assert_cmpstr(json_object_get_string_member(s, "name"), ==, "web");
    g_assert_cmpstr(json_object_get_string_member(s, "status"), ==, "not_found");
    g_assert_false(json_object_has_member(s, "job_id"));
    json_object_unref(s);
}

static void test_status_completed_plain(void)
{
    JsonObject *job = job_row("completed", TRUE, "VM deletion started", NULL, "{\"name\":\"web\"}");
    JsonObject *s = pcv_vm_delete_status_build("web", job);
    g_assert_cmpstr(json_object_get_string_member(s, "status"), ==, "completed");
    g_assert_cmpstr(json_object_get_string_member(s, "job_id"), ==, "job-0000beef");
    g_assert_cmpstr(json_object_get_string_member(s, "detail"), ==, "VM deletion started");
    g_assert_true(json_object_get_boolean_member(s, "outcome_known"));
    g_assert_cmpint(json_object_get_int_member(s, "updated_at"), ==, 1760000042);
    g_assert_false(json_object_has_member(s, "recovery_reason"));
    g_assert_false(json_object_has_member(s, "warnings"));
    g_assert_false(json_object_has_member(s, "already_absent"));
    g_assert_false(json_object_has_member(s, "error"));
    json_object_unref(s); json_object_unref(job);
}

static void test_status_completed_warnings_lifted(void)
{
    JsonObject *job = job_row("completed", TRUE, NULL, NULL,
        "{\"name\":\"web\",\"warnings\":[\"storage exorcism was partial; see daemon log\"]}");
    JsonObject *s = pcv_vm_delete_status_build("web", job);
    JsonArray *a = json_object_get_array_member(s, "warnings");                           
    g_assert_nonnull(a);
    g_assert_cmpstr(json_array_get_string_element(a, 0), ==, "storage exorcism was partial; see daemon log");
    json_object_unref(s); json_object_unref(job);
}

static void test_status_completed_already_absent(void)
{
    JsonObject *job = job_row("completed", TRUE, NULL, NULL, "{\"name\":\"ghost\",\"already_absent\":true}");
    JsonObject *s = pcv_vm_delete_status_build("ghost", job);
    g_assert_true(json_object_get_boolean_member(s, "already_absent"));
    json_object_unref(s); json_object_unref(job);
}

static void test_status_failed_error(void)
{
    JsonObject *job = job_row("failed", TRUE, NULL, NULL,
        "{\"name\":\"web\",\"error\":\"Disk file cleanup failed: /x.qcow2 (Permission denied). Delete can be retried.\"}");
    JsonObject *s = pcv_vm_delete_status_build("web", job);
    g_assert_cmpstr(json_object_get_string_member(s, "status"), ==, "failed");
    g_assert_true(g_str_has_prefix(json_object_get_string_member(s, "error"), "Disk file cleanup failed"));
    json_object_unref(s); json_object_unref(job);
}

static void test_status_interrupted_unknown_outcome(void)
{
    JsonObject *job = job_row("interrupted", FALSE, NULL, "daemon_restart", NULL);
    JsonObject *s = pcv_vm_delete_status_build("web", job);
    g_assert_cmpstr(json_object_get_string_member(s, "status"), ==, "interrupted");
    g_assert_false(json_object_get_boolean_member(s, "outcome_known"));
    g_assert_cmpstr(json_object_get_string_member(s, "recovery_reason"), ==, "daemon_restart");
    json_object_unref(s); json_object_unref(job);
}

static void test_status_unparsable_result_ignored(void)
{
    JsonObject *job = job_row("completed", TRUE, NULL, NULL, "not-json");
    JsonObject *s = pcv_vm_delete_status_build("web", job);
    g_assert_cmpstr(json_object_get_string_member(s, "status"), ==, "completed");
    g_assert_false(json_object_has_member(s, "warnings"));
    g_assert_false(json_object_has_member(s, "error"));
    json_object_unref(s); json_object_unref(job);
}

void test_vm_delete_status_register(void)
{
    g_test_add_func("/vm_delete_status/job_result_shapes", test_job_result_shapes);
    g_test_add_func("/vm_delete_status/not_found", test_status_not_found);
    g_test_add_func("/vm_delete_status/completed_plain", test_status_completed_plain);
    g_test_add_func("/vm_delete_status/completed_warnings_lifted", test_status_completed_warnings_lifted);
    g_test_add_func("/vm_delete_status/completed_already_absent", test_status_completed_already_absent);
    g_test_add_func("/vm_delete_status/failed_error", test_status_failed_error);
    g_test_add_func("/vm_delete_status/interrupted_unknown_outcome", test_status_interrupted_unknown_outcome);
    g_test_add_func("/vm_delete_status/unparsable_result_ignored", test_status_unparsable_result_ignored);
}
