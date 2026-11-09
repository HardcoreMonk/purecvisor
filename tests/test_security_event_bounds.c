                                                                   
                                                                       
                                                         
                                                                    
                                                                            
                                                            
                                                           
  
                                        
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include "modules/security/security_event_bounds.h"
#include "modules/security/security_store.h"

                                                                   
gboolean pcv_sec_bounds_audit_take(gint source, gint severity, gboolean is_new, gint64 now);
void pcv_sec_bounds_reset_for_test(gint64 now);
void pcv_security_store_set_now_for_test(gint64 t);

#define BOUNDS_T0 ((gint64)1710000100)

                                                                     
static sqlite3 *
open_old_schema_db(gchar **path_out)
{
    gchar *path = g_strdup_printf("%s/pcv-sec-bounds-test-%u.db",
                                  g_get_tmp_dir(), g_random_int());
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    const gchar *ddl =
        "CREATE TABLE security_events ("
        "  event_id TEXT PRIMARY KEY, timestamp INTEGER NOT NULL, source TEXT NOT NULL,"
        "  type TEXT NOT NULL, severity TEXT NOT NULL, confidence INTEGER NOT NULL,"
        "  target_kind TEXT NOT NULL, target TEXT NOT NULL, summary TEXT NOT NULL,"
        "  recommended_action TEXT NOT NULL, status TEXT NOT NULL, evidence_json TEXT NOT NULL,"
        "  coalesce_key TEXT NOT NULL, occurrence_count INTEGER NOT NULL DEFAULT 1,"
        "  last_seen INTEGER NOT NULL, created_at INTEGER NOT NULL);"
        "CREATE TABLE overlay_wg_keys ("
        "  tenant TEXT NOT NULL, vm TEXT NOT NULL, pubkey TEXT NOT NULL,"
        "  privkey_enc TEXT NOT NULL, overlay_ip TEXT NOT NULL, PRIMARY KEY(tenant, vm));";
    g_assert_cmpint(sqlite3_exec(db, ddl, NULL, NULL, NULL), ==, SQLITE_OK);
    *path_out = path;
    return db;
}

static void
close_and_unlink(sqlite3 *db, gchar *path)
{
    sqlite3_close(db);
    g_unlink(path);
    g_autofree gchar *wal = g_strdup_printf("%s-wal", path);
    g_autofree gchar *shm = g_strdup_printf("%s-shm", path);
    g_unlink(wal);
    g_unlink(shm);
    g_free(path);
}

                                                                      
static gchar *
column_default(sqlite3 *db, const gchar *table, const gchar *col, gboolean *found)
{
    g_autofree gchar *sql = g_strdup_printf("PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &st, NULL), ==, SQLITE_OK);
    gchar *dflt = NULL;
    *found = FALSE;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (g_strcmp0((const gchar *)sqlite3_column_text(st, 1), col) == 0) {
            *found = TRUE;
            const guchar *d = sqlite3_column_text(st, 4);
            dflt = d ? g_strdup((const gchar *)d) : NULL;
        }
    }
    sqlite3_finalize(st);
    return dflt;
}

static gint
count_sql(sqlite3 *db, const gchar *sql)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &st, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_ROW);
    gint n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void
