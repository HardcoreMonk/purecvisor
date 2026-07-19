
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

static gboolean
_validate_backup_policy_params(const gchar *vm_name,
                               gint         interval_hours,
                               gint         retention_count)
{

    if (!vm_name || vm_name[0] == '\0') return FALSE;
    if (g_strcmp0(vm_name, "*") != 0 && !pcv_validate_vm_name(vm_name))
        return FALSE;

    if (interval_hours < 1 || interval_hours > 8760) return FALSE;

    if (retention_count < 1 || retention_count > 365) return FALSE;
    return TRUE;
}

static void test_backup_policy_valid(void) {
    g_assert_true(_validate_backup_policy_params("web-prod", 24, 7));
    g_assert_true(_validate_backup_policy_params("db01", 1, 1));
    g_assert_true(_validate_backup_policy_params("*", 12, 30));
}

static void test_backup_policy_null_name(void) {
    g_assert_false(_validate_backup_policy_params(NULL, 24, 7));
}

static void test_backup_policy_empty_name(void) {
    g_assert_false(_validate_backup_policy_params("", 24, 7));
}

static void test_backup_policy_invalid_name(void) {
    g_assert_false(_validate_backup_policy_params("web prod", 24, 7));
    g_assert_false(_validate_backup_policy_params("vm;rm", 24, 7));
    g_assert_false(_validate_backup_policy_params("../../etc", 24, 7));
}

static void test_backup_policy_invalid_interval(void) {
    g_assert_false(_validate_backup_policy_params("web-prod", 0, 7));
    g_assert_false(_validate_backup_policy_params("web-prod", -1, 7));
}

static void test_backup_policy_invalid_retention(void) {
    g_assert_false(_validate_backup_policy_params("web-prod", 24, 0));
    g_assert_false(_validate_backup_policy_params("web-prod", 24, -5));
}

static void test_backup_policy_upper_bound(void) {

    g_assert_false(_validate_backup_policy_params("web-prod", 8761, 7));
    g_assert_false(_validate_backup_policy_params("web-prod", 99999, 7));

    g_assert_false(_validate_backup_policy_params("web-prod", 24, 366));
    g_assert_false(_validate_backup_policy_params("web-prod", 24, 9999));

    g_assert_true(_validate_backup_policy_params("web-prod", 8760, 365));

    g_assert_false(_validate_backup_policy_params("web-prod", 8760, 366));
    g_assert_false(_validate_backup_policy_params("web-prod", 8761, 365));
}

static gboolean
_is_restore_allowed_state(const gchar *vm_state)
{
    if (!vm_state) return FALSE;

    if (g_strcmp0(vm_state, "stopped") == 0) return TRUE;
    if (g_strcmp0(vm_state, "shutoff") == 0) return TRUE;

    return FALSE;
}

static void test_backup_restore_state_validation(void) {

    g_assert_true(_is_restore_allowed_state("stopped"));
    g_assert_true(_is_restore_allowed_state("shutoff"));

    g_assert_false(_is_restore_allowed_state("migrating"));
    g_assert_false(_is_restore_allowed_state("saving"));

    g_assert_false(_is_restore_allowed_state(NULL));
    g_assert_false(_is_restore_allowed_state("running"));
    g_assert_false(_is_restore_allowed_state("paused"));
}

static gboolean
_validate_auto_snapshot_name(const gchar *name)
{
    if (!name) return FALSE;

    if (!g_str_has_prefix(name, "pcv-auto-")) return FALSE;

    const gchar *ts = name + strlen("pcv-auto-");
    if (strlen(ts) != 15) return FALSE;

    for (int i = 0; i < 8; i++)
        if (!g_ascii_isdigit(ts[i])) return FALSE;
    if (ts[8] != '-') return FALSE;
    for (int i = 9; i < 15; i++)
        if (!g_ascii_isdigit(ts[i])) return FALSE;
    return TRUE;
}

