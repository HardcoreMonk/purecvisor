




















#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>







static gboolean
validate_zfs_token(const gchar *s)
{
    if (!s || *s == '\0' || strlen(s) > 128)
        return FALSE;
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}








static const gchar *
get_param(JsonObject *params, const gchar *primary, const gchar *fallback)
{
    if (!params) return NULL;
    if (json_object_has_member(params, primary))
        return json_object_get_string_member(params, primary);
    if (fallback && json_object_has_member(params, fallback))
        return json_object_get_string_member(params, fallback);
    return NULL;
}



typedef enum {
    SNAP_VALID_OK = 0,
    SNAP_VALID_MISSING_PARAMS,
    SNAP_VALID_INVALID_CHARS,
} SnapValidateResult;

static SnapValidateResult
validate_rollback_params(JsonObject *params)
{
    if (!params) return SNAP_VALID_MISSING_PARAMS;

    const gchar *vm_name  = get_param(params, "name", "vm_id");
    const gchar *snap_name = get_param(params, "snapshot_name", "snap_name");

    if (!vm_name || vm_name[0] == '\0' ||
        !snap_name || snap_name[0] == '\0')
        return SNAP_VALID_MISSING_PARAMS;

    if (!validate_zfs_token(vm_name) || !validate_zfs_token(snap_name))
        return SNAP_VALID_INVALID_CHARS;

    return SNAP_VALID_OK;
}




#define TEST_VIR_DOMAIN_RUNNING  1
#define TEST_VIR_DOMAIN_BLOCKED  2
#define TEST_VIR_DOMAIN_PAUSED   3
#define TEST_VIR_DOMAIN_SHUTDOWN 4
#define TEST_VIR_DOMAIN_SHUTOFF  5
#define TEST_VIR_DOMAIN_CRASHED  6
#define TEST_VIR_DOMAIN_PMSUSPENDED 7





static gboolean
rollback_was_running(int domain_state)
{
    return (domain_state == TEST_VIR_DOMAIN_RUNNING ||
            domain_state == TEST_VIR_DOMAIN_PAUSED);
}









static gchar *
build_zfs_dataset_path(const gchar *pool,
                        const gchar *vm_name,
                        const gchar *snap_name)
{
    return g_strdup_printf("%s/%s@%s", pool, vm_name, snap_name);
}





static void test_rollback_missing_vm_name(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "snapshot_name", "daily-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

static void test_rollback_missing_snap_name(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}





static void test_rollback_invalid_vm_name_semicolon(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web;rm -rf /");
    json_object_set_string_member(params, "snapshot_name", "daily-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_dotdot(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "../etc/passwd");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_dollar(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "$HOME");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_vm_name_slash(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web/prod");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}

static void test_rollback_invalid_snap_name_chars(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "snap@evil");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    json_object_unref(params);
}





static void test_rollback_empty_vm_name(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "");
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}

static void test_rollback_empty_snap_name(void) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_MISSING_PARAMS);

    json_object_unref(params);
}





static void test_rollback_overlong_vm_name(void) {

    gchar *long_name = g_strnfill(129, 'a');
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", long_name);
    json_object_set_string_member(params, "snapshot_name", "snap-01");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_INVALID_CHARS);

    g_free(long_name);
    json_object_unref(params);
}





static void test_rollback_valid_params(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "web-prod");
    json_object_set_string_member(params, "snapshot_name", "daily-20260410");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_OK);

    json_object_unref(params);
}

static void test_rollback_dual_key_support(void) {

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", "web-prod");
    json_object_set_string_member(params, "snap_name", "daily-20260410");

    SnapValidateResult r = validate_rollback_params(params);
    g_assert_cmpint(r, ==, SNAP_VALID_OK);


    const gchar *vm  = get_param(params, "name", "vm_id");
    const gchar *sn  = get_param(params, "snapshot_name", "snap_name");
    g_assert_cmpstr(vm, ==, "web-prod");
    g_assert_cmpstr(sn, ==, "daily-20260410");

    json_object_unref(params);
}





static void test_rollback_state_running(void) {

    g_assert_true(rollback_was_running(TEST_VIR_DOMAIN_RUNNING));
}

static void test_rollback_state_paused(void) {

    g_assert_true(rollback_was_running(TEST_VIR_DOMAIN_PAUSED));
}

