   
                           
                                                          
                                                                 
                                                             
                                                            
                                                    
                                      
  
            
                                                                        
                                                           
                                                     
                                                              
  
               
                                                              
                                                              
                                                              
                                                        
                                                         
                                                             
  
                 
                                             
                                                      
                                                             
                                                 
                                                    
                                                         
   
#include "pcv_job_recovery.h"
#include "pcv_job_queue_internal.h"
#include "utils/pcv_log.h"
#include "api/drain.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_MAX (64u * 1024u)
#define FILES_MAX 256u
#define DISK_MAX (16u * 1024u * 1024u)
#define WAIT_MAX 128u
#define WAIT_BYTES (8u * 1024u * 1024u)
#define RETRY_MAX 32u

struct _PcvJobRef {
    gint refs;
    gchar *id, *generation, *method, *target;
    gboolean reserved, submitted;
};

typedef struct {
    PcvJobRef *ref;
    PcvJobStatus status;
    gchar *result, *error, *encoded, *name, *reason;
    gsize length;
    guint attempts;
    gint64 due;
    gboolean durable, persisted, pending, retained;
    PcvJobFinished callback;
    gpointer data;
    GDestroyNotify destroy;
} Record;

static struct {
    GMutex mu;
    GCond idle;
    GQueue queue;
    GPtrArray *retry;
    GHashTable *protected_ids, *quarantine_ids, *reconcile_ids;
    gint dirfd;
    guint files, reservations, waiting, retained, timer;
    guint64 bytes, waiting_bytes;
    guint startup_attempts;
    gint64 startup_due;
    gboolean ready, busy, closing, inventory_overflow, startup_pending;
} R;
static gsize once;

                                             
                                                       
static void
_ensure(void)
{
    if (g_once_init_enter(&once)) {
        g_mutex_init(&R.mu);
        g_cond_init(&R.idle);
        R.dirfd = -1;
        g_once_init_leave(&once, 1);
    }
}

PcvJobRef *pcv_job_ref(PcvJobRef *ref)
{ if (ref) g_atomic_int_inc(&ref->refs); return ref; }
const gchar *pcv_job_id(const PcvJobRef *ref) { return ref ? ref->id : NULL; }
const gchar *pcv_job_generation(const PcvJobRef *ref) { return ref ? ref->generation : NULL; }
const gchar *pcv_job_method(const PcvJobRef *ref) { return ref ? ref->method : NULL; }
const gchar *pcv_job_target(const PcvJobRef *ref) { return ref ? ref->target : NULL; }

                                      
                                                
static void
_release_reservation(PcvJobRef *ref)
{
    g_mutex_lock(&R.mu);
    if (ref->reserved) {
        ref->reserved = FALSE;
        R.reservations--;
    }
    g_mutex_unlock(&R.mu);
}

void
pcv_job_unref(PcvJobRef *ref)
{
    if (!ref || !g_atomic_int_dec_and_test(&ref->refs)) return;
    _release_reservation(ref);
    g_free(ref->id);
    g_free(ref->generation);
    g_free(ref->method);
    g_free(ref->target);
    g_free(ref);
}

                                                    
                                                             
static PcvJobRef *
_identity(const gchar *id, const gchar *generation, const gchar *method, const gchar *target)
{
    PcvJobRef *ref = g_new0(PcvJobRef, 1);
    ref->refs = 1;
    ref->id = g_strdup(id);
    ref->generation = g_strdup(generation);
    ref->method = g_strdup(method);
    ref->target = g_strdup(target);
    return ref;
}

PcvJobRef *
pcv_job_admit(const gchar *type, const gchar *target, const gchar *params, const gchar *method)
{
    _ensure();
    if (!type || !method || !*method || strlen(method) > 1024 ||
        (target && strlen(target) > 4096)) return NULL;
    g_mutex_lock(&R.mu);
    gboolean allowed = R.ready && !R.closing && !R.inventory_overflow &&
        R.reservations < WAIT_MAX && R.files + R.reservations < FILES_MAX &&
        R.bytes + (R.reservations + 1) * (guint64)RECORD_MAX <= DISK_MAX;
    if (allowed) R.reservations++;
    g_mutex_unlock(&R.mu);
    if (!allowed) {
        PCV_LOG_WARN("job_recovery", "Tracked admission rejected: recovery capacity unavailable");
        return NULL;
    }
    PcvJobRef *ref = _identity(NULL, NULL, method, target);
    ref->reserved = TRUE;
    ref->generation = g_uuid_string_random();
    ref->id = pcv_job_db_admit(type, target, params, method, ref->generation);
    if (!ref->id) { pcv_job_unref(ref); return NULL; }
    return ref;
}

