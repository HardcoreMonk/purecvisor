   
                          
                                       
  
      
                                                   
                                                  
                                               
                                                  
  
                           
                                                                          
                                                                    
                                                            
                                                            
                                                                   
                                                                      
                           
  
                       
                                                       
                                                           
                                                       
                                                           
                                                                    
   

#include "pcv_audit_chain.h"

#include <openssl/evp.h>
#include <string.h>

G_DEFINE_QUARK(pcv-audit-chain-error-quark, pcv_audit_chain_error)

static void
_set_sqlite_error(GError **error, sqlite3 *db, const gchar *context, gint rc)
{
    g_set_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_SQLITE,
                "%s: sqlite rc=%d: %s", context, rc,
                db ? sqlite3_errmsg(db) : "database unavailable");
}

static gboolean
_exec(sqlite3 *db, const gchar *sql, const gchar *context, GError **error)
{
    gchar *message = NULL;
    gint rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK)
        return TRUE;
    g_set_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_SQLITE,
                "%s: sqlite rc=%d: %s", context, rc,
                message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return FALSE;
}

static void
_rollback(sqlite3 *db)
{
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

static gboolean
_has_column(sqlite3 *db, const gchar *column)
{
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(audit_log)", -1,
                           &stmt, NULL) != SQLITE_OK)
        return FALSE;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *name = (const gchar *)sqlite3_column_text(stmt, 1);
        if (g_strcmp0(name, column) == 0) {
            found = TRUE;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

   
                               
                                       
                     
                 
                    
                 
                 
                                        
                                         
  
                           
                                                                     
                                      
                                                                  
                                       
                                                                      
                                                                      
                                      
                                                           
  
                       
                                                  
                                                  
                                                                
   
gchar *
pcv_audit_chain_record_hash(const gchar *prev_hash, const gchar *ts,
                            const gchar *username, const gchar *method,
                            const gchar *target, const gchar *result,
                            gint error_code, GError **error)
{
    gchar *preimage = g_strdup_printf("%s|%s|%s|%s|%s|%s|%d",
                                      prev_hash ? prev_hash : "",
                                      ts ? ts : "",
                                      username ? username : "",
                                      method ? method : "",
                                      target ? target : "",
                                      result ? result : "",
                                      error_code);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        g_free(preimage);
        g_set_error_literal(error, PCV_AUDIT_CHAIN_ERROR,
                            PCV_AUDIT_CHAIN_ERROR_CRYPTO,
                            "SHA-256 context allocation failed");
        return NULL;
    }

    guchar digest[EVP_MAX_MD_SIZE];
    guint digest_len = 0;
    gboolean ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
                  EVP_DigestUpdate(ctx, preimage, strlen(preimage)) == 1 &&
                  EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    g_free(preimage);
    if (!ok) {
        g_set_error_literal(error, PCV_AUDIT_CHAIN_ERROR,
                            PCV_AUDIT_CHAIN_ERROR_CRYPTO,
                            "SHA-256 calculation failed");
        return NULL;
    }

    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);
    return g_string_free(hex, FALSE);
}

                  
                                                              
                                                         
                                                                  
  
                 
                                               
                                           
   
static gboolean
_checkpoint_anchor(sqlite3 *db, gint64 epoch_id, const gchar *fallback,
                   gint64 *start_id, gchar **expected_prev, GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT first_retained_id, expected_prev_hash "
        "FROM audit_chain_checkpoint WHERE epoch_id=? "
        "ORDER BY first_retained_id DESC LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare checkpoint anchor", rc);
        return FALSE;
    }
    sqlite3_bind_int64(stmt, 1, epoch_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *start_id = sqlite3_column_int64(stmt, 0);
        *expected_prev = g_strdup(
            (const gchar *)sqlite3_column_text(stmt, 1));
    } else if (rc == SQLITE_DONE) {
        *start_id = 0;
        *expected_prev = g_strdup(fallback ? fallback : PCV_AUDIT_CHAIN_GENESIS);
    } else {
        _set_sqlite_error(error, db, "read checkpoint anchor", rc);
        sqlite3_finalize(stmt);
        return FALSE;
    }
    sqlite3_finalize(stmt);
    return TRUE;
}

                  
                                          
                                        
                                          
                                                                    
                                                                
                                                              
                                                  
  
                 
                                                
                                                     
   
static gboolean
_verify_segment(sqlite3 *db, gint64 epoch_id, const gchar *baseline,
                gint64 *break_rowid, GError **error)
{
    gint64 start_id = 0;
    gchar *expected_prev = NULL;
    if (!_checkpoint_anchor(db, epoch_id, baseline, &start_id,
                            &expected_prev, error))
        return FALSE;

    const gchar *sql = epoch_id == 0
        ? "SELECT id,ts,username,method,target,result,error_code,prev_hash,rec_hash "
          "FROM audit_log WHERE chain_epoch IS NULL AND rec_hash IS NOT NULL "
          "AND id>=? AND id<=COALESCE((SELECT predecessor_last_id "
          "FROM audit_chain_epoch ORDER BY epoch_id ASC LIMIT 1),9223372036854775807) "
          "ORDER BY id ASC"
        : "SELECT id,ts,username,method,target,result,error_code,prev_hash,rec_hash "
          "FROM audit_log WHERE chain_epoch=? "
          "AND id>=? ORDER BY id ASC";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare chain verification", rc);
        g_free(expected_prev);
        return FALSE;
    }
    gint bind = 1;
    if (epoch_id != 0)
        sqlite3_bind_int64(stmt, bind++, epoch_id);
    sqlite3_bind_int64(stmt, bind, start_id);

    gboolean valid = TRUE;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        gint64 rowid = sqlite3_column_int64(stmt, 0);
        const gchar *prev = (const gchar *)sqlite3_column_text(stmt, 7);
        const gchar *stored = (const gchar *)sqlite3_column_text(stmt, 8);
        if (g_strcmp0(prev ? prev : "", expected_prev) != 0) {
            if (break_rowid) *break_rowid = rowid;
            valid = FALSE;
            break;
        }
        GError *hash_error = NULL;
        gchar *computed = pcv_audit_chain_record_hash(
            prev, (const gchar *)sqlite3_column_text(stmt, 1),
            (const gchar *)sqlite3_column_text(stmt, 2),
            (const gchar *)sqlite3_column_text(stmt, 3),
            (const gchar *)sqlite3_column_text(stmt, 4),
            (const gchar *)sqlite3_column_text(stmt, 5),
            sqlite3_column_int(stmt, 6), &hash_error);
        if (!computed) {
            g_propagate_error(error, hash_error);
            valid = FALSE;
            break;
        }
        if (g_strcmp0(computed, stored) != 0) {
            if (break_rowid) *break_rowid = rowid;
            g_free(computed);
            valid = FALSE;
            break;
        }
        g_free(expected_prev);
        expected_prev = computed;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW && valid) {
        _set_sqlite_error(error, db, "step chain verification", rc);
        valid = FALSE;
    }
    sqlite3_finalize(stmt);
    g_free(expected_prev);
    return valid;
}

                                                       
                                                     
                                                                   
static gboolean
_active_epoch(sqlite3 *db, gint64 *epoch_id, gchar **baseline,
              GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT epoch_id, baseline_prev_hash FROM audit_chain_epoch "
        "WHERE closed_at IS NULL ORDER BY epoch_id DESC", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare active epoch", rc);
        return FALSE;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            _set_sqlite_error(error, db, "read active epoch", rc);
            return FALSE;
        }
        g_set_error_literal(error, PCV_AUDIT_CHAIN_ERROR,
                            PCV_AUDIT_CHAIN_ERROR_STATE,
                            "active audit chain epoch is missing");
        return FALSE;
    }
    *epoch_id = sqlite3_column_int64(stmt, 0);
    if (baseline)
        *baseline = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (baseline) g_clear_pointer(baseline, g_free);
        g_set_error_literal(error, PCV_AUDIT_CHAIN_ERROR,
                            PCV_AUDIT_CHAIN_ERROR_STATE,
                            "multiple active audit chain epochs");
        return FALSE;
    }
    sqlite3_finalize(stmt);
    return TRUE;
}

   
                          
                                               
                                    
                                 
  
                                                                  
                                                              
                                                            
  
                           
                                                                   
                                                                 
                
                                                                            
                                                                  
                                                               
                                                          
                                                     
  
                       
                                               
                                                 
                                                          
   
gboolean
pcv_audit_chain_verify(sqlite3 *db, PcvAuditChainHealth *health,
                       GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);
    PcvAuditChainHealth local = {
        .current_ok = FALSE,
        .historical_break = FALSE,
        .active_epoch = 0,
        .first_break_rowid = 0,
    };
    g_strlcpy(local.reason, "verify_failed", sizeof(local.reason));

    gint64 epoch_id = 0;
    gchar *baseline = NULL;
    if (!_active_epoch(db, &epoch_id, &baseline, error))
        return FALSE;
    local.active_epoch = epoch_id;

                                                               
                                                          
                                                      
    sqlite3_stmt *orphan = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT MIN(id) FROM audit_log WHERE "
        "(chain_epoch IS NULL AND id>COALESCE((SELECT predecessor_last_id "
        "FROM audit_chain_epoch ORDER BY epoch_id ASC LIMIT 1),0)) OR "
        "(chain_epoch IS NOT NULL AND chain_epoch NOT IN "
        "(SELECT epoch_id FROM audit_chain_epoch))",
        -1, &orphan, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare audit epoch ownership check", rc);
        g_free(baseline);
        return FALSE;
    }
    rc = sqlite3_step(orphan);
    if (rc == SQLITE_ROW && sqlite3_column_type(orphan, 0) != SQLITE_NULL) {
        local.first_break_rowid = sqlite3_column_int64(orphan, 0);
        g_strlcpy(local.reason, "epoch_orphan", sizeof(local.reason));
        local.last_verified_at = g_get_real_time() / G_USEC_PER_SEC;
        sqlite3_finalize(orphan);
        g_free(baseline);
        if (health) *health = local;
        return FALSE;
    }
    sqlite3_finalize(orphan);
    if (rc != SQLITE_ROW) {
        _set_sqlite_error(error, db, "read audit epoch ownership", rc);
        g_free(baseline);
        return FALSE;
    }

    sqlite3_stmt *marker = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT MIN(predecessor_break_id) FROM audit_chain_epoch "
        "WHERE predecessor_status='broken'", -1, &marker, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare historical break marker", rc);
        g_free(baseline);
        return FALSE;
    }
    rc = sqlite3_step(marker);
    if (rc == SQLITE_ROW && sqlite3_column_type(marker, 0) != SQLITE_NULL) {
        local.historical_break = TRUE;
        local.first_break_rowid = sqlite3_column_int64(marker, 0);
    }
    sqlite3_finalize(marker);
    if (rc != SQLITE_ROW) {
        _set_sqlite_error(error, db, "read historical break marker", rc);
        g_free(baseline);
        return FALSE;
    }

    gint64 legacy_break = 0;
    GError *legacy_error = NULL;
    if (!_verify_segment(db, 0, PCV_AUDIT_CHAIN_GENESIS,
                         &legacy_break, &legacy_error)) {
        if (legacy_error) {
            g_propagate_error(error, legacy_error);
            g_free(baseline);
            return FALSE;
        }
        local.historical_break = TRUE;
        if (local.first_break_rowid == 0 ||
            (legacy_break > 0 && legacy_break < local.first_break_rowid))
            local.first_break_rowid = legacy_break;
    }

    gint64 active_break = 0;
    gboolean active_ok = _verify_segment(db, epoch_id, baseline,
                                         &active_break, error);
    g_free(baseline);
    if (!active_ok && error && *error)
        return FALSE;
    local.current_ok = active_ok;
    if (!active_ok) {
        local.first_break_rowid = active_break;
        g_strlcpy(local.reason, "verify_failed", sizeof(local.reason));
    } else if (local.historical_break) {
        g_strlcpy(local.reason, "legacy_break", sizeof(local.reason));
    } else {
        g_strlcpy(local.reason, "ok", sizeof(local.reason));
    }
    local.last_verified_at = g_get_real_time() / G_USEC_PER_SEC;
    if (health) *health = local;
    return active_ok;
}

                                                                   
                                                                
                                                         
                                              