static void test_rollback_state_shutoff(void) {

    g_assert_false(rollback_was_running(TEST_VIR_DOMAIN_SHUTOFF));
}

static void test_rollback_state_crashed(void) {

    g_assert_false(rollback_was_running(TEST_VIR_DOMAIN_CRASHED));
}





static void test_rollback_zfs_dataset_path(void) {
    gchar *path = build_zfs_dataset_path("pcvpool/vms", "web01", "daily-01");
    g_assert_cmpstr(path, ==, "pcvpool/vms/web01@daily-01");
    g_free(path);
}

static void test_rollback_zfs_dataset_path_with_underscores(void) {

    gchar *path = build_zfs_dataset_path("pcvpool/vms", "db_primary", "auto_2026_04_10");
    g_assert_cmpstr(path, ==, "pcvpool/vms/db_primary@auto_2026_04_10");
    g_free(path);
}





static void test_zfs_token_valid_alphanum(void) {
    g_assert_true(validate_zfs_token("web01"));
    g_assert_true(validate_zfs_token("web-prod"));
    g_assert_true(validate_zfs_token("snap_20260410"));
    g_assert_true(validate_zfs_token("A1B2C3"));
}

static void test_zfs_token_invalid_null(void) {
    g_assert_false(validate_zfs_token(NULL));
}

static void test_zfs_token_invalid_empty(void) {
    g_assert_false(validate_zfs_token(""));
}

static void test_zfs_token_boundary_128(void) {

    gchar *name = g_strnfill(128, 'a');
    g_assert_true(validate_zfs_token(name));
    g_free(name);
}

static void test_zfs_token_boundary_129(void) {

    gchar *name = g_strnfill(129, 'a');
    g_assert_false(validate_zfs_token(name));
    g_free(name);
}





void test_snapshot_rollback_register(void) {

    g_test_add_func("/snapshot/rollback/missing_vm_name",
                    test_rollback_missing_vm_name);
    g_test_add_func("/snapshot/rollback/missing_snap_name",
                    test_rollback_missing_snap_name);


    g_test_add_func("/snapshot/rollback/invalid_vm_name_semicolon",
                    test_rollback_invalid_vm_name_semicolon);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_dotdot",
                    test_rollback_invalid_vm_name_dotdot);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_dollar",
                    test_rollback_invalid_vm_name_dollar);
    g_test_add_func("/snapshot/rollback/invalid_vm_name_slash",
                    test_rollback_invalid_vm_name_slash);
    g_test_add_func("/snapshot/rollback/invalid_snap_name_chars",
                    test_rollback_invalid_snap_name_chars);


    g_test_add_func("/snapshot/rollback/empty_vm_name",
                    test_rollback_empty_vm_name);
    g_test_add_func("/snapshot/rollback/empty_snap_name",
                    test_rollback_empty_snap_name);


    g_test_add_func("/snapshot/rollback/overlong_vm_name",
                    test_rollback_overlong_vm_name);


    g_test_add_func("/snapshot/rollback/valid_params",
                    test_rollback_valid_params);
    g_test_add_func("/snapshot/rollback/dual_key_support",
                    test_rollback_dual_key_support);


    g_test_add_func("/snapshot/rollback/state_running",
                    test_rollback_state_running);
    g_test_add_func("/snapshot/rollback/state_paused",
                    test_rollback_state_paused);
    g_test_add_func("/snapshot/rollback/state_shutoff",
                    test_rollback_state_shutoff);
    g_test_add_func("/snapshot/rollback/state_crashed",
                    test_rollback_state_crashed);


    g_test_add_func("/snapshot/rollback/zfs_dataset_path",
                    test_rollback_zfs_dataset_path);
    g_test_add_func("/snapshot/rollback/zfs_dataset_path_underscores",
                    test_rollback_zfs_dataset_path_with_underscores);


    g_test_add_func("/snapshot/rollback/zfs_token_valid",
                    test_zfs_token_valid_alphanum);
    g_test_add_func("/snapshot/rollback/zfs_token_null",
                    test_zfs_token_invalid_null);
    g_test_add_func("/snapshot/rollback/zfs_token_empty",
                    test_zfs_token_invalid_empty);
    g_test_add_func("/snapshot/rollback/zfs_token_boundary_128",
                    test_zfs_token_boundary_128);
    g_test_add_func("/snapshot/rollback/zfs_token_boundary_129",
                    test_zfs_token_boundary_129);
}
