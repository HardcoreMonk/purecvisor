


























































































#include "../../utils/pcv_config.h"
#include "vm_state.h"
#include <sqlite3.h>
#include <glib.h>
#include <libvirt/libvirt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>











#define DB_DIR  "/var/lib/purecvisor"
#define DB_PATH "/var/lib/purecvisor/vm_state.db"














#define LOCK_EXPIRY_DEFAULT   300
#define LOCK_EXPIRY_FAST      60
#define LOCK_EXPIRY_ZFS       600
#define LOCK_EXPIRY_MIGRATE   1800


#define LOCK_EXPIRY_SEC       LOCK_EXPIRY_MIGRATE






static gint _lock_expiry_for_op(gint op_type) {
    switch (op_type) {
        case 1:
        case 4:
            return LOCK_EXPIRY_ZFS;
        case 2:
        case 3:
            return LOCK_EXPIRY_FAST;
        case 6:
            return LOCK_EXPIRY_MIGRATE;
        default:
            return LOCK_EXPIRY_DEFAULT;
    }
}

















static sqlite3  *g_db   = NULL;














static GMutex    g_db_mutex;






















static gboolean pid_is_alive(gint pid) {
    if (pid <= 0) return FALSE;

    return (kill((pid_t)pid, 0) == 0);
}


















static void log_sqlite_error(const gchar *context, int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        g_warning("[vm_state] %s: SQLite error %d: %s", context, rc, sqlite3_errmsg(g_db));
    }
}



























void init_pending_state_machine(void) {
    g_mutex_init(&g_db_mutex);



    const gchar *env_path = g_getenv("PCV_VM_STATE_DB_PATH");
    const gchar *db_path = (env_path && *env_path) ? env_path : pcv_config_get_db_path();
    gchar *db_dir = g_path_get_dirname(db_path);
    if (g_mkdir_with_parents(db_dir, 0700) != 0) {
        g_warning("[vm_state] Failed to create DB directory: %s", db_dir);
    }


    int rc = sqlite3_open(db_path, &g_db);
    g_free(db_dir);
    if (rc != SQLITE_OK) {
        g_critical("[vm_state] Cannot open database '%s': %s", db_path, sqlite3_errmsg(g_db));
        g_db = NULL;
        return;
    }


    rc = sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA journal_mode=WAL", rc);


    rc = sqlite3_exec(g_db, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA wal_autocheckpoint", rc);


    rc = sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA busy_timeout", rc);


    const gchar *create_sql =
        "CREATE TABLE IF NOT EXISTS vm_locks ("
        "  vm_id      TEXT    PRIMARY KEY,"
        "  op_type    INTEGER NOT NULL,"
        "  pid        INTEGER NOT NULL,"
        "  locked_at  INTEGER NOT NULL"
        ");";
    rc = sqlite3_exec(g_db, create_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_critical("[vm_state] Failed to create vm_locks table: %s", sqlite3_errmsg(g_db));
        return;
    }














    const gchar *select_sql = "SELECT vm_id, pid FROM vm_locks;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(g_db, select_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {




        GPtrArray *stale_ids = g_ptr_array_new_with_free_func(g_free);


        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *vm_id = (const gchar *)sqlite3_column_text(stmt, 0);
            gint         pid   = sqlite3_column_int(stmt, 1);

            if (!pid_is_alive(pid)) {

                g_ptr_array_add(stale_ids, g_strdup(vm_id));
            }
        }
        sqlite3_finalize(stmt);


        for (guint i = 0; i < stale_ids->len; i++) {
            const gchar *stale_id = g_ptr_array_index(stale_ids, i);






            sqlite3_stmt *del_stmt = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del_stmt, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, stale_id, -1, SQLITE_STATIC);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
            }
            g_message("[vm_state] Reconcile: released stale lock for VM '%s' (dead PID %d)",
                      stale_id, 0);
        }

        if (stale_ids->len > 0) {
            g_message("[vm_state] Reconcile: %u stale lock(s) released.", stale_ids->len);
        } else {
            g_message("[vm_state] Reconcile: no stale locks found.");
        }
        g_ptr_array_free(stale_ids, TRUE);
    }

    g_message("[vm_state] SQLite WAL state machine initialized at '%s'.", db_path);
}


