static gboolean
_create_schema(sqlite3 *db, GError **error)
{
    if (!_exec(db,
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,ts TEXT NOT NULL,event_ts TEXT,"
        "node TEXT NOT NULL,username TEXT,method TEXT NOT NULL,target TEXT,"
        "result TEXT NOT NULL,error_code INTEGER,duration_ms INTEGER,src_ip TEXT,"
        "prev_hash TEXT,rec_hash TEXT,chain_epoch INTEGER)",
        "create audit_log", error))
        return FALSE;

    struct { const gchar *name; const gchar *sql; } columns[] = {
        {"event_ts", "ALTER TABLE audit_log ADD COLUMN event_ts TEXT"},
        {"prev_hash", "ALTER TABLE audit_log ADD COLUMN prev_hash TEXT"},
        {"rec_hash", "ALTER TABLE audit_log ADD COLUMN rec_hash TEXT"},
        {"chain_epoch", "ALTER TABLE audit_log ADD COLUMN chain_epoch INTEGER"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(columns); i++) {
        if (!_has_column(db, columns[i].name) &&
            !_exec(db, columns[i].sql, "migrate audit_log column", error))
            return FALSE;
    }

    return _exec(db,
        "CREATE TABLE IF NOT EXISTS audit_chain_epoch ("
        "epoch_id INTEGER PRIMARY KEY AUTOINCREMENT,started_at TEXT NOT NULL,"
        "baseline_prev_hash TEXT NOT NULL,predecessor_last_id INTEGER,"
        "predecessor_last_hash TEXT,predecessor_status TEXT NOT NULL,"
        "predecessor_break_id INTEGER,reason TEXT NOT NULL,closed_at TEXT)",
        "create audit_chain_epoch", error) &&
        _exec(db,
        "CREATE TABLE IF NOT EXISTS audit_chain_checkpoint ("
        "epoch_id INTEGER NOT NULL,created_at TEXT NOT NULL,"
        "first_retained_id INTEGER NOT NULL,expected_prev_hash TEXT NOT NULL,"
        "reason TEXT NOT NULL,PRIMARY KEY(epoch_id,first_retained_id))",
        "create audit_chain_checkpoint", error) &&
        _exec(db,
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_audit_epoch_prev "
        "ON audit_log(chain_epoch,prev_hash) "
        "WHERE chain_epoch IS NOT NULL AND rec_hash IS NOT NULL",
        "create predecessor uniqueness index", error) &&
        _exec(db,
        "CREATE INDEX IF NOT EXISTS idx_audit_epoch_id "
        "ON audit_log(chain_epoch,id)",
        "create epoch row index", error);
}

   
                           
                                                
                          
                             
  
                                                               
                                                                   
                                                           
                                                                        
                  
  
                           
                                                                            
                                                   
                                                                   
                
                                                                        
                                                                  
       
                                                                                 
                                                                       
  
                       
                                                  
                                                    
                                                      
   
gboolean
pcv_audit_chain_prepare(sqlite3 *db, PcvAuditChainHealth *health,
                        GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);
    if (!_exec(db, "BEGIN IMMEDIATE", "begin audit schema migration", error))
        return FALSE;
    if (!_create_schema(db, error)) {
        _rollback(db);
        return FALSE;
    }

    sqlite3_stmt *count_stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM audit_chain_epoch",
                                -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare epoch count", rc);
        _rollback(db);
        return FALSE;
    }
    rc = sqlite3_step(count_stmt);
    gint64 epoch_count = rc == SQLITE_ROW ? sqlite3_column_int64(count_stmt, 0) : -1;
    sqlite3_finalize(count_stmt);
    if (epoch_count < 0) {
        _set_sqlite_error(error, db, "read epoch count", rc);
        _rollback(db);
        return FALSE;
    }

    if (epoch_count == 0) {
        gint64 break_rowid = 0;
        GError *verify_error = NULL;
        gboolean legacy_ok = _verify_segment(
            db, 0, PCV_AUDIT_CHAIN_GENESIS, &break_rowid, &verify_error);
        if (verify_error) {
            g_propagate_error(error, verify_error);
            _rollback(db);
            return FALSE;
        }

        sqlite3_stmt *boundary = NULL;
        rc = sqlite3_prepare_v2(db,
            "SELECT COALESCE(MAX(id),0) FROM audit_log WHERE chain_epoch IS NULL",
            -1, &boundary, NULL);
        if (rc != SQLITE_OK) {
            _set_sqlite_error(error, db, "prepare legacy boundary", rc);
            _rollback(db);
            return FALSE;
        }
        rc = sqlite3_step(boundary);
        gint64 legacy_last_id = rc == SQLITE_ROW
                               ? sqlite3_column_int64(boundary, 0) : -1;
        sqlite3_finalize(boundary);
        if (legacy_last_id < 0) {
            _set_sqlite_error(error, db, "read legacy boundary", rc);
            _rollback(db);
            return FALSE;
        }

        sqlite3_stmt *last = NULL;
        rc = sqlite3_prepare_v2(db,
            "SELECT id,rec_hash FROM audit_log WHERE chain_epoch IS NULL "
            "AND rec_hash IS NOT NULL ORDER BY id DESC LIMIT 1",
            -1, &last, NULL);
        if (rc != SQLITE_OK) {
            _set_sqlite_error(error, db, "prepare legacy tail", rc);
            _rollback(db);
            return FALSE;
        }
        gint64 hashed_last_id = 0;
        gchar *last_hash = NULL;
        rc = sqlite3_step(last);
        if (rc == SQLITE_ROW) {
            hashed_last_id = sqlite3_column_int64(last, 0);
            last_hash = g_strdup((const gchar *)sqlite3_column_text(last, 1));
        }
        sqlite3_finalize(last);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            _set_sqlite_error(error, db, "read legacy tail", rc);
            _rollback(db);
            return FALSE;
        }

        const gchar *status = hashed_last_id == 0 ? "genesis" :
                              legacy_ok ? "verified" : "broken";
        const gchar *baseline = legacy_ok && last_hash
                              ? last_hash : PCV_AUDIT_CHAIN_GENESIS;
        sqlite3_stmt *insert_epoch = NULL;
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO audit_chain_epoch("
            "started_at,baseline_prev_hash,predecessor_last_id,"
            "predecessor_last_hash,predecessor_status,predecessor_break_id,reason) "
            "VALUES(strftime('%Y-%m-%d %H:%M:%S','now'),?,?,?,?,?,?)",
            -1, &insert_epoch, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(insert_epoch, 1, baseline, -1, SQLITE_TRANSIENT);
            if (legacy_last_id > 0)
                sqlite3_bind_int64(insert_epoch, 2, legacy_last_id);
            else sqlite3_bind_null(insert_epoch, 2);
            if (last_hash) sqlite3_bind_text(insert_epoch, 3, last_hash, -1,
                                             SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_epoch, 3);
            sqlite3_bind_text(insert_epoch, 4, status, -1, SQLITE_STATIC);
            if (!legacy_ok) sqlite3_bind_int64(insert_epoch, 5, break_rowid);
            else sqlite3_bind_null(insert_epoch, 5);
            sqlite3_bind_text(insert_epoch, 6,
                              legacy_ok ? "legacy_migration" : "legacy_break",
                              -1, SQLITE_STATIC);
            rc = sqlite3_step(insert_epoch);
        }
        sqlite3_finalize(insert_epoch);
        g_free(last_hash);
        if (rc != SQLITE_DONE) {
            _set_sqlite_error(error, db, "create first audit epoch", rc);
            _rollback(db);
            return FALSE;
        }
    }

    if (!_exec(db, "COMMIT", "commit audit schema migration", error)) {
        _rollback(db);
        return FALSE;
    }

    PcvAuditChainHealth local = {0};
    if (!pcv_audit_chain_verify(db, &local, error)) {
        if (!error || !*error)
            g_set_error(error, PCV_AUDIT_CHAIN_ERROR,
                        PCV_AUDIT_CHAIN_ERROR_INTEGRITY,
                        "active audit epoch is broken at rowid %ld",
                        (long)local.first_break_rowid);
        if (health) *health = local;
        return FALSE;
    }
    if (health) *health = local;
    return TRUE;
}

   
                          
                                
                                   
                                          
                               
  
                                                                            
                                                                        
                                                               
  
                           
                                                                          
                                     
                                                                     
                                                                   
        
                                                                                      
                                                                          
              
                                                                            
                                                                      
                                 
  
                       
                                                 
                                                  
   
