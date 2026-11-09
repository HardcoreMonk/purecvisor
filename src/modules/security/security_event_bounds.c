   
                                                                               
  
                           
                                                                       
                                                               
                                                              
  
                                                       
                                                           
                                                
  
                                                                            
                                                                                
                                                                    
                                                           
                                                                              
                                                         
                                                                                                        
                                                                                                                       
                                                                  
                                                         
                                                                
   
#include "modules/security/security_event_bounds.h"

#include "modules/audit/pcv_audit.h"
#include "modules/daemons/alert_engine.h"
#include "modules/security/security_policy.h"

#include <string.h>                                                                    

  
                                                       
                                                   
                                                         
  
                                                                         
                                                                                
                                                                     
                                                                             
                               
                                                            
                                                        
                                                              
               
                                                                  
   

                                                                      
static gint g_retention_cap = PCV_SEC_MAX_RETAINED_EVENTS;

  
                                                   
                                     
  
                                                                                
                                                               
                                                               
                                 
                                                                 
   
void
pcv_security_store_set_retention_cap_for_test(gint cap)
{
                                                
    g_retention_cap = (cap > 0) ? cap : PCV_SEC_MAX_RETAINED_EVENTS;
}

static GQuark
bounds_error_quark(void)
{
    return g_quark_from_static_string("pcv-security-event-bounds");
}

  
                                                                                        
                                                            
   
static gboolean
alter_ignore_duplicate(sqlite3 *db, const gchar *sql, GError **error)
{
    gchar *aerr = NULL;
    gint arc = sqlite3_exec(db, sql, NULL, NULL, &aerr);
    if (arc == SQLITE_OK) {
        return TRUE;
    }
                                                                     
    gboolean dup = (aerr && strstr(aerr, "duplicate column") != NULL);
    if (!dup) {
                                                   
        if (error) {
            g_set_error(error, bounds_error_quark(), arc,
                        "%s", aerr ? aerr : sqlite3_errmsg(db));
        }
        sqlite3_free(aerr);
        return FALSE;
    }
    sqlite3_free(aerr);                                     
    return TRUE;
}

  
                                                          
                                                           
                                                        
                     
  
                                                                
                                                            
                                                   
                                                              
                    
                                                                         
   
gboolean
pcv_sec_bounds_migrate(sqlite3 *db, GError **error)
{
    static const gchar *const alters[] = {
        "ALTER TABLE overlay_wg_keys ADD COLUMN slot INTEGER NOT NULL DEFAULT -1",
        "ALTER TABLE security_events ADD COLUMN repeat_summary_at INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE security_events ADD COLUMN repeat_summary_count INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE security_events ADD COLUMN closed_reason TEXT NOT NULL DEFAULT ''",
                                                                                
        "ALTER TABLE security_events ADD COLUMN closed_by TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE security_events ADD COLUMN closed_at INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE security_events ADD COLUMN close_note TEXT NOT NULL DEFAULT ''",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(alters); i++) {
        if (!alter_ignore_duplicate(db, alters[i], error)) {
            return FALSE;
        }
    }

    gchar *ierr = NULL;
    gint rc = sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_security_events_status_last_seen "
        "ON security_events(status, last_seen)", NULL, NULL, &ierr);
    if (rc != SQLITE_OK) {
        if (error) {
            g_set_error(error, bounds_error_quark(), rc,
                        "%s", ierr ? ierr : sqlite3_errmsg(db));
        }
        sqlite3_free(ierr);
        return FALSE;
    }
    return TRUE;
}

  
                                                                     
                                                    
                                                       
                                                                      
                                        
   
gboolean
pcv_sec_bounds_enforce_retention(sqlite3 *db, GError **error)
{
      
                                                                     
                                                         
      
                                                                                
                                                                                
                                                                                 
                                                                              
                                                                             
                                                                    
      
                                                                               
                                                         
                                                                 
                                                                     
                                                                      
                                                                    
                                                                       
       
                                                                              
                                                                      
                                                                            
    const gchar *sql =
        "DELETE FROM security_events "
        "WHERE status IN ('resolved','suppressed') AND rowid NOT IN ("
        "SELECT rowid FROM security_events "
        "WHERE status IN ('resolved','suppressed') "
        "ORDER BY MAX(timestamp, closed_at) DESC, rowid DESC LIMIT ?1)";
    sqlite3_stmt *stmt = NULL;
    gint rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("security_store: prepare retention prune failed: %s",
                  sqlite3_errmsg(db));
        g_set_error(error, bounds_error_quark(), rc, "prepare retention prune: %s",
                    sqlite3_errmsg(db));
        return FALSE;
    }
    sqlite3_bind_int(stmt, 1, g_retention_cap);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        g_warning("security_store: execute retention prune failed: %s",
                  sqlite3_errmsg(db));
        g_set_error(error, bounds_error_quark(), rc, "execute retention prune: %s",
                    sqlite3_errmsg(db));
        return FALSE;
    }
    return TRUE;
}

  
                                                                             
                                                                   
                                                                             
                                                             
                                                                   
                                                            
                                         
  
                                                                          
                                                       
   
