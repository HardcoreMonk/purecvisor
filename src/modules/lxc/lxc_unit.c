                                                                             
                             
                                                                                       
                                                         
  
                           
                                                                                     
                                                                                  
                                                                                  
                                                                                              
                                                                                    
                                                                                                         
                                                                                                 
                                                                                            
                                                                                          
                                                                            
                                                                                                                                             
                                                                       
                                           
                                                                   
                                                                             
                       
  
                                                                         
                                                                       
                                                                                
#include "lxc_unit.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <lxc/lxccontainer.h>

#include "api/drain.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

#define UNIT_LOG_DOM         "lxc_unit"
#define UNIT_OUTPUT_MAX      ((gsize)64 * 1024)                                          
#define UNIT_WAIT_MAX_POLLS  40                                                               
#define UNIT_CAUSE_LINES     3                                                
#define UNIT_PATH_INVALID    "container path '%s' must be absolute and use only [A-Za-z0-9/._-]"

                                                                    

                                                                          
                                                             
static gboolean
_real_run(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out,
          gint *exit_status, GError **error)
{
    const PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_BASE,                                                    
        .timeout_sec       = timeout_sec,
        .max_output_bytes  = UNIT_OUTPUT_MAX,
        .cancellable       = NULL,
        .should_abort      = pcv_drain_is_terminating,
        .new_process_group = TRUE,
    };
    return pcv_spawn_sync_bounded(argv, &bound, stdout_out, stderr_out, exit_status, error);
}

                                                                 
                                      
static gboolean
_real_is_running(const gchar *lxc_path, const gchar *name)
{
    struct lxc_container *c = lxc_container_new(name, lxc_path);
    if (!c)
        return FALSE;
    gboolean running = c->is_running(c);
    lxc_container_put(c);
    return running;
}

                                                            
static void
_real_sleep_ms(guint ms)
{
    g_usleep((gulong)ms * 1000);
}

static const PcvLxcUnitOps G_unit_real_ops = {
    .run        = _real_run,
    .is_running = _real_is_running,
    .sleep_ms   = _real_sleep_ms,
    .now_us     = g_get_monotonic_time,
};

                                                        
static const PcvLxcUnitOps *G_unit_ops_test;
                                                                                  
static GMutex G_unit_env_mu;
static gchar *G_unit_env_path_test;

                                                                   
static const PcvLxcUnitOps *
_unit_ops(void)
{
    const PcvLxcUnitOps *ops = g_atomic_pointer_get(&G_unit_ops_test);
    return ops ? ops : &G_unit_real_ops;
}

                                                                               
static gchar *
_unit_env_path(void)
{
    g_mutex_lock(&G_unit_env_mu);
    gchar *p = g_strdup(G_unit_env_path_test ? G_unit_env_path_test : PCV_LXC_UNIT_ENV_PATH);
    g_mutex_unlock(&G_unit_env_mu);
    return p;
}

                                                                 

                                                              
                                                  