static void test_backup_snapshot_name_valid(void) {
    g_assert_true(_validate_auto_snapshot_name("pcv-auto-20260324-100000"));
    g_assert_true(_validate_auto_snapshot_name("pcv-auto-20261231-235959"));
}

static void test_backup_snapshot_name_invalid(void) {
    g_assert_false(_validate_auto_snapshot_name(NULL));
    g_assert_false(_validate_auto_snapshot_name(""));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-"));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-2026032"));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-20260324100000"));
    g_assert_false(_validate_auto_snapshot_name("manual-snap"));
    g_assert_false(_validate_auto_snapshot_name("pcv-auto-abcdefgh-123456"));
}

static void test_backup_policy_json_roundtrip(void) {

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", "web-prod");
    json_object_set_int_member(obj, "interval_hours", 24);
    json_object_set_int_member(obj, "retention_count", 7);
    json_object_set_boolean_member(obj, "enabled", TRUE);

    g_assert_cmpstr(json_object_get_string_member(obj, "vm_name"), ==, "web-prod");
    g_assert_cmpint(json_object_get_int_member(obj, "interval_hours"), ==, 24);
    g_assert_cmpint(json_object_get_int_member(obj, "retention_count"), ==, 7);
    g_assert_true(json_object_get_boolean_member(obj, "enabled"));

    json_object_unref(obj);
}

static void test_backup_policy_wildcard(void) {

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm_name", "*");
    json_object_set_int_member(obj, "interval_hours", 12);
    json_object_set_int_member(obj, "retention_count", 14);
    json_object_set_boolean_member(obj, "enabled", TRUE);

    g_assert_cmpstr(json_object_get_string_member(obj, "vm_name"), ==, "*");
    g_assert_cmpint(json_object_get_int_member(obj, "interval_hours"), ==, 12);

    json_object_unref(obj);
}

static void test_backup_incremental_result_json(void) {

    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "snapshot", "pcv-auto-20260324-100000");
    json_object_set_string_member(result, "base_snapshot", "pcv-auto-20260323-100000");
    json_object_set_string_member(result, "file", "/var/lib/purecvisor/backups/web-prod_incr_20260324.zfs");
    json_object_set_int_member(result, "size_bytes", 1048576);

    g_assert_true(json_object_has_member(result, "snapshot"));
    g_assert_true(json_object_has_member(result, "base_snapshot"));
    g_assert_true(json_object_has_member(result, "file"));
    g_assert_true(json_object_has_member(result, "size_bytes"));
    g_assert_cmpint(json_object_get_int_member(result, "size_bytes"), >, 0);

    json_object_unref(result);
}

void test_backup_basic_register(void) {
    g_test_add_func("/backup/policy/valid",              test_backup_policy_valid);
    g_test_add_func("/backup/policy/null_name",          test_backup_policy_null_name);
    g_test_add_func("/backup/policy/empty_name",         test_backup_policy_empty_name);
    g_test_add_func("/backup/policy/invalid_name",       test_backup_policy_invalid_name);
    g_test_add_func("/backup/policy/invalid_interval",   test_backup_policy_invalid_interval);
    g_test_add_func("/backup/policy/invalid_retention",  test_backup_policy_invalid_retention);
    g_test_add_func("/backup/policy/upper_bound",        test_backup_policy_upper_bound);
    g_test_add_func("/backup/restore/state_validation",  test_backup_restore_state_validation);
    g_test_add_func("/backup/snapshot_name/valid",       test_backup_snapshot_name_valid);
    g_test_add_func("/backup/snapshot_name/invalid",     test_backup_snapshot_name_invalid);
    g_test_add_func("/backup/policy/json_roundtrip",     test_backup_policy_json_roundtrip);
    g_test_add_func("/backup/policy/wildcard",           test_backup_policy_wildcard);
    g_test_add_func("/backup/incremental/result_json",   test_backup_incremental_result_json);
}