typedef struct {
    gint64 window_start;                              
    guint used;                                               
    guint used_warn;                                      
    guint over;                                                                 
} SecBudgetSlot;

static SecBudgetSlot g_budget[PCV_SEC_SOURCE_SLOTS];

  
                                                           
                                                                     
   
static guint
budget_roll(SecBudgetSlot *b, gint64 now, gint64 *since_out)
{
    if (b->window_start == 0
        || (now >= b->window_start && now - b->window_start < PCV_SEC_AUDIT_BUDGET_WINDOW_SEC)) {
        return 0;                                                         
    }
    guint over = b->over;
    *since_out = b->window_start;
    *b = (SecBudgetSlot){0};
    return over;
}

  
                                                                            
                                                           
                                                      
                                                          
                                                                
                                                                       
   
gboolean
pcv_sec_bounds_audit_take(gint source, gint severity, gboolean is_new, gint64 now)
{
    if (source < 0 || source >= PCV_SEC_SOURCE_SLOTS) {
        return FALSE;
    }
    SecBudgetSlot *b = &g_budget[source];
    gint64 since = 0;
    (void)budget_roll(b, now, &since);                                              
    if (b->window_start == 0) {
        b->window_start = now;
    }
    gboolean crit = (severity == PCV_SECURITY_SEVERITY_CRIT);
    if (b->used < PCV_SEC_AUDIT_BUDGET_PER_HOUR
        && (crit || b->used_warn < PCV_SEC_AUDIT_BUDGET_WARN_MAX)) {
        b->used++;
        if (!crit) {
            b->used_warn++;
        }
        return TRUE;
    }
    if (is_new) {
        b->over++;
    }
    return FALSE;
}

                                                           
static void
flood_poll(gint64 now, PcvSecInsertResult *out)
{
    for (gint s = 0; s < PCV_SEC_SOURCE_SLOTS; s++) {
        gint64 since = 0;
        guint over = budget_roll(&g_budget[s], now, &since);
        if (over > 0) {
            out->flood_count[s] = over;
            out->flood_since[s] = since;
        }
    }
}

                                                                  
static gboolean
db_fail(sqlite3 *db, GError **error, gint rc, const gchar *what)
{
    g_warning("security_event_bounds: %s failed: %s", what, sqlite3_errmsg(db));
    g_set_error(error, bounds_error_quark(), rc, "%s: %s", what, sqlite3_errmsg(db));
    return FALSE;
}

                                                            
static gboolean
set_summary_base(sqlite3 *db, const gchar *event_id, gint64 at, gint64 count, GError **error)
{
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "UPDATE security_events SET repeat_summary_at=?, repeat_summary_count=? WHERE event_id=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return db_fail(db, error, rc, "prepare summary base");
    }
    sqlite3_bind_int64(st, 1, at);
    sqlite3_bind_int64(st, 2, count);
    sqlite3_bind_text(st, 3, event_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? TRUE : db_fail(db, error, rc, "update summary base");
}

  
                                                                 
                                                            
                                                              
                                                                    
                                                                  
   
static gboolean
judge_repeat(sqlite3 *db, const PcvSecurityEvent *ev, gint64 now, PcvSecInsertResult *out,
             GError **error)
{
    g_autofree gchar *key = pcv_security_event_coalesce_key(ev);
                                                                           
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT event_id, occurrence_count, repeat_summary_at, repeat_summary_count, created_at "
        "FROM security_events WHERE coalesce_key=? AND status IN ('open','action_pending')",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return db_fail(db, error, rc, "prepare coalesced row read");
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    gint64 occ = 0, at = 0, cnt = 0, created = 0;
    if (rc == SQLITE_ROW) {
        const guchar *id = sqlite3_column_text(st, 0);
        g_strlcpy(out->row_event_id, id ? (const gchar *)id : "", sizeof out->row_event_id);
        occ = sqlite3_column_int64(st, 1);
        at = sqlite3_column_int64(st, 2);
        cnt = sqlite3_column_int64(st, 3);
        created = sqlite3_column_int64(st, 4);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
                                                                  
        return rc == SQLITE_DONE ? TRUE : db_fail(db, error, rc, "read coalesced row");
    }

                                                               
    if (!pcv_security_policy_should_audit(ev)) {
        return TRUE;
    }
    if (at == 0 && cnt == 0) {
        at = created;
        cnt = 1;
    }
    if (now < at) {
        return set_summary_base(db, out->row_event_id, now, cnt, error);                    
    }
    if (now - at < PCV_SEC_REPEAT_SUMMARY_SEC || occ - cnt < 1) {
        return TRUE;
    }
                                                                  
    if (!pcv_sec_bounds_audit_take((gint)ev->source, (gint)ev->severity, FALSE, now)) {
        return TRUE;
    }
    if (!set_summary_base(db, out->row_event_id, now, occ, error)) {
        return FALSE;                                                                   
    }
    out->repeat_due = TRUE;
    out->repeat_count = occ - cnt;
    return TRUE;
}

  
                                                                             
                                                     
                                                                             
                                                              
                                                                 
                                                          
                                                 
  
                                                                                  
                                                                                                
                                                                           
                                                                      
                                                                       
                                                                                     
                                                                          
                              
   