gboolean pcv_job_progress(PcvJobRef *ref, gint progress, const gchar *detail)
{ return ref && pcv_job_db_progress(ref, progress, detail); }

                                               
                                                             
static void
_record_free(gpointer pointer)
{
    Record *record = pointer;
    if (record->retained) {
        g_mutex_lock(&R.mu);
        R.retained--;
        R.waiting_bytes -= record->length;
        g_mutex_unlock(&R.mu);
    }
    if (record->destroy) record->destroy(record->data);
    pcv_job_unref(record->ref);
    g_free(record->result);
    g_free(record->error);
    g_free(record->encoded);
    g_free(record->name);
    g_free(record->reason);
    g_free(record);
}

                                                      
                                                              
static gboolean
_deliver(gpointer pointer)
{
    Record *record = pointer;
    if (record->callback)
        record->callback(record->ref, record->status, record->error, record->persisted,
                         record->pending, record->reason, record->data);
    _record_free(record);
    return G_SOURCE_REMOVE;
}

                                             
                                                                   
static gchar *
_encode(Record *record, gsize *length)
{
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "version", 1);
    json_object_set_string_member(obj, "job_id", record->ref->id);
    json_object_set_string_member(obj, "generation", record->ref->generation);
    json_object_set_string_member(obj, "method", record->ref->method);
    if (record->ref->target) json_object_set_string_member(obj, "target", record->ref->target);
    else json_object_set_null_member(obj, "target");
    json_object_set_int_member(obj, "status", record->status);
    if (record->result) json_object_set_string_member(obj, "result", record->result);
    else json_object_set_null_member(obj, "result");
    if (record->error) json_object_set_string_member(obj, "error", record->error);
    else json_object_set_null_member(obj, "error");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *payload = json_to_string(node, FALSE);
    json_node_free(node);
    gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, -1);
    obj = json_object_new();
    json_object_set_string_member(obj, "payload", payload);
    json_object_set_string_member(obj, "sha256", checksum);
    node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *encoded = json_to_string(node, FALSE);
    *length = strlen(encoded);
    g_free(checksum);
    g_free(payload);
    json_node_free(node);
    return encoded;
}

                                                  
                                                 
static gboolean
_string(JsonObject *obj, const gchar *key, gboolean nullable)
{
    JsonNode *node = json_object_get_member(obj, key);
    return node && ((nullable && JSON_NODE_HOLDS_NULL(node)) ||
        (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING));
}