test_bounds_migrate_adds_columns_and_index(void)
{
    gchar *path = NULL;
    sqlite3 *db = open_old_schema_db(&path);
    GError *error = NULL;

    g_assert_true(pcv_sec_bounds_migrate(db, &error));
    g_assert_no_error(error);

    struct { const gchar *col; const gchar *want; } cols[] = {
        { "repeat_summary_at", "0" },
        { "repeat_summary_count", "0" },
        { "closed_reason", "''" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cols); i++) {
        gboolean found = FALSE;
        g_autofree gchar *dflt = column_default(db, "security_events", cols[i].col, &found);
        g_assert_true(found);
        g_assert_cmpstr(dflt, ==, cols[i].want);
    }
    gboolean slot_found = FALSE;
    g_autofree gchar *slot_dflt = column_default(db, "overlay_wg_keys", "slot", &slot_found);
    g_assert_true(slot_found);
    g_assert_cmpstr(slot_dflt, ==, "-1");
    g_assert_cmpint(count_sql(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='idx_security_events_status_last_seen'"), ==, 1);

                                                             
    g_assert_true(pcv_sec_bounds_migrate(db, &error));
    g_assert_no_error(error);

    close_and_unlink(db, path);
}

static void
test_bounds_old_schema_reopen_after_migration(void)
{
    gchar *path = NULL;
    sqlite3 *db = open_old_schema_db(&path);
    GError *error = NULL;
    g_assert_true(pcv_sec_bounds_migrate(db, &error));
    g_assert_no_error(error);

                                                                               
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO security_events("
        "event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,"
        "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at"
        ") VALUES('old-1',1710000000,'runtime','process_suspicious','warn',80,'process',"
        "'pid:1','s','','open','{}','k1',1,1710000000,1710000000)",
        NULL, NULL, NULL), ==, SQLITE_OK);

                                          
    g_assert_cmpint(count_sql(db,
        "SELECT COUNT(*) FROM (SELECT event_id,timestamp,source,type,severity,confidence,"
        "target_kind,target,summary,recommended_action,status,evidence_json,"
        "occurrence_count,last_seen,created_at FROM security_events)"), ==, 1);
    g_assert_cmpint(count_sql(db,
        "SELECT COUNT(*) FROM security_events WHERE event_id='old-1' AND "
        "repeat_summary_at=0 AND repeat_summary_count=0 AND closed_reason=''"), ==, 1);

    close_and_unlink(db, path);
}

static void
test_bounds_migrate_adds_close_columns(void)
{
                                                                          
                                                          
    gchar *path = NULL;
    sqlite3 *db = open_old_schema_db(&path);
    GError *error = NULL;
    g_assert_true(pcv_sec_bounds_migrate(db, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_sec_bounds_migrate(db, &error));
    g_assert_no_error(error);

    struct { const gchar *col; const gchar *want; } cols[] = {
        { "closed_by", "''" },
        { "closed_at", "0" },
        { "close_note", "''" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(cols); i++) {
        gboolean found = FALSE;
        g_autofree gchar *dflt = column_default(db, "security_events", cols[i].col, &found);
        g_assert_true(found);
        g_assert_cmpstr(dflt, ==, cols[i].want);
    }

    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO security_events("
        "event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,"
        "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at"
        ") VALUES('old-close-1',1710000000,'lsm','process_suspicious','warn',80,'process',"
        "'/usr/sbin/ip','s','manual_runbook','open','{}','k-close',1,1710000000,1710000000)",
        NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(count_sql(db,
        "SELECT COUNT(*) FROM security_events WHERE event_id='old-close-1' AND "
        "closed_by='' AND closed_at=0 AND close_note=''"), ==, 1);

    close_and_unlink(db, path);
}

                                        
static gint
take_n(gint source, gint severity, gboolean is_new, gint64 now, gint n)
{
    gint ok = 0;
    for (gint i = 0; i < n; i++) {
        ok += pcv_sec_bounds_audit_take(source, severity, is_new, now) ? 1 : 0;
    }
    return ok;
}

static void
test_bounds_budget_600_then_over(void)
{
    pcv_sec_bounds_reset_for_test(BOUNDS_T0);
    g_assert_cmpint(take_n(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, TRUE,
                           BOUNDS_T0, 600), ==, 600);
    g_assert_false(pcv_sec_bounds_audit_take(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT,
                                             TRUE, BOUNDS_T0));
    pcv_sec_bounds_reset_for_test(0);
}

static void
test_bounds_warn_share_capped_at_500(void)
{
                                                                       
    pcv_sec_bounds_reset_for_test(BOUNDS_T0);
    const gint s = PCV_SECURITY_SOURCE_SURICATA;
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_WARN, TRUE, BOUNDS_T0, 500), ==, 500);
    g_assert_false(pcv_sec_bounds_audit_take(s, PCV_SECURITY_SEVERITY_WARN, TRUE, BOUNDS_T0));
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0, 100), ==, 100);
    g_assert_false(pcv_sec_bounds_audit_take(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0));
    pcv_sec_bounds_reset_for_test(0);
}