#define LIVE_PENDING_ACTION \
    "EXISTS (SELECT 1 FROM security_actions a " \
    "WHERE a.event_id=security_events.event_id AND a.status='pending' " \
    "AND (a.ttl_sec<=0 OR a.expires_at>?1))"
#define FOLDABLE_WHERE "status='open' AND severity IN (?2,?3) AND NOT " LIVE_PENDING_ACTION

static gint g_cap_for_test = 0;                                                
static gint g_batch_for_test = 0;                                            
static gint64 g_expire_last = 0;                                                        
static gboolean g_protected_over = FALSE;                                        

static gint
open_cap(void)
{
    return g_cap_for_test > 0 ? g_cap_for_test : PCV_SEC_OPEN_FOLDABLE_CAP;
}

static void
bind_foldable(sqlite3_stmt *st, gint64 now)
{
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_text(st, 2, pcv_security_severity_to_string(PCV_SECURITY_SEVERITY_INFO), -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(st, 3, pcv_security_severity_to_string(PCV_SECURITY_SEVERITY_WARN), -1,
                      SQLITE_STATIC);
}

  
                                                                         
                                                                              
   
static gint
suppress_oldest(sqlite3 *db, gint64 now, const gchar *reason, gint64 before, GError **error)
{
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "UPDATE security_events SET status='suppressed', closed_reason=?4 WHERE rowid IN ("
        "SELECT rowid FROM security_events WHERE " FOLDABLE_WHERE " AND last_seen<?5 "
        "ORDER BY last_seen ASC, rowid ASC LIMIT ?6)", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        (void)db_fail(db, error, rc, "prepare fold");
        return -1;
    }
    bind_foldable(st, now);
    sqlite3_bind_text(st, 4, reason, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, before);
    sqlite3_bind_int(st, 6, g_batch_for_test > 0 ? g_batch_for_test : PCV_SEC_BOUNDS_BATCH);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        (void)db_fail(db, error, rc, "execute fold");
        return -1;
    }
    return sqlite3_changes(db);
}

  
                                                                          
                                                                      
                                                                                          
   
static gint
pcv_sec_bounds_fold_overflow(sqlite3 *db, gint64 now, gint64 *oldest_last_seen_out, GError **error)
{
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*), MIN(last_seen) FROM security_events WHERE " FOLDABLE_WHERE,
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        (void)db_fail(db, error, rc, "prepare foldable count");
        return -1;
    }
    bind_foldable(st, now);
    rc = sqlite3_step(st);
    gint64 count = 0, oldest = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(st, 0);
        oldest = sqlite3_column_int64(st, 1);                                  
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        (void)db_fail(db, error, rc, "read foldable count");
        return -1;
    }
    if (count <= open_cap()) {
        return 0;
    }
    gint folded = suppress_oldest(db, now, "overflow", G_MAXINT64, error);
    if (folded > 0) {
        *oldest_last_seen_out = oldest;
    }
    return folded;
}

                                                             
static gint
pcv_sec_bounds_expire(sqlite3 *db, gint64 now, GError **error)
{
    return suppress_oldest(db, now, "expired", now - PCV_SEC_OPEN_EXPIRE_SEC, error);
}

  
                                                                   
                                                               
   