gboolean lock_vm_operation(const gchar *vm_id, gint op_type, gchar **err_msg) {
    if (!g_db) {


        g_warning("[vm_state] DB not initialized, skipping lock for VM '%s'", vm_id);
        return TRUE;
    }

    g_mutex_lock(&g_db_mutex);

    gboolean result = FALSE;
    sqlite3_stmt *stmt = NULL;












    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
    gint   pid = (gint)getpid();



    int rc = sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup_printf("DB transaction error: %s", sqlite3_errmsg(g_db));
        goto done;
    }













    const gchar *check_sql =
        "SELECT op_type, pid, locked_at FROM vm_locks WHERE vm_id = ?;";
    rc = sqlite3_prepare_v2(g_db, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup("DB prepare error");
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }





    sqlite3_bind_text(stmt, 1, vm_id, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {

        gint existing_op  = sqlite3_column_int(stmt, 0);
        gint existing_pid = sqlite3_column_int(stmt, 1);
        gint64 locked_at_val = sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);
        stmt = NULL;




        gint ttl = _lock_expiry_for_op(existing_op);
        gboolean lock_expired = (locked_at_val > 0 && (now - locked_at_val) >= ttl);



        if (pid_is_alive(existing_pid) && !lock_expired) {








            static const gchar *op_names[] = {
                "NONE", "STARTING", "STOPPING", "DELETING",
                "CREATING", "TUNING", "SNAPSHOT", "MIGRATING"
            };
            const gchar *op_name = (existing_op >= 0 && existing_op <= 7)
                                   ? op_names[existing_op] : "UNKNOWN";
            if (err_msg)
                *err_msg = g_strdup_printf(
                    "VM '%s' is already locked (op: %s, pid: %d)", vm_id, op_name, existing_pid);
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            goto done;
        } else {








            if (lock_expired)
                g_warning("[vm_state] Overwriting expired lock for VM '%s' (age %ld sec, PID %d)",
                          vm_id, (long)(now - locked_at_val), existing_pid);
            else
                g_warning("[vm_state] Overwriting stale lock for VM '%s' (dead PID %d)",
                          vm_id, existing_pid);
            sqlite3_stmt *del_stmt = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del_stmt, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, vm_id, -1, SQLITE_STATIC);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
            }
        }
    } else {
        sqlite3_finalize(stmt);
        stmt = NULL;
    }


    const gchar *insert_sql =
        "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES (?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup("DB insert prepare error");
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }


    sqlite3_bind_text (stmt, 1, vm_id,   -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 2, op_type);
    sqlite3_bind_int  (stmt, 3, pid);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {

        if (err_msg)
            *err_msg = g_strdup_printf("DB insert failed: %s", sqlite3_errmsg(g_db));
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }


    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    result = TRUE;

done:


    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_db_mutex);
    return result;
}




















void unlock_vm_operation(const gchar *vm_id) {
    if (!g_db || !vm_id) return;

    g_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const gchar *sql = "DELETE FROM vm_locks WHERE vm_id = ?;";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, vm_id, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        log_sqlite_error("unlock DELETE", rc);
        sqlite3_finalize(stmt);
    }

    g_mutex_unlock(&g_db_mutex);
}
















gint pcv_vm_state_get_lock_count(void) {
    if (!g_db) return 0;

    g_mutex_lock(&g_db_mutex);
    gint count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT COUNT(*) FROM vm_locks;", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_db_mutex);
    return count;
}























gint pcv_vm_state_cleanup_expired(void) {
    if (!g_db) return 0;

    g_mutex_lock(&g_db_mutex);




    gint64 cutoff = (gint64)g_get_real_time() / G_USEC_PER_SEC - LOCK_EXPIRY_SEC;
    gint cleaned = 0;


    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT vm_id, op_type, pid, locked_at FROM vm_locks WHERE locked_at < ?;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        GPtrArray *expired_ids = g_ptr_array_new_with_free_func(g_free);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *vm_id = (const gchar *)sqlite3_column_text(stmt, 0);
            gint op_type = sqlite3_column_int(stmt, 1);
            gint pid = sqlite3_column_int(stmt, 2);
            gint64 locked_at = sqlite3_column_int64(stmt, 3);
            gint64 now_sec = (gint64)g_get_real_time() / G_USEC_PER_SEC;
            g_warning("[vm_state] Expired lock: VM '%s' op=%d pid=%d age=%ld sec",
                      vm_id, op_type, pid, (long)(now_sec - locked_at));
            g_ptr_array_add(expired_ids, g_strdup(vm_id));
        }
        sqlite3_finalize(stmt);

        for (guint i = 0; i < expired_ids->len; i++) {
            const gchar *eid = g_ptr_array_index(expired_ids, i);
            sqlite3_stmt *del = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del, 1, eid, -1, SQLITE_STATIC);
                sqlite3_step(del);
                sqlite3_finalize(del);
                cleaned++;
            }
        }
        if (cleaned > 0)
            g_message("[vm_state] Cleanup: %d expired lock(s) removed.", cleaned);
        g_ptr_array_free(expired_ids, TRUE);
    }

    g_mutex_unlock(&g_db_mutex);
    return cleaned;
}













PcvVmRuntimeState
pcv_vm_state_get_runtime(const gchar *vm_name)
{
    if (!vm_name) return PCV_VM_UNKNOWN;

    extern virConnectPtr virt_conn_pool_acquire(void);
    extern void          virt_conn_pool_release(virConnectPtr);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) return PCV_VM_UNKNOWN;

    virDomainPtr dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        return PCV_VM_STOPPED;
    }

    int state = 0, reason = 0;
    virDomainGetState(dom, &state, &reason, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    switch (state) {
        case VIR_DOMAIN_RUNNING:   return PCV_VM_RUNNING;
        case VIR_DOMAIN_PAUSED:    return PCV_VM_PAUSED;
        case VIR_DOMAIN_SHUTOFF:   return PCV_VM_STOPPED;
        case VIR_DOMAIN_PMSUSPENDED: return PCV_VM_PAUSED;
        default:                   return PCV_VM_UNKNOWN;
    }
}







const gchar *
pcv_vm_state_runtime_str(PcvVmRuntimeState state)
{
    switch (state) {
        case PCV_VM_RUNNING:   return "running";
        case PCV_VM_STOPPED:   return "stopped";
        case PCV_VM_PAUSED:    return "paused";
        case PCV_VM_MIGRATING: return "migrating";
        case PCV_VM_ERROR:     return "error";
        default:               return "unknown";
    }
}






















void shutdown_pending_state_machine(void) {
    g_mutex_lock(&g_db_mutex);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        g_message("[vm_state] SQLite DB closed.");
    }
    g_mutex_unlock(&g_db_mutex);
    g_mutex_clear(&g_db_mutex);
}