gboolean
pcv_audit_chain_append(sqlite3 *db, const PcvAuditChainRecord *record,
                       gint64 *inserted_rowid, GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);
    g_return_val_if_fail(record != NULL, FALSE);
    if (inserted_rowid) *inserted_rowid = 0;
    if (!_exec(db, "BEGIN IMMEDIATE", "begin atomic audit append", error))
        return FALSE;

    gint64 epoch_id = 0;
    gchar *baseline = NULL;
    if (!_active_epoch(db, &epoch_id, &baseline, error)) {
        _rollback(db);
        return FALSE;
    }

    gchar *prev_hash = NULL;
    sqlite3_stmt *head = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT rec_hash FROM audit_log WHERE chain_epoch=? "
        "AND rec_hash IS NOT NULL ORDER BY id DESC LIMIT 1",
        -1, &head, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(head, 1, epoch_id);
        rc = sqlite3_step(head);
        if (rc == SQLITE_ROW)
            prev_hash = g_strdup((const gchar *)sqlite3_column_text(head, 0));
    }
    sqlite3_finalize(head);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "read active audit head", rc);
        g_free(baseline);
        _rollback(db);
        return FALSE;
    }
    if (!prev_hash) {
        gint64 start_id = 0;
        if (!_checkpoint_anchor(db, epoch_id, baseline, &start_id,
                                &prev_hash, error)) {
            g_free(baseline);
            _rollback(db);
            return FALSE;
        }
    }
    g_free(baseline);

    gchar *rec_hash = pcv_audit_chain_record_hash(
        prev_hash, record->ts, record->username, record->method,
        record->target, record->result, record->error_code, error);
    if (!rec_hash) {
        g_free(prev_hash);
        _rollback(db);
        return FALSE;
    }

    sqlite3_stmt *insert = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO audit_log("
        "ts,node,username,method,target,result,error_code,duration_ms,src_ip,"
        "event_ts,prev_hash,rec_hash,chain_epoch) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &insert, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(insert, 1, record->ts, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, record->node, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, record->username, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, record->method, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 5, record->target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 6, record->result, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 7, record->error_code);
        sqlite3_bind_int64(insert, 8, record->duration_ms);
        sqlite3_bind_text(insert, 9, record->src_ip, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 10, record->event_ts, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 11, prev_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 12, rec_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert, 13, epoch_id);
        rc = sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    g_free(prev_hash);
    g_free(rec_hash);
    if (rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "insert audit chain record", rc);
        _rollback(db);
        return FALSE;
    }
    gint64 rowid = sqlite3_last_insert_rowid(db);
    if (!_exec(db, "COMMIT", "commit atomic audit append", error)) {
        _rollback(db);
        return FALSE;
    }
    if (inserted_rowid) *inserted_rowid = rowid;
    return TRUE;
}

                                                                   
                                                                 
                                                         
                                                    