static gboolean
expire_tick(sqlite3 *db, gint64 now, PcvSecInsertResult *out, GError **error)
{
    if (now < g_expire_last) {
        g_expire_last = now;
        return TRUE;
    }
    if (now - g_expire_last < PCV_SEC_EXPIRE_CHECK_SEC) {
        return TRUE;
    }
    g_expire_last = now;
    gint expired = pcv_sec_bounds_expire(db, now, error);
    if (expired < 0) {
        return FALSE;
    }
    out->expired = expired;
    out->expired_before = now - PCV_SEC_OPEN_EXPIRE_SEC;
    return TRUE;
}

                                             
static gboolean
judge_new(sqlite3 *db, const PcvSecurityEvent *ev, gint64 now, PcvSecInsertResult *out,
          GError **error)
{
    g_strlcpy(out->row_event_id, ev->event_id, sizeof out->row_event_id);
    if (pcv_security_policy_should_audit(ev)) {
        out->audit_new = pcv_sec_bounds_audit_take((gint)ev->source, (gint)ev->severity, TRUE, now);
    }
    gint folded = pcv_sec_bounds_fold_overflow(db, now, &out->folded_oldest_last_seen, error);
    if (folded < 0) {
        return FALSE;
    }
    out->folded = folded;
    return TRUE;
}

  
                                                                     
                                                        
                                                                  
                                                                  
                                           
                                                                         
   
gboolean
pcv_sec_bounds_apply(sqlite3 *db, const PcvSecurityEvent *ev, PcvSecInsertKind kind,
                     gint64 now, PcvSecInsertResult *out, GError **error)
{
    out->kind = kind;
    flood_poll(now, out);
    gboolean ok = (kind == PCV_SEC_INSERT_COALESCED)
                  ? judge_repeat(db, ev, now, out, error)
                  : judge_new(db, ev, now, out, error);
                                                                 
    if (!expire_tick(db, now, out, ok ? error : NULL)) {
        ok = FALSE;
    }
    if (out->folded > 0 || out->expired > 0) {
                                                                     
        (void)pcv_sec_bounds_enforce_retention(db, NULL);
    }
    return ok;
}

  
                                                                                
                                                         
                                                        
                        
                                                     
                                                                        
                             
   
gboolean
pcv_sec_bounds_protected_over_cap(sqlite3 *db, gint64 now, gint *count_out)
{
    *count_out = 0;
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM security_events WHERE status='action_pending' OR (status='open' "
        "AND (severity=?1 OR EXISTS (SELECT 1 FROM security_actions a "
        "WHERE a.event_id=security_events.event_id AND a.status='pending' "
        "AND (a.ttl_sec<=0 OR a.expires_at>?2))))", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return db_fail(db, NULL, rc, "prepare protected count");
    }
    sqlite3_bind_text(st, 1, pcv_security_severity_to_string(PCV_SECURITY_SEVERITY_CRIT), -1,
                      SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *count_out = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        return db_fail(db, NULL, rc, "read protected count");
    }
    gboolean over = *count_out > open_cap();
    if (over != g_protected_over) {
        g_protected_over = over;
        g_warning("security_store: protected open events (crit/pending action) %s cap: %d / %d%s",
                  over ? "over" : "back under", *count_out, open_cap(),
                  over ? " — not folded, event DB may keep growing" : "");
    }
    return over;
}

  
                                                                             
                                                             
                                                                             
                                                             
                                                         
                                         
   

                                                                                     
static gboolean
close_note_blanked(gunichar c)
{
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F)
           || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

  
                                                                                   
                                                                  
                                                                                   
                                                                           
   
gchar *
pcv_sec_bounds_close_note_sanitize(const gchar *raw)
{
    if (!raw) {
        return NULL;
    }
    g_autofree gchar *valid = g_utf8_make_valid(raw, -1);
    GString *out = g_string_sized_new(strlen(valid));
    for (const gchar *p = valid; *p; p = g_utf8_next_char(p)) {
        if (close_note_blanked(g_utf8_get_char(p))) {
            g_string_append_c(out, ' ');
        } else {
            g_string_append_len(out, p, g_utf8_next_char(p) - p);
        }
    }
    gchar *note = g_strstrip(g_string_free(out, FALSE));                         
    glong chars = g_utf8_strlen(note, -1);
    if (chars < 1 || chars > PCV_SEC_CLOSE_NOTE_MAX) {
        g_free(note);
        return NULL;
    }
    return note;
}

                                                                  
static PcvSecCloseResult
close_miss_cause(sqlite3 *db, const gchar *event_id, gint64 now, GError **error)
{
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "SELECT status, " LIVE_PENDING_ACTION " FROM security_events WHERE event_id=?2", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        (void)db_fail(db, error, rc, "prepare close cause");
        return PCV_SEC_CLOSE_ERROR;
    }
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_text(st, 2, event_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    PcvSecCloseResult cause = PCV_SEC_CLOSE_NOT_FOUND;
    if (rc == SQLITE_ROW) {
        const gchar *status = (const gchar *)sqlite3_column_text(st, 0);
        gboolean live = sqlite3_column_int(st, 1) != 0;
                                                                                         
        cause = (g_strcmp0(status, "action_pending") == 0 || (g_strcmp0(status, "open") == 0 && live))
                ? PCV_SEC_CLOSE_PENDING : PCV_SEC_CLOSE_NOT_OPEN;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        (void)db_fail(db, error, rc, "read close cause");
        return PCV_SEC_CLOSE_ERROR;
    }
    return cause;
}

  
                                                                   
                                                           
                                  
                                                                 
                                                          
                                                                    
   