gboolean
pcv_lxc_unit_path_valid(const gchar *lxc_path)
{
    if (!lxc_path || lxc_path[0] != '/')
        return FALSE;
    for (const gchar *p = lxc_path; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '/' && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

                                                                   
                                 
gchar *
pcv_lxc_unit_env_text(const gchar *lxc_path)
{
    return pcv_lxc_unit_path_valid(lxc_path) ? g_strdup_printf("PCV_LXC_PATH=%s\n", lxc_path) : NULL;
}

                                                                      
                                                        
gboolean
pcv_lxc_unit_env_write(const gchar *env_path, const gchar *lxc_path, GError **error)
{
    g_return_val_if_fail(env_path && *env_path, FALSE);
    g_autofree gchar *text = pcv_lxc_unit_env_text(lxc_path);
    if (!text) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, UNIT_PATH_INVALID, lxc_path ? lxc_path : "(null)");
        return FALSE;
    }
    g_autofree gchar *cur = NULL;
    if (g_file_get_contents(env_path, &cur, NULL, NULL) && g_strcmp0(cur, text) == 0)
        return TRUE;                                                                
    g_autofree gchar *dir = g_path_get_dirname(env_path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", dir, g_strerror(e));
        return FALSE;
    }
                                                                     
    return g_file_set_contents_full(env_path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
}

                                                                     
                                                       
gboolean
pcv_lxc_unit_log_rotate(const gchar *log_path, goffset limit, GError **error)
{
    g_return_val_if_fail(log_path && *log_path, FALSE);
    GStatBuf st;
    if (g_lstat(log_path, &st) != 0 || !S_ISREG(st.st_mode) || (goffset)st.st_size <= limit)
        return TRUE;
    g_autofree gchar *old = g_strconcat(log_path, ".1", NULL);
    if (g_rename(log_path, old) != 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", log_path, g_strerror(e));
        return FALSE;
    }
    return TRUE;
}

                                                                  
                                                  
static gboolean
_is_timestamp(const gchar *s, gsize len)
{
    guint dots = 0;
    if (len < 3 || s[0] == '.' || s[len - 1] == '.')
        return FALSE;
    for (gsize i = 0; i < len; i++) {
        if (s[i] == '.')
            dots++;
        else if (!g_ascii_isdigit(s[i]))
            return FALSE;
    }
    return dots == 1;
}

                                                                                             
                                                                                            
                                                        
static const gchar *
_error_message(const gchar *line)
{
    const gchar *p = line;
    for (guint tok = 0; tok < 3 && *p; tok++) {                                       
        while (*p == ' ' || *p == '\t')
            p++;
        const gchar *s = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!_is_timestamp(s, (gsize)(p - s)))
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!g_str_has_prefix(p, "ERROR") || (p[5] != ' ' && p[5] != '\t' && p[5] != '\0'))
            return NULL;
        p += 5;
        for (const gchar *q = strchr(p, ':'); q; q = strchr(q + 1, ':')) {
            const gchar *d = q + 1;
            if (!g_ascii_isdigit(*d))
                continue;
            while (g_ascii_isdigit(*d))
                d++;
            if (g_str_has_prefix(d, " - "))
                return d + 3;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        return p;
    }
    return NULL;
}

                                                                              
                                             
gchar *
pcv_lxc_unit_log_cause(const gchar *log_tail)
{
    if (!log_tail || !*log_tail)
        return NULL;
    g_auto(GStrv) lines = g_strsplit(log_tail, "\n", -1);
    GString *out = g_string_new(NULL);
    guint n = 0;
    for (guint i = 0; lines[i] && n < UNIT_CAUSE_LINES && out->len <= PCV_LXC_UNIT_CAUSE_MAX; i++) {
        const gchar *msg = _error_message(lines[i]);
        if (!msg)
            continue;
        g_autofree gchar *valid = g_utf8_make_valid(msg, -1);                                        
        g_strstrip(valid);
        if (!*valid)
            continue;
        if (n++)
            g_string_append(out, "; ");
        g_string_append(out, valid);
    }
    if (n == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    const gchar *p = out->str;
    while (*p) {                                                                    
        const gchar *next = g_utf8_next_char(p);
        if ((gsize)(next - out->str) > PCV_LXC_UNIT_CAUSE_MAX)
            break;
        p = next;
    }
    g_string_truncate(out, (gsize)(p - out->str));
    return g_string_free(out, FALSE);
}

                                                                    

                                                                                          
                                                                                                     
                                                    
static gchar **
_unit_argv(const gchar *verb, const gchar *unit)
{
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup("systemctl"));
    g_ptr_array_add(a, g_strdup(verb));
    if (g_strcmp0(verb, "show") == 0) {
        g_ptr_array_add(a, g_strdup("--property=LoadState"));
        g_ptr_array_add(a, g_strdup("--property=ActiveState"));
    }
    g_ptr_array_add(a, g_strdup(unit));
    g_ptr_array_add(a, NULL);
    return (gchar **)g_ptr_array_free(a, FALSE);
}

                                                         
                                      
static gchar *
_first_line(const gchar *text)
{
    if (!text)
        return NULL;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        g_strstrip(lines[i]);
        if (*lines[i])
            return g_utf8_make_valid(lines[i], -1);
    }
    return NULL;
}

                                                                     
                                                                              