static void
test_bounds_budget_per_source_independent(void)
{
    pcv_sec_bounds_reset_for_test(BOUNDS_T0);
    g_assert_cmpint(take_n(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, TRUE,
                           BOUNDS_T0, 601), ==, 600);
    g_assert_true(pcv_sec_bounds_audit_take(PCV_SECURITY_SOURCE_SURICATA,
                                            PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0));
                                
    g_assert_false(pcv_sec_bounds_audit_take(PCV_SEC_SOURCE_SLOTS, PCV_SECURITY_SEVERITY_CRIT,
                                             TRUE, BOUNDS_T0));
    g_assert_false(pcv_sec_bounds_audit_take(-1, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0));
    pcv_sec_bounds_reset_for_test(0);
}

static void
test_bounds_repeat_refusal_not_counted_as_over(void)
{
                                                                       
                                                                     
    pcv_sec_bounds_reset_for_test(BOUNDS_T0);
    const gint s = PCV_SECURITY_SOURCE_LSM;
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0, 600), ==, 600);
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_CRIT, FALSE, BOUNDS_T0, 3), ==, 0);
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0, 2), ==, 0);

    gchar *path = g_strdup_printf("%s/pcv-sec-bounds-store-%u.db", g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(path));
    pcv_security_store_set_now_for_test(BOUNDS_T0 + 3600);
    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "bounds-flood-probe", sizeof ev.event_id);
    ev.timestamp = BOUNDS_T0 + 3600;
    ev.source = PCV_SECURITY_SOURCE_LSM;
    ev.severity = PCV_SECURITY_SEVERITY_INFO;
    g_strlcpy(ev.target, "probe", sizeof ev.target);
    ev.status = PCV_SECURITY_STATUS_OPEN;
    PcvSecInsertResult r;
    GError *error = NULL;
    g_assert_true(pcv_security_store_insert_event_ex(&ev, &r, &error));
    g_assert_no_error(error);
    g_assert_cmpint(r.kind, ==, PCV_SEC_INSERT_NEW);
    g_assert_cmpstr(r.row_event_id, ==, "bounds-flood-probe");
    g_assert_false(r.audit_new);
    g_assert_cmpuint(r.flood_count[s], ==, 2);
    g_assert_cmpint(r.flood_since[s], ==, BOUNDS_T0);
    g_assert_cmpuint(r.flood_count[PCV_SECURITY_SOURCE_SURICATA], ==, 0);

    pcv_security_store_close();
    pcv_security_store_set_now_for_test(0);
    pcv_sec_bounds_reset_for_test(0);
    close_and_unlink(NULL, path);
}

static void
test_bounds_budget_clock_backwards_resets_window(void)
{
                                                 
    pcv_sec_bounds_reset_for_test(BOUNDS_T0);
    const gint s = PCV_SECURITY_SOURCE_LSM;
    g_assert_cmpint(take_n(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0, 601), ==, 600);
    g_assert_true(pcv_sec_bounds_audit_take(s, PCV_SECURITY_SEVERITY_CRIT, TRUE, BOUNDS_T0 - 10));
    pcv_sec_bounds_reset_for_test(0);
}

void
test_security_event_bounds_register(void)
{
    g_test_add_func("/security/bounds/migrate_adds_columns_and_index",
                    test_bounds_migrate_adds_columns_and_index);
    g_test_add_func("/security/bounds/old_schema_reopen_after_migration",
                    test_bounds_old_schema_reopen_after_migration);
    g_test_add_func("/security/bounds/migrate_adds_close_columns",
                    test_bounds_migrate_adds_close_columns);
    g_test_add_func("/security/bounds/budget_600_then_over", test_bounds_budget_600_then_over);
    g_test_add_func("/security/bounds/warn_share_capped_at_500",
                    test_bounds_warn_share_capped_at_500);
    g_test_add_func("/security/bounds/budget_per_source_independent",
                    test_bounds_budget_per_source_independent);
    g_test_add_func("/security/bounds/repeat_refusal_not_counted_as_over",
                    test_bounds_repeat_refusal_not_counted_as_over);
    g_test_add_func("/security/bounds/budget_clock_backwards_resets_window",
                    test_bounds_budget_clock_backwards_resets_window);
}