PcvSecCloseResult
pcv_sec_bounds_close_event(sqlite3 *db, const gchar *event_id, const gchar *actor,
                           const gchar *reason, gint64 now, GError **error)
{
    if (!db || !event_id || !*event_id || !actor || !*actor) {
        g_set_error(error, bounds_error_quark(), SQLITE_MISUSE, "%s",
                    db ? "event_id and actor are required" : "security store is not open");
        return PCV_SEC_CLOSE_ERROR;
    }
    g_autofree gchar *note = pcv_sec_bounds_close_note_sanitize(reason);
    if (!note) {
        return PCV_SEC_CLOSE_INVALID_REASON;
    }
    sqlite3_stmt *st = NULL;
    gint rc = sqlite3_prepare_v2(db,
        "UPDATE security_events SET status='resolved', closed_reason='operator', closed_by=?3, "
        "closed_at=?1, close_note=?4 WHERE event_id=?2 AND status='open' AND NOT " LIVE_PENDING_ACTION,
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        (void)db_fail(db, error, rc, "prepare close");
        return PCV_SEC_CLOSE_ERROR;
    }
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_text(st, 2, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, note, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        (void)db_fail(db, error, rc, "execute close");
        return PCV_SEC_CLOSE_ERROR;
    }
    return sqlite3_changes(db) > 0 ? PCV_SEC_CLOSE_OK : close_miss_cause(db, event_id, now, error);
}

                                                     
static void
audit_system(const gchar *op, const gchar *target)
{
    pcv_audit_log(PCV_AUDIT_ACTOR_SYSTEM, op, target, "ok", 0, 0, "local");
}

  
                                                          
                                                         
                                                 
   
void
pcv_sec_bounds_emit(const PcvSecurityEvent *ev, const PcvSecInsertResult *r)
{
    for (gint s = 0; s < PCV_SEC_SOURCE_SLOTS; s++) {
        if (r->flood_count[s] == 0) {
            continue;
        }
        g_autofree gchar *t = g_strdup_printf("%s#n=%u,since=%" G_GINT64_FORMAT,
                                              pcv_security_source_to_string((PcvSecuritySource)s),
                                              r->flood_count[s], r->flood_since[s]);
        audit_system("security.event.flood", t);
    }
    if (r->audit_new) {
          
                                                                 
          
                                                                                 
                                                                           
                                                                     
                                                                     
           
        audit_system("security.event", r->row_event_id);
        pcv_alert_record_security_event(r->row_event_id,
                                        pcv_security_severity_to_string(ev->severity), ev->summary);
    }
    if (r->repeat_due) {
        g_autofree gchar *t = g_strdup_printf("%s#n=%" G_GINT64_FORMAT,
                                              r->row_event_id, r->repeat_count);
        audit_system("security.event.repeat", t);
    }
    if (r->folded > 0) {                                     
        g_autofree gchar *t = g_strdup_printf("security_events#folded=%d,oldest=%" G_GINT64_FORMAT,
                                              r->folded, r->folded_oldest_last_seen);
        audit_system("security.event.overflow", t);
    }
    if (r->expired > 0) {
        g_autofree gchar *t = g_strdup_printf("security_events#expired=%d,before=%" G_GINT64_FORMAT,
                                              r->expired, r->expired_before);
        audit_system("security.event.expire", t);
    }
}

  
                                                                                    
                                                    
                                                            
                                 
   
void
pcv_sec_bounds_reset_for_test(gint64 now)
{
    memset(g_budget, 0, sizeof g_budget);
    g_cap_for_test = 0;
    g_batch_for_test = 0;
    g_expire_last = now;
    g_protected_over = FALSE;
}

  
                                                                      
                                                         
                                                                                
   
void
pcv_sec_bounds_set_caps_for_test(gint cap, gint batch)
{
    g_cap_for_test = cap > 0 ? cap : 0;
    g_batch_for_test = batch > 0 ? batch : 0;
}