static gboolean
_insert_retention_checkpoint(sqlite3 *db, gint64 epoch_id,
                             gint64 delete_max, GError **error)
{
    const gchar *remaining_sql = epoch_id == 0
        ? "SELECT id,prev_hash FROM audit_log WHERE chain_epoch IS NULL "
          "AND rec_hash IS NOT NULL AND id>? ORDER BY id ASC LIMIT 1"
        : "SELECT id,prev_hash FROM audit_log WHERE chain_epoch=? "
          "AND rec_hash IS NOT NULL AND id>? ORDER BY id ASC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, remaining_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare retained audit anchor", rc);
        return FALSE;
    }
    gint bind = 1;
    if (epoch_id != 0) sqlite3_bind_int64(stmt, bind++, epoch_id);
    sqlite3_bind_int64(stmt, bind, delete_max);
    rc = sqlite3_step(stmt);
    gint64 first_id = 0;
    gchar *expected = NULL;
    if (rc == SQLITE_ROW) {
        first_id = sqlite3_column_int64(stmt, 0);
        expected = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "read retained audit anchor", rc);
        return FALSE;
    }

    if (!expected) {
                                                                       
                                         
        gint64 active_id = 0;
        if (!_active_epoch(db, &active_id, NULL, error))
            return FALSE;
        if (epoch_id != active_id)
            return TRUE;
        sqlite3_stmt *last = NULL;
        rc = sqlite3_prepare_v2(db,
            "SELECT id,rec_hash FROM audit_log WHERE chain_epoch=? "
            "AND rec_hash IS NOT NULL AND id<=? ORDER BY id DESC LIMIT 1",
            -1, &last, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(last, 1, epoch_id);
            sqlite3_bind_int64(last, 2, delete_max);
            rc = sqlite3_step(last);
            if (rc == SQLITE_ROW) {
                first_id = sqlite3_column_int64(last, 0) + 1;
                expected = g_strdup((const gchar *)sqlite3_column_text(last, 1));
            }
        }
        sqlite3_finalize(last);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            _set_sqlite_error(error, db, "read deleted audit tail", rc);
            return FALSE;
        }
    }
    if (!expected)
        return TRUE;

    sqlite3_stmt *insert = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO audit_chain_checkpoint("
        "epoch_id,created_at,first_retained_id,expected_prev_hash,reason) "
        "VALUES(?,strftime('%Y-%m-%d %H:%M:%S','now'),?,?,'retention')",
        -1, &insert, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(insert, 1, epoch_id);
        sqlite3_bind_int64(insert, 2, first_id);
        sqlite3_bind_text(insert, 3, expected, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    g_free(expected);
    if (rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "store retention checkpoint", rc);
        return FALSE;
    }
    return TRUE;
}

   
                             
                                
                                              
                                         
                                     
  
                                                                
                                                                       
                                                            
                                                     
  
                           
                                                                             
                                                            
                                                                       
                                  
                                                                             
                                                               
                                                                           
                                                              
                          
  
                       
                                                
                                                      
                                    
   