static void
_set_run_error(GError **error, const gchar *name, const gchar *verb, guint timeout_sec, const GError *run_err)
{
    if (g_error_matches(run_err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
        if (g_strcmp0(verb, "start") == 0)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "lxc-start failed for '%s': systemctl start timed out after %us", name, timeout_sec);
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "lxc-start failed for '%s': systemctl %s timed out", name, verb);
    } else if (g_error_matches(run_err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                    "lxc-start failed for '%s': cancelled (daemon shutting down)", name);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lxc-start failed for '%s': %s",
                    name, run_err ? run_err->message : "unknown error");
    }
}

                                                            
                                         
static guint
_prep_timeout(const PcvLxcUnitOps *ops, gint64 deadline)
{
    gint64 left = (deadline - ops->now_us()) / G_USEC_PER_SEC;
    return left < 1 ? 1 : (guint)left;
}

                                                                                         
                                                    
static gboolean
_unit_show(const PcvLxcUnitOps *ops, const gchar *name, const gchar *unit, guint timeout_sec,
           gchar **load_out, gchar **active_out, GError **error)
{
    g_auto(GStrv) argv = _unit_argv("show", unit);
    g_autofree gchar *out = NULL, *err_out = NULL;
    gint status = -1;
    GError *run_err = NULL;
    if (!ops->run((const gchar * const *)argv, timeout_sec, &out, &err_out, &status, &run_err)) {
        _set_run_error(error, name, "show", timeout_sec, run_err);
        g_clear_error(&run_err);
        return FALSE;
    }
    if (status != 0) {
        g_autofree gchar *first = _first_line(err_out);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lxc-start failed for '%s': systemctl show exit %d: %s",
                    name, status, first ? first : "(no stderr)");
        return FALSE;
    }
    g_autofree gchar *load = NULL, *active = NULL;
    g_auto(GStrv) lines = g_strsplit(out ? out : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        g_strstrip(lines[i]);
        if (g_str_has_prefix(lines[i], "LoadState=") && !load)
            load = g_strdup(lines[i] + strlen("LoadState="));
        else if (g_str_has_prefix(lines[i], "ActiveState=") && !active)
            active = g_strdup(lines[i] + strlen("ActiveState="));
    }
    *load_out = load ? g_steal_pointer(&load) : g_strdup("");
    *active_out = active ? g_steal_pointer(&active) : g_strdup("");
    return TRUE;
}

                                                                                      
                                         
static gboolean
_unit_reset_failed(const PcvLxcUnitOps *ops, const gchar *name, const gchar *unit, guint timeout_sec, GError **error)
{
    g_auto(GStrv) argv = _unit_argv("reset-failed", unit);
    g_autofree gchar *out = NULL, *err_out = NULL;
    gint status = -1;
    GError *run_err = NULL;
    if (!ops->run((const gchar * const *)argv, timeout_sec, &out, &err_out, &status, &run_err)) {
        _set_run_error(error, name, "reset-failed", timeout_sec, run_err);
        g_clear_error(&run_err);
        return FALSE;
    }
    if (status != 0)
        PCV_LOG_DEBUG(UNIT_LOG_DOM, "%s: reset-failed exit %d (ignored)", unit, status);
    return TRUE;
}

                                                                                      
                                                                   
                                                                      
void
pcv_lxc_unit_reset_failed_best_effort(const gchar *name)
{
    g_return_if_fail(name != NULL);
    const PcvLxcUnitOps *ops = _unit_ops();
    g_autofree gchar *unit = g_strdup_printf("%s%s.service", PCV_LXC_UNIT_TEMPLATE, name);
    g_auto(GStrv) argv = _unit_argv("reset-failed", unit);
    g_autofree gchar *out = NULL, *err_out = NULL;
    gint status = -1;
    g_autoptr(GError) run_err = NULL;
    if (!ops->run((const gchar * const *)argv, PCV_LXC_UNIT_PREP_BUDGET_SEC, &out, &err_out, &status, &run_err)) {
        PCV_LOG_WARN(UNIT_LOG_DOM, "%s: reset-failed after destroy failed: %s", unit,
                     run_err ? run_err->message : "unknown");
        return;
    }
    if (status != 0)
        PCV_LOG_DEBUG(UNIT_LOG_DOM, "%s: reset-failed exit %d (ignored)", unit, status);
}

                                                                           
                                 
