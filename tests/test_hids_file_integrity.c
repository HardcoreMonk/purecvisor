#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <unistd.h>
#include "modules/security/hids_file_integrity.h"

void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_target(void);

/*
 * File integrity tests keep baseline refresh explicit: scan reports drift after
 * a trusted refresh, but scan itself never changes trust state.
 */
static void
cleanup_security_db(const gchar *path)
{
    gchar *wal = g_strdup_printf("%s-wal", path);
    gchar *shm = g_strdup_printf("%s-shm", path);
    g_unlink(path);
    g_unlink(wal);
    g_unlink(shm);
    g_free(wal);
    g_free(shm);
}

static void
test_hids_file_integrity_baseline_refresh_scan(void)
{
    GError *error = NULL;
    gchar *tmp_dir = g_dir_make_tmp("pcv-hids-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmp_dir);

    gchar *file_path = g_build_filename(tmp_dir, "daemon.conf", NULL);
    gchar *db_path = g_build_filename(tmp_dir, "pcv_security.db", NULL);
    const gchar *paths[] = { file_path };

    g_assert_true(g_file_set_contents(file_path, "trusted", -1, &error));
    g_assert_no_error(error);

    cleanup_security_db(db_path);
    pcv_test_audit_reset();

    g_assert_cmpint(pcv_hids_baseline_status(db_path), ==, PCV_HIDS_BASELINE_UNKNOWN);
    g_assert_true(pcv_hids_baseline_refresh(db_path, paths, G_N_ELEMENTS(paths),
                                            "admin", &error));
    g_assert_no_error(error);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.baseline.refresh");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, "file_baseline");
    g_assert_cmpint(pcv_hids_baseline_status(db_path), ==, PCV_HIDS_BASELINE_TRUSTED);

    GPtrArray *clean = pcv_hids_file_integrity_scan(db_path, paths, G_N_ELEMENTS(paths));
    g_assert_nonnull(clean);
    g_assert_cmpuint(clean->len, ==, 0);
    g_ptr_array_unref(clean);

    g_assert_true(g_file_set_contents(file_path, "changed", -1, &error));
    g_assert_no_error(error);

    GPtrArray *changed = pcv_hids_file_integrity_scan(db_path, paths, G_N_ELEMENTS(paths));
    g_assert_nonnull(changed);
    g_assert_cmpuint(changed->len, ==, 1);

    JsonObject *event = g_ptr_array_index(changed, 0);
    g_assert_cmpstr(json_object_get_string_member(event, "path"), ==, file_path);
    g_assert_cmpstr(json_object_get_string_member(event, "status"), ==, "changed");
    g_ptr_array_unref(changed);

    cleanup_security_db(db_path);
    g_unlink(file_path);
    g_rmdir(tmp_dir);
    g_free(file_path);
    g_free(db_path);
    g_free(tmp_dir);
}

static void
test_hids_file_integrity_rejects_symlink_swap(void)
{
    /*
     * Tamper-evidence: once a real file is baselined, swapping it for a symlink
     * (even one whose target has identical content) must NOT silently validate.
     * compute_file_state now opens with O_NOFOLLOW, so the open fails and the
     * public scan reports the path as unreadable instead of clean/changed.
     */
    GError *error = NULL;
    gchar *tmp_dir = g_dir_make_tmp("pcv-hids-symlink-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmp_dir);

    gchar *file_path = g_build_filename(tmp_dir, "guarded.conf", NULL);
    gchar *decoy_path = g_build_filename(tmp_dir, "decoy.conf", NULL);
    gchar *db_path = g_build_filename(tmp_dir, "pcv_security.db", NULL);
    const gchar *paths[] = { file_path };

    /* Baseline a real regular file whose content matches the decoy exactly. */
    g_assert_true(g_file_set_contents(file_path, "trusted", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(decoy_path, "trusted", -1, &error));
    g_assert_no_error(error);

    cleanup_security_db(db_path);
    pcv_test_audit_reset();

    g_assert_true(pcv_hids_baseline_refresh(db_path, paths, G_N_ELEMENTS(paths),
                                            "admin", &error));
    g_assert_no_error(error);

    /* Baseline is clean before tampering. */
    GPtrArray *clean = pcv_hids_file_integrity_scan(db_path, paths, G_N_ELEMENTS(paths));
    g_assert_nonnull(clean);
    g_assert_cmpuint(clean->len, ==, 0);
    g_ptr_array_unref(clean);

    /* Swap the guarded path for a symlink pointing at the same-content decoy. */
    g_assert_cmpint(g_unlink(file_path), ==, 0);
    g_assert_cmpint(symlink(decoy_path, file_path), ==, 0);

    GPtrArray *swapped = pcv_hids_file_integrity_scan(db_path, paths, G_N_ELEMENTS(paths));
    g_assert_nonnull(swapped);
    g_assert_cmpuint(swapped->len, ==, 1);
    JsonObject *event = g_ptr_array_index(swapped, 0);
    g_assert_cmpstr(json_object_get_string_member(event, "path"), ==, file_path);
    g_assert_cmpstr(json_object_get_string_member(event, "status"), ==, "unreadable");
    g_ptr_array_unref(swapped);

    cleanup_security_db(db_path);
    g_unlink(file_path);
    g_unlink(decoy_path);
    g_rmdir(tmp_dir);
    g_free(file_path);
    g_free(decoy_path);
    g_free(db_path);
    g_free(tmp_dir);
}

void
test_hids_file_integrity_register(void)
{
    g_test_add_func("/security/file_integrity/baseline-refresh-scan",
                    test_hids_file_integrity_baseline_refresh_scan);
    g_test_add_func("/security/file_integrity/rejects-symlink-swap",
                    test_hids_file_integrity_rejects_symlink_swap);
}