gboolean
pcv_audit_chain_retention(sqlite3 *db, const gchar *cutoff_ts,
                          gint *deleted_rows, GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);
    g_return_val_if_fail(cutoff_ts != NULL, FALSE);
    if (deleted_rows) *deleted_rows = 0;
    if (!_exec(db, "BEGIN IMMEDIATE", "begin audit retention", error))
        return FALSE;

    gint64 active_id = 0;
    gchar *baseline = NULL;
    if (!_active_epoch(db, &active_id, &baseline, error)) {
        _rollback(db);
        return FALSE;
    }
    gint64 active_break = 0;
    if (!_verify_segment(db, active_id, baseline, &active_break, error)) {
        g_free(baseline);
        if (!error || !*error)
            g_set_error(error, PCV_AUDIT_CHAIN_ERROR,
                        PCV_AUDIT_CHAIN_ERROR_INTEGRITY,
                        "active audit epoch is broken at rowid %ld",
                        (long)active_break);
        _rollback(db);
        return FALSE;
    }
    g_free(baseline);

    sqlite3_stmt *boundary = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT MIN(id) FROM audit_log WHERE ts>=?", -1, &boundary, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(boundary, 1, cutoff_ts, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(boundary);
    }
    if (rc != SQLITE_ROW) {
        _set_sqlite_error(error, db, "read retention boundary", rc);
        sqlite3_finalize(boundary);
        _rollback(db);
        return FALSE;
    }
    gint64 first_keep = sqlite3_column_type(boundary, 0) == SQLITE_NULL
                       ? 0 : sqlite3_column_int64(boundary, 0);
    sqlite3_finalize(boundary);
    if (first_keep == 1) {
        if (!_exec(db, "COMMIT", "commit empty audit retention", error)) {
            _rollback(db);
            return FALSE;
        }
        return TRUE;
    }

    gint64 delete_max = first_keep > 1 ? first_keep - 1 : 0;
    if (first_keep == 0) {
        sqlite3_stmt *max_stmt = NULL;
        rc = sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0) FROM audit_log",
                                -1, &max_stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(max_stmt);
        if (rc == SQLITE_ROW) delete_max = sqlite3_column_int64(max_stmt, 0);
        sqlite3_finalize(max_stmt);
        if (rc != SQLITE_ROW) {
            _set_sqlite_error(error, db, "read audit retention tail", rc);
            _rollback(db);
            return FALSE;
        }
    }
    if (delete_max == 0) {
        if (!_exec(db, "COMMIT", "commit empty audit retention", error)) {
            _rollback(db);
            return FALSE;
        }
        return TRUE;
    }

    sqlite3_stmt *segments = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT COALESCE(chain_epoch,0) FROM audit_log "
        "WHERE id<=? AND rec_hash IS NOT NULL", -1, &segments, NULL);
    if (rc != SQLITE_OK) {
        _set_sqlite_error(error, db, "prepare retention segments", rc);
        _rollback(db);
        return FALSE;
    }
    sqlite3_bind_int64(segments, 1, delete_max);
    GArray *epoch_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    while ((rc = sqlite3_step(segments)) == SQLITE_ROW) {
        gint64 id = sqlite3_column_int64(segments, 0);
        g_array_append_val(epoch_ids, id);
    }
    sqlite3_finalize(segments);
    if (rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "read retention segments", rc);
        g_array_unref(epoch_ids);
        _rollback(db);
        return FALSE;
    }
    for (guint i = 0; i < epoch_ids->len; i++) {
        gint64 id = g_array_index(epoch_ids, gint64, i);
        if (!_insert_retention_checkpoint(db, id, delete_max, error)) {
            g_array_unref(epoch_ids);
            _rollback(db);
            return FALSE;
        }
    }
    g_array_unref(epoch_ids);

    sqlite3_stmt *delete_stmt = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM audit_log WHERE id<=?",
                            -1, &delete_stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(delete_stmt, 1, delete_max);
        rc = sqlite3_step(delete_stmt);
    }
    sqlite3_finalize(delete_stmt);
    if (rc != SQLITE_DONE) {
        _set_sqlite_error(error, db, "delete retained audit prefix", rc);
        _rollback(db);
        return FALSE;
    }
    gint deleted = sqlite3_changes(db);
    if (!_exec(db, "COMMIT", "commit audit retention", error)) {
        _rollback(db);
        return FALSE;
    }
    if (deleted_rows) *deleted_rows = deleted;
    return TRUE;
}