static gboolean
_unit_is_settling(const gchar *state)
{
    static const gchar * const busy[] = { "active", "activating", "deactivating", "reloading", "refreshing", NULL };
    return state && g_strv_contains(busy, state);
}

                                                                                               
                                                                                        
                                                                         
                                                     
static gchar *
_log_read_from(const gchar *path, goffset offset)
{
    int fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    const gsize cap = (gsize)PCV_LXC_LOG_ROTATE_BYTES;
    const gboolean tail = offset < 0;
    if (tail)
        offset = MAX((goffset)0, (goffset)st.st_size - (goffset)cap);
    else if ((goffset)st.st_size < offset)
        offset = 0;
    gchar *buf = g_malloc(cap + 1);
    gsize got = 0;
    if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset) {
        while (got < cap) {
            ssize_t r = read(fd, buf + got, cap - got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += (gsize)r;
        }
    }
    close(fd);
    buf[got] = '\0';
    if (tail && offset > 0) {                                                            
        const gchar *nl = memchr(buf, '\n', got);
        const gsize skip = nl ? (gsize)(nl - buf) + 1 : got;
        memmove(buf, buf + skip, got - skip + 1);
    }
    return buf;
}

                                                                                     
                                                                        
GPtrArray *
pcv_lxc_unit_log_tail(const gchar *lxc_path, const gchar *name, const gchar *fallback_dir,
                      gint n_lines, gint *total, GError **error)
{
    g_return_val_if_fail(lxc_path && name && *name, NULL);
    g_autofree gchar *primary = g_strdup_printf("%s/%s/%s.log", lxc_path, name, name);
    g_autofree gchar *text = _log_read_from(primary, -1);
    if (!text && fallback_dir) {
        g_autofree gchar *fallback = g_strdup_printf("%s/%s.log", fallback_dir, name);
        text = _log_read_from(fallback, -1);
    }
    if (!text) {
        g_autofree gchar *config = g_strdup_printf("%s/%s/config", lxc_path, name);
        if (!g_file_test(config, G_FILE_TEST_EXISTS)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "container '%s' has no log file and no config", name);
            return NULL;
        }
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) lines = g_strsplit(text ? text : "", "\n", -1);
    gint count = (gint)g_strv_length(lines);
    if (count > 0 && lines[count - 1][0] == '\0')                                           
        count--;
    for (gint i = MAX(0, count - MAX(n_lines, 0)); i < count; i++)
        g_ptr_array_add(out, g_utf8_make_valid(lines[i], -1));
    if (total)
        *total = count;
    return out;
}

                                                               
static goffset
_file_size(const gchar *path)
{
    GStatBuf st;
    return (g_lstat(path, &st) == 0 && S_ISREG(st.st_mode)) ? (goffset)st.st_size : 0;
}

                                                           
                                                           