static Record *
_decode(const gchar *text)
{
    JsonParser *parser = json_parser_new();
    JsonParser *inner = json_parser_new();
    Record *record = NULL;
    if (!json_parser_load_from_data(parser, text, -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto done;
    JsonObject *outer = json_node_get_object(json_parser_get_root(parser));
    if (json_object_get_size(outer) != 2 || !_string(outer, "payload", FALSE) ||
        !_string(outer, "sha256", FALSE)) goto done;
    const gchar *payload = json_object_get_string_member(outer, "payload");
    gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, -1);
    gboolean valid = g_strcmp0(checksum, json_object_get_string_member(outer, "sha256")) == 0;
    g_free(checksum);
    if (!valid || !json_parser_load_from_data(inner, payload, -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(inner))) goto done;
    JsonObject *obj = json_node_get_object(json_parser_get_root(inner));
    JsonNode *version = json_object_get_member(obj, "version");
    JsonNode *status = json_object_get_member(obj, "status");
    if (json_object_get_size(obj) != 8 || !version || !status ||
        !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 ||
        json_node_get_int(version) != 1 || !JSON_NODE_HOLDS_VALUE(status) ||
        json_node_get_value_type(status) != G_TYPE_INT64 ||
        (json_node_get_int(status) != PCV_JOB_COMPLETED && json_node_get_int(status) != PCV_JOB_FAILED) ||
        !_string(obj, "job_id", FALSE) || !_string(obj, "generation", FALSE) ||
        !_string(obj, "method", FALSE) || !_string(obj, "target", TRUE) ||
        !_string(obj, "result", TRUE) || !_string(obj, "error", TRUE)) goto done;
    const gchar *id = json_object_get_string_member(obj, "job_id");
    const gchar *generation = json_object_get_string_member(obj, "generation");
    if (strlen(id) != 12 || !g_str_has_prefix(id, "job-") || !g_uuid_string_is_valid(generation)) goto done;
    for (guint i = 4; i < 12; i++) if (!g_ascii_isxdigit(id[i])) goto done;
    record = g_new0(Record, 1);
    record->ref = _identity(id, generation, json_object_get_string_member(obj, "method"),
        JSON_NODE_HOLDS_NULL(json_object_get_member(obj, "target")) ? NULL : json_object_get_string_member(obj, "target"));
    record->status = json_node_get_int(status);
    if (!JSON_NODE_HOLDS_NULL(json_object_get_member(obj, "result")))
        record->result = g_strdup(json_object_get_string_member(obj, "result"));
    if (!JSON_NODE_HOLDS_NULL(json_object_get_member(obj, "error")))
        record->error = g_strdup(json_object_get_string_member(obj, "error"));
    record->durable = TRUE;
done:
    g_object_unref(inner);
    g_object_unref(parser);
    return record;
}

                                                             
                                             
static gboolean
_write_record(Record *record)
{
    gchar *temp = g_strdup_printf("%s.tmp", record->name);
    gint fd = openat(R.dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    gboolean ok = fd >= 0;
    gsize offset = 0;
    while (ok && offset < record->length) {
        ssize_t n = write(fd, record->encoded + offset, record->length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = FALSE; break; }
        offset += (gsize)n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) ok = FALSE;
    if (ok) ok = renameat(R.dirfd, temp, R.dirfd, record->name) == 0;
    if (ok) ok = fsync(R.dirfd) == 0;
    g_free(temp);
    return ok;
}

static gboolean
_remove_record(const gchar *name)
{
    gboolean removed = unlinkat(R.dirfd, name, 0) == 0 || errno == ENOENT;
    return removed && fsync(R.dirfd) == 0;
}

                                                     
static void _protect(const gchar *id, const gchar *reason);

static void
_quarantine(const gchar *name, const gchar *reason)
{
    if (strlen(name) >= 12 && g_str_has_prefix(name, "job-")) {
        gchar *id = g_strndup(name, 12);
        _protect(id, reason);
        g_free(id);
    }
    gchar *unique = g_uuid_string_random();
    gchar *destination = g_strdup_printf("%s.%s.%s.quarantine", name, reason, unique);
    if (renameat(R.dirfd, name, R.dirfd, destination) == 0) (void)fsync(R.dirfd);
    g_free(destination);
    g_free(unique);
    PCV_LOG_WARN("job_recovery", "Journal quarantined (reason=%s)", reason);
}

                                                                      
                                            
static void
_inventory(void)
{
    guint files = 0;
    guint64 bytes = 0;
    gboolean overflow = FALSE;
    gint duplicate = openat(R.dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir = duplicate >= 0 ? fdopendir(duplicate) : NULL;
    if (!dir) { if (duplicate >= 0) close(duplicate); overflow = TRUE; }
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) continue;
        struct stat st;
        files++;
        if (fstatat(R.dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) overflow = TRUE;
        else bytes += MAX((gint64)st.st_size, 0);
        if (files > FILES_MAX || bytes > DISK_MAX) { overflow = TRUE; break; }
    }
    if (dir) closedir(dir);
    g_mutex_lock(&R.mu);
    R.files = files;
    R.bytes = bytes;
    R.inventory_overflow = overflow;
    g_mutex_unlock(&R.mu);
}

static void
_protect(const gchar *id, const gchar *reason)
{
    g_mutex_lock(&R.mu);
    GHashTable *map = g_str_equal(reason, "db_replay_pending") || g_str_equal(reason, "retry_exhausted") || g_str_equal(reason, "retry_capacity")
        ? R.protected_ids : R.quarantine_ids;
    if (map) g_hash_table_replace(map, g_strdup(id), g_strdup(reason));
    g_mutex_unlock(&R.mu);
}

                                                      
                                                       
                                                            
static void
_process(Record *record)
{
    if (!record->durable) {
                                                        
                                                                  
        Record *decoded = _decode(record->encoded);
        if (!decoded) { record->reason = g_strdup("invalid_memory_record"); return; }
        record->result = g_steal_pointer(&decoded->result);
        record->error = g_steal_pointer(&decoded->error);
        _record_free(decoded);
        record->durable = _write_record(record);
        if (!record->durable) {
            record->reason = g_strdup("journal_io_failure");
            _protect(record->ref->id, record->reason);
            g_clear_pointer(&record->encoded, g_free);
            _inventory();
            return;
        }
    }
    g_clear_pointer(&record->encoded, g_free);
    record->attempts++;
    PcvJobApply applied = pcv_job_db_apply(record->ref, record->status, record->result);
    record->persisted = applied == PCV_JOB_APPLIED;
    if (record->persisted && _remove_record(record->name)) {
        g_mutex_lock(&R.mu);
        g_hash_table_remove(R.protected_ids, record->ref->id);
        g_mutex_unlock(&R.mu);
    } else if (applied == PCV_JOB_CONFLICT) {
        _quarantine(record->name, "identity_or_terminal_conflict");
        _protect(record->ref->id, "identity_or_terminal_conflict");
        g_free(record->reason);
        record->reason = g_strdup("identity_or_terminal_conflict");
    } else {
        record->pending = TRUE;
        g_free(record->reason);
        record->reason = g_strdup(record->attempts >= RETRY_MAX ? "retry_exhausted" : "db_replay_pending");
        record->due = g_get_monotonic_time() + (gint64)MIN(60u, 1u << MIN(record->attempts - 1, 6u)) * G_USEC_PER_SEC;
        _protect(record->ref->id, record->reason);
    }
    _inventory();
}

                                                               
                                                            
static void
_notify(Record *record)
{
    if (!record->callback && !record->destroy) return;
    Record *copy = g_new0(Record, 1);
    copy->ref = pcv_job_ref(record->ref);
    copy->status = record->status;
    copy->error = g_strdup(record->error);
    copy->reason = g_strdup(record->reason);
    copy->persisted = record->persisted;
    copy->pending = record->pending;
    copy->callback = record->callback;
    copy->data = record->data;
    copy->destroy = record->destroy;
    record->callback = NULL;
    record->destroy = NULL;
    pcv_drain_invoke(NULL, _deliver, copy);
}

                                           
                                                      
                                                 
static void
_writer(GTask *task, gpointer source G_GNUC_UNUSED,
        gpointer data G_GNUC_UNUSED, GCancellable *cancel G_GNUC_UNUSED)
{
    for (;;) {
        g_mutex_lock(&R.mu);
                                               
                                                            
                                                               
        if (R.startup_pending && !R.closing && R.startup_attempts < RETRY_MAX &&
            R.startup_due <= g_get_monotonic_time()) {
            GHashTable *ids = g_hash_table_ref(R.reconcile_ids);
            R.startup_attempts++;
            g_mutex_unlock(&R.mu);
            gboolean reconciled = pcv_job_db_reconcile(ids);
            g_hash_table_unref(ids);
            g_mutex_lock(&R.mu);
            if (reconciled) {
                R.startup_pending = FALSE;
                R.ready = !R.closing;
            } else {
                R.startup_due = g_get_monotonic_time() +
                    (gint64)MIN(60u, 1u << MIN(R.startup_attempts - 1, 6u)) * G_USEC_PER_SEC;
            }
            g_mutex_unlock(&R.mu);
            continue;
        }
        Record *record = g_queue_pop_head(&R.queue);
        if (!record) {
            R.busy = FALSE;
            g_cond_broadcast(&R.idle);
            g_mutex_unlock(&R.mu);
            break;
        }
        R.waiting--;
        g_mutex_unlock(&R.mu);
        _process(record);
        _notify(record);
        if (record->pending) {
            g_mutex_lock(&R.mu);
            g_ptr_array_add(R.retry, record);
            g_mutex_unlock(&R.mu);
        } else _record_free(record);
    }
    g_task_return_boolean(task, TRUE);
}

                                                                 
static void
_start_locked(void)
{
    gboolean startup_due = R.startup_pending && !R.closing && R.startup_attempts < RETRY_MAX &&
        R.startup_due <= g_get_monotonic_time();
    if (R.busy || (g_queue_is_empty(&R.queue) && !startup_due)) return;
    R.busy = TRUE;
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_run_in_thread(task, _writer);
    g_object_unref(task);
}

void
pcv_job_finish_async(PcvJobRef *ref, PcvJobStatus status, const gchar *result,
                     const gchar *error, PcvJobFinished callback, gpointer data, GDestroyNotify destroy)
{
    _ensure();
    if (!ref) { if (destroy) destroy(data); return; }
    Record *record = g_new0(Record, 1);
    record->ref = pcv_job_ref(ref);
    record->status = status;
    record->callback = callback;
    record->data = data;
    record->destroy = destroy;
                                                    
    if ((status != PCV_JOB_COMPLETED && status != PCV_JOB_FAILED) ||
        (result && strnlen(result, RECORD_MAX + 1) > RECORD_MAX) ||
        (error && strnlen(error, RECORD_MAX + 1) > RECORD_MAX)) {
        record->error = g_strdup("Result journal limit exceeded");
        record->reason = g_strdup("record_limit");
    } else {
        record->result = (gchar *)result;                      
        record->error = (gchar *)error;
        record->encoded = _encode(record, &record->length);
        record->result = NULL;
        record->error = NULL;
        record->name = g_strdup_printf("%s-%s.json", ref->id, ref->generation);
        if (record->length > RECORD_MAX) record->reason = g_strdup("record_limit");
    }
    g_mutex_lock(&R.mu);
    gboolean duplicate = ref->submitted;
    if (!duplicate) ref->submitted = TRUE;
    gboolean allowed = !duplicate && !record->reason && R.ready && !R.closing &&
        R.retained < WAIT_MAX && R.waiting_bytes + record->length <= WAIT_BYTES;
    if (allowed) {
        R.waiting++;
        R.retained++;
        record->retained = TRUE;
        R.waiting_bytes += record->length;
        g_queue_push_tail(&R.queue, record);
        _start_locked();
    }
    g_mutex_unlock(&R.mu);
    if (!allowed) {
        if (duplicate) { _record_free(record); return; }
        if (!record->reason) record->reason = g_strdup("writer_unavailable");
        PCV_LOG_WARN("job_recovery", "Result not journaled (reason=%s)", record->reason);
        _protect(ref->id, record->reason);
        _notify(record);
        _record_free(record);
    }
}

                                                     
                                                      
                                                           
static gboolean
_retry_tick(gpointer unused G_GNUC_UNUSED)
{
    g_mutex_lock(&R.mu);
    if (!R.closing) {
        for (guint i = 0; i < R.retry->len;) {
            Record *record = g_ptr_array_index(R.retry, i);
            if (record->attempts < RETRY_MAX && record->due <= g_get_monotonic_time()) {
                g_ptr_array_steal_index_fast(R.retry, i);
                record->pending = FALSE;
                R.waiting++;
                g_queue_push_tail(&R.queue, record);
            } else i++;
        }
        _start_locked();
    }
    g_mutex_unlock(&R.mu);
    return G_SOURCE_CONTINUE;
}

gboolean
pcv_job_recovery_pending(const gchar *id)
{
    _ensure();
    g_mutex_lock(&R.mu);
    const gchar *reason = R.protected_ids ? g_hash_table_lookup(R.protected_ids, id) : NULL;
    gboolean pending = reason && (g_str_equal(reason, "db_replay_pending") ||
        g_str_equal(reason, "retry_exhausted") || g_str_equal(reason, "retry_capacity"));
    g_mutex_unlock(&R.mu);
    return pending;
}

gboolean
pcv_job_recovery_protected(const gchar *id)
{
    _ensure();
    g_mutex_lock(&R.mu);
    gboolean protected = R.protected_ids && (g_hash_table_contains(R.protected_ids, id) ||
        g_hash_table_contains(R.quarantine_ids, id));
    g_mutex_unlock(&R.mu);
    return protected;
}

                                             
gchar *
pcv_job_recovery_reason(const gchar *id)
{
    _ensure();
    g_mutex_lock(&R.mu);
    const gchar *reason = R.protected_ids ? g_hash_table_lookup(R.protected_ids, id) : NULL;
    if (!reason && R.quarantine_ids) reason = g_hash_table_lookup(R.quarantine_ids, id);
    gchar *copy = g_strdup(reason);
    g_mutex_unlock(&R.mu);
    return copy;
}

void
pcv_job_recovery_init(const gchar *db_path)
{
    _ensure();
    R.retry = g_ptr_array_new_with_free_func(_record_free);
    R.protected_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    R.quarantine_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    R.closing = FALSE;
    gchar *directory = g_strconcat(db_path, ".journal", NULL);
                                                                   
    gboolean ok = g_mkdir(directory, 0700) == 0 || errno == EEXIST;
    R.dirfd = ok ? open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
    struct stat st;
    ok = R.dirfd >= 0 && fstat(R.dirfd, &st) == 0 && (st.st_mode & 0077) == 0;
    gchar *parent = g_path_get_dirname(directory);
    gint parentfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ok) ok = parentfd >= 0 && fsync(parentfd) == 0;
    if (parentfd >= 0) close(parentfd);
    g_free(parent);
    g_free(directory);
    GHashTable *durable_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (ok) {
        _inventory();
        gint duplicate = openat(R.dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *dir = duplicate >= 0 ? fdopendir(duplicate) : NULL;
        struct dirent *entry;
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        while (dir && (entry = readdir(dir))) {
            if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) continue;
            if (names->len == FILES_MAX) { ok = FALSE; break; }
            g_ptr_array_add(names, g_strdup(entry->d_name));
        }
        if (dir) closedir(dir);
        else { if (duplicate >= 0) close(duplicate); ok = FALSE; }
        for (guint i = 0; ok && i < names->len; i++) {
            const gchar *name = g_ptr_array_index(names, i);
                                                        
            if (!g_str_has_suffix(name, ".json") && strlen(name) >= 12 && g_str_has_prefix(name, "job-")) {
                gchar *id = g_strndup(name, 12);
                _protect(id, "quarantine");
                g_free(id);
            }
            if (g_str_has_suffix(name, ".quarantine")) continue;
            if (!g_str_has_suffix(name, ".json")) {
                _quarantine(name, "partial_or_unknown");
                continue;
            }
            gint fd = openat(R.dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            gchar buffer[RECORD_MAX + 1];
            gsize length = 0;
            gboolean read_ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_size > 0 && st.st_size <= RECORD_MAX && (st.st_mode & 0077) == 0;
            while (read_ok && length < (gsize)st.st_size) {
                ssize_t n = read(fd, buffer + length, st.st_size - length);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { read_ok = FALSE; break; }
                length += n;
            }
            if (fd >= 0) close(fd);
            buffer[length] = 0;
            Record *record = read_ok && !memchr(buffer, 0, length) ? _decode(buffer) : NULL;
            if (!record) { _quarantine(name, "invalid_record"); continue; }
            gchar *expected = g_strdup_printf("%s-%s.json", record->ref->id, record->ref->generation);
            gboolean same_name = g_str_equal(expected, name);
            g_free(expected);
            if (!same_name) { _record_free(record); _quarantine(name, "identity_filename"); continue; }
            record->name = g_strdup(name);
            record->length = length;
            _process(record);
            if (record->pending) {
                g_hash_table_add(durable_ids, g_strdup(record->ref->id));
                if (R.retained < WAIT_MAX && R.waiting_bytes + record->length <= WAIT_BYTES) {
                    record->ref->reserved = TRUE;
                    R.reservations++;
                    record->retained = TRUE;
                    R.retained++;
                    R.waiting_bytes += record->length;
                    g_ptr_array_add(R.retry, record);
                } else {
                    _protect(record->ref->id, "retry_capacity");
                    _record_free(record);                            
                }
            } else _record_free(record);
        }
        g_ptr_array_unref(names);
        _inventory();
    }
                                                            
    R.reconcile_ids = durable_ids;
    R.ready = ok && pcv_job_db_reconcile(durable_ids);
    R.startup_pending = ok && !R.ready;
    R.startup_attempts = R.startup_pending ? 1 : 0;
    R.startup_due = g_get_monotonic_time() + G_USEC_PER_SEC;
    if (ok) R.timer = g_timeout_add_seconds(1, _retry_tick, NULL);
    else PCV_LOG_WARN("job_recovery", "Journal unavailable — tracked admission disabled");
}

                                        
                                                                 
void
pcv_job_recovery_shutdown(void)
{
    _ensure();
    g_mutex_lock(&R.mu);
    R.closing = TRUE;
    guint timer = R.timer;
    R.timer = 0;
    g_mutex_unlock(&R.mu);
    if (timer) g_source_remove(timer);
    g_mutex_lock(&R.mu);
    while (R.busy) g_cond_wait(&R.idle, &R.mu);
    R.ready = FALSE;
    if (R.dirfd >= 0) close(R.dirfd);
    R.dirfd = -1;
    GPtrArray *retry = R.retry;
    R.retry = NULL;
    g_clear_pointer(&R.protected_ids, g_hash_table_unref);
    g_clear_pointer(&R.quarantine_ids, g_hash_table_unref);
    g_clear_pointer(&R.reconcile_ids, g_hash_table_unref);
    R.startup_pending = FALSE;
    R.startup_attempts = 0;
    R.files = 0;
    R.bytes = 0;
    R.inventory_overflow = FALSE;
    g_mutex_unlock(&R.mu);
    if (retry) g_ptr_array_unref(retry);
}