gboolean
pcv_lxc_unit_start(const gchar *lxc_path, const gchar *name, GError **error)
{
                                                                                                        
                                                                                                
                                                                                      
                                                                                                      
    g_return_val_if_fail(name && *name, FALSE);
    if (!pcv_lxc_unit_path_valid(lxc_path)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, UNIT_PATH_INVALID, lxc_path ? lxc_path : "(null)");
        return FALSE;
    }
    const PcvLxcUnitOps *ops = _unit_ops();
    g_autofree gchar *unit = g_strconcat(PCV_LXC_UNIT_TEMPLATE, name, ".service", NULL);
    const gint64 deadline = ops->now_us() + (gint64)PCV_LXC_UNIT_PREP_BUDGET_SEC * G_USEC_PER_SEC;

                                                                                
                                                                         
    g_autofree gchar *load = NULL, *active = NULL;
    if (!_unit_show(ops, name, unit, _prep_timeout(ops, deadline), &load, &active, error))
        return FALSE;
    gint64 step = (gint64)PCV_LXC_UNIT_POLL_MS * 1000;
    for (guint polls = 0; _unit_is_settling(active) && !ops->is_running(lxc_path, name); polls++) {
        const gint64 t = ops->now_us();
        if (polls >= UNIT_WAIT_MAX_POLLS || t + step > deadline) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "lxc-start failed for '%s': %s is still %s while the container is stopped", name, unit, active);
            return FALSE;
        }
        ops->sleep_ms(PCV_LXC_UNIT_POLL_MS);
        g_clear_pointer(&load, g_free);
        g_clear_pointer(&active, g_free);
        if (!_unit_show(ops, name, unit, _prep_timeout(ops, deadline), &load, &active, error))
            return FALSE;
        step = MAX(step, ops->now_us() - t);
    }
    if (g_strcmp0(load, "not-found") == 0) {                                       
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, PCV_LXC_UNIT_NOT_INSTALLED);
        return FALSE;
    }
    if (g_strcmp0(active, "failed") == 0 &&
        !_unit_reset_failed(ops, name, unit, _prep_timeout(ops, deadline), error))
        return FALSE;

                                                                       
    g_autofree gchar *env_path = _unit_env_path();
    GError *werr = NULL;
    if (!pcv_lxc_unit_env_write(env_path, lxc_path, &werr)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lxc-start failed for '%s': cannot write %s: %s",
                    name, env_path, werr->message);
        g_clear_error(&werr);
        return FALSE;
    }
    g_autofree gchar *log_path = g_strdup_printf("%s/%s/%s.log", lxc_path, name, name);
    if (!pcv_lxc_unit_log_rotate(log_path, PCV_LXC_LOG_ROTATE_BYTES, &werr)) {
        PCV_LOG_WARN(UNIT_LOG_DOM, "container '%s': LXC log not rotated: %s", name, werr->message);
        g_clear_error(&werr);
    }
    const goffset offset = _file_size(log_path);

                                                                      
    g_auto(GStrv) argv = _unit_argv("start", unit);
    g_autofree gchar *out = NULL, *err_out = NULL;
    gint status = -1;
    GError *run_err = NULL;
    if (!ops->run((const gchar * const *)argv, PCV_LXC_UNIT_START_TIMEOUT_SEC, &out, &err_out, &status, &run_err)) {
        _set_run_error(error, name, "start", PCV_LXC_UNIT_START_TIMEOUT_SEC, run_err);
        g_clear_error(&run_err);
        return FALSE;
    }
    if (status != 0 && err_out && strstr(err_out, "not found")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, PCV_LXC_UNIT_NOT_INSTALLED);
        return FALSE;
    }
    if (status == 0 && ops->is_running(lxc_path, name)) {
        PCV_LOG_INFO(UNIT_LOG_DOM, "container '%s' started by %s", name, unit);
        return TRUE;
    }
    g_autofree gchar *tail = _log_read_from(log_path, offset);
    g_autofree gchar *cause = pcv_lxc_unit_log_cause(tail);
    if (cause) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lxc-start failed for '%s': %s", name, cause);
    } else if (status != 0) {
        g_autofree gchar *first = _first_line(err_out);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lxc-start failed for '%s': systemctl start exit %d: %s",
                    name, status, first ? first : "(no stderr)");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "lxc-start failed for '%s': container is not RUNNING after %s started", name, unit);
    }
    return FALSE;
}

                                                                          
                                                 
                                                            
void
pcv_lxc_unit_set_ops_for_test(const PcvLxcUnitOps *ops)
{
    g_atomic_pointer_set(&G_unit_ops_test, ops);
}

                                                                                   
                                              
                                                            
void
pcv_lxc_unit_set_env_path_for_test(const gchar *path)
{
    g_mutex_lock(&G_unit_env_mu);
    g_free(G_unit_env_path_test);
    G_unit_env_path_test = g_strdup(path);
    g_mutex_unlock(&G_unit_env_mu);
}
