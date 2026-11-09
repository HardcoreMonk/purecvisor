                                                                             
                                     
                                                                                        
  
                           
                                                                                      
                                                                                                   
                                                                              
                                                                                                      
                                                                                 
                                                                                           
                                                                          
                                                                                                         
                                                                                                        
                                                                                                              
                                                                                   
                                                                         
                                                                            
  
                                                                            
                                                    
                                                                                

#include "lxc_volume_guard.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <gio/gio.h>
#include <linux/openat2.h>

#define VOLUME_LINK_MSG "host_path must not traverse symbolic links"
#define VOLGEN_RESET_FAILED "(volume guard fingerprint could not be reset)"                                            

                                                                                       
                                                                                                   
                                                                    
static void
_walk_error(GError **error, int e)
{
    if (e == ELOOP)
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, VOLUME_LINK_MSG);
    else if (e == ENOENT || e == ENOTDIR)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "host_path does not exist");
    else if (e == ENOSYS)                                            
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT, "kernel lacks openat2");
    else
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "walk host_path: %s", g_strerror(e));
}

int
pcv_lxc_volume_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    if (canon_out)
        *canon_out = NULL;
    if (!host_path || host_path[0] != '/') {                                 
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, "host_path must be an absolute path");
        return -1;
    }
                                                                            
                                                       
    gchar **parts = g_strsplit(host_path, "/", -1);
    GPtrArray *comps = g_ptr_array_new();
    gboolean dotdot = FALSE, dir_tail = FALSE;                                                                  
    for (gchar **p = parts; *p; p++) {
        if (!**p || strcmp(*p, ".") == 0) {
            dir_tail = comps->len > 0;
            continue;
        }
        dotdot = dotdot || strcmp(*p, "..") == 0;
        dir_tail = FALSE;
        g_ptr_array_add(comps, *p);
    }
    int cur = -1;
    if (dotdot)
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, VOLUME_LINK_MSG);
    else if ((cur = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0)
        _walk_error(error, errno);
    GString *canon = g_string_new(NULL);
    for (guint i = 0; cur >= 0 && i < comps->len; i++) {
                                                                                   
        struct open_how how = {
            .flags   = O_PATH | O_CLOEXEC | (i + 1 < comps->len || dir_tail ? O_DIRECTORY : 0),
            .resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
        };
        int nx = (int)syscall(SYS_openat2, cur, (const gchar *)comps->pdata[i], &how, sizeof how);
        int e = errno;
        close(cur);
        cur = nx;
        if (nx < 0)
            _walk_error(error, e);
        else
            g_string_append_printf(canon, "/%s", (const gchar *)comps->pdata[i]);
    }
    g_ptr_array_free(comps, TRUE);
    g_strfreev(parts);
    if (cur < 0) {
        g_string_free(canon, TRUE);
        return -1;
    }
    if (canon->len == 0)                          
        g_string_append_c(canon, '/');
    if (canon_out)
        *canon_out = g_string_free(canon, FALSE);
    else
        g_string_free(canon, TRUE);
    return cur;
}

                                                                              

                                                                                        
                              
static void
_mount_entry_free(gpointer data)
{
    PcvLxcMountEntry *e = data;
    g_free(e->source);
    g_free(e->dest);
    g_free(e->options);
    g_free(e);
}

                                                                                                    
                                                                                            
                                                 
static gboolean
_options_have_bind(const gchar *options)
{
    gchar **items = g_strsplit(options, ",", -1);
    gboolean bind = FALSE;
    for (gchar **i = items; *i && !bind; i++)
        bind = strcmp(*i, "bind") == 0 || strcmp(*i, "rbind") == 0;
    g_strfreev(items);
    return bind;
}

                                                                                                         
                                                                                                           
                                                                           
                                                      
static gboolean
_config_span(const gchar *line, const gchar *key, gsize *start, gsize *len, gboolean *quote_open)
{
    gsize kl = strlen(key);
    const gchar *p = line + strspn(line, " \t");
    if (strncmp(p, key, kl) != 0)                                           
        return FALSE;
    p += kl;
    p += strspn(p, " \t");
    if (*p != '=')                                                             
        return FALSE;
    p++;
    p += strspn(p, " \t");
    gsize n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    if (quote_open)
        *quote_open = n > 0 && (p[0] == '"' || p[0] == '\'') && !(n > 1 && p[n - 1] == p[0]);
    if (n > 1 && (p[0] == '"' || p[0] == '\'') && p[n - 1] == p[0]) {                                 
        p++;
        n -= 2;
    }
    *start = (gsize)(p - line);
    *len = n;
    return TRUE;
}

                                                                                                   
                           
                                                              
static gchar *
_config_value(const gchar *line, const gchar *key)
{
    gsize start, len;
    return _config_span(line, key, &start, &len, NULL) ? g_strndup(line + start, len) : NULL;
}

                                                                                                     
                                                                                             
                                                          
                                          
static PcvLxcMountEntry *
_mount_entry_parse(gchar *value)
{
    PcvLxcMountEntry *e = NULL;
    FILE *f = fmemopen(value, strlen(value), "r");
    struct mntent m;
    char buf[PATH_MAX];
    if (f && getmntent_r(f, &m, buf, sizeof buf) && m.mnt_fsname[0] && m.mnt_dir[0]) {
        e = g_new0(PcvLxcMountEntry, 1);
        e->source  = g_strdup(m.mnt_fsname);
        e->dest    = g_strdup(m.mnt_dir);
        e->options = g_strdup(m.mnt_opts);
        e->is_bind = _options_have_bind(e->options);
    }
    if (f)
        fclose(f);
    return e;
}

                                                   
                                       
gboolean
pcv_lxc_mount_entry_is_volume(const PcvLxcMountEntry *e)
{
    return e && e->is_bind && e->source[0] == '/';
}

                                                                                       
                                                      
GPtrArray *
pcv_lxc_config_mount_entries(const gchar *config_text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(_mount_entry_free);
    gchar **lines = g_strsplit_set(config_text ? config_text : "", "\r\n", -1);                                    
    for (gchar **l = lines; *l; l++) {
        gchar *value = _config_value(*l, "lxc.mount.entry");
        PcvLxcMountEntry *e = value && *value ? _mount_entry_parse(value) : NULL;                       
        if (e)
            g_ptr_array_add(out, e);
        g_free(value);
    }
    g_strfreev(lines);
    return out;
}

                                                                                                     
                                                                                       
                                                                               
                                                                            
static gboolean
_guard_text(const gchar *text, gchar **bad_container_path_out)
{
    GPtrArray *entries = pcv_lxc_config_mount_entries(text);
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < entries->len; i++) {
        const PcvLxcMountEntry *e = entries->pdata[i];
        if (!pcv_lxc_mount_entry_is_volume(e))                                                    
            continue;
        GError *err = NULL;
        int fd = pcv_lxc_volume_source_open(e->source, NULL, &err);
        if (fd >= 0)
            close(fd);
                                                                                        
        ok = !g_error_matches(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
        if (!ok && bad_container_path_out)
            *bad_container_path_out = g_strconcat("/", e->dest + strspn(e->dest, "/"), NULL);
        g_clear_error(&err);
    }
    g_ptr_array_unref(entries);
    return ok;
}

                                                                            
                                                          
gboolean
pcv_lxc_volume_guard_check(const gchar *lxc_path, const gchar *name, gchar **bad_container_path_out)
{
    if (bad_container_path_out)
        *bad_container_path_out = NULL;
    if (!lxc_path || !name)
        return TRUE;                                          
    gchar *path = g_build_filename(lxc_path, name, "config", NULL);
    gchar *text = NULL;
    gboolean have = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!have)                                                                            
        return TRUE;
    gboolean ok = _guard_text(text, bad_container_path_out);
    g_free(text);
    return ok;
}

                                                               
                                         
gchar *
pcv_lxc_volume_guard_message(const gchar *container_path)
{
    return g_strdup_printf("volume source changed since attach: %s", container_path ? container_path : "");
}

                                                                                               
                                                                               
                                                                                         
GQuark
pcv_lxc_mount_error_quark(void)
{
    return g_quark_from_static_string("pcv-lxc-mount-error-quark");
}

                                                                                

                                                                              
                                                                                                          
                                                             
static gboolean
_hook_active(gchar **lines)
{
    gboolean active = FALSE;
    for (gchar **l = lines; *l; l++) {
        gchar *v = _config_value(*l, PCV_LXC_VOLUME_GUARD_HOOK_KEY);
        gchar *all = v ? NULL : _config_value(*l, "lxc.hook");
        if (v)
            active = *v ? active || strcmp(v, PCV_LXC_VOLUME_GUARD_HOOK_VALUE) == 0 : FALSE;
        else if (all && !*all)
            active = FALSE;
        g_free(v);
        g_free(all);
    }
    return active;
}

                                                                                  
                                                           
gchar *
pcv_lxc_config_with_guard_hook(const gchar *config_text)
{
    const gchar *text = config_text ? config_text : "";
    GPtrArray *entries = pcv_lxc_config_mount_entries(text);
    gboolean bind = FALSE;
    for (guint i = 0; !bind && i < entries->len; i++) {
        const PcvLxcMountEntry *e = entries->pdata[i];
        bind = pcv_lxc_mount_entry_is_volume(e);                                      
    }
    g_ptr_array_unref(entries);
    gchar **lines = g_strsplit_set(text, "\r\n", -1);                                    
    gboolean have = _hook_active(lines);
    g_strfreev(lines);
    if (!bind || have)
        return NULL;
    gsize n = strlen(text);
    gboolean open_line = n > 0 && text[n - 1] != '\n' && text[n - 1] != '\r';                              
    return g_strconcat(text, open_line ? "\n" : "",
                       PCV_LXC_VOLUME_GUARD_HOOK_KEY " = " PCV_LXC_VOLUME_GUARD_HOOK_VALUE "\n", NULL);
}

                                                                           
                                                     
gboolean
pcv_lxc_volume_guard_hook_installed(const gchar *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && (st.st_mode & S_IXUSR) != 0;
}

                                                                                           
                                             
static gboolean
_lxc_name_ok(const gchar *name)
{
    return name && *name && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

                                                                    
                                                                                            
                                                                                           
int
pcv_lxc_volume_guard_hook_run_with_root(const gchar *volstage_root, const gchar *config_path, const gchar *lxc_name)
{
    gchar *text = NULL;
    if (!config_path || !*config_path || !g_file_get_contents(config_path, &text, NULL, NULL)) {
                                                                          
        g_printerr("pcv volume guard: cannot read the container config (LXC_CONFIG_FILE)\n");
        return 1;
    }
    if (!_lxc_name_ok(lxc_name)) {
        g_printerr("pcv volume guard: cannot read the container name (LXC_NAME)\n");
        g_free(text);
        return 1;
    }
                                                                                                 
    gchar *prefix = g_strconcat(volstage_root, "/", lxc_name, "/", NULL);
    GPtrArray *entries = pcv_lxc_config_mount_entries(text);
    gboolean inside = TRUE;
    gchar *relative_dest = NULL;
    for (guint i = 0; inside && i < entries->len; i++) {
        const PcvLxcMountEntry *e = entries->pdata[i];
        if (e->is_bind && e->source[0] != '/') {                                                                    
            relative_dest = g_strdup(e->dest);
            inside = FALSE;
        } else {
            inside = !pcv_lxc_mount_entry_is_volume(e) || g_str_has_prefix(e->source, prefix);
        }
    }
    g_ptr_array_unref(entries);
    g_free(prefix);
    if (relative_dest) {                                         
        g_printerr("pcv volume guard: bind mount with a relative source is not allowed: %s\n", relative_dest);
        g_free(relative_dest);
        g_free(text);
        return 1;
    }
    if (!inside) {
        g_printerr("pcv volume guard: volume source is outside the purecvisor staging area — start it through purecvisor\n");
        g_free(text);
        return 1;
    }
    gchar *bad = NULL;
    gboolean ok = _guard_text(text, &bad);                                           
    g_free(text);
    if (!ok) {
        gchar *msg = pcv_lxc_volume_guard_message(bad);                             
        g_printerr("pcv volume guard: %s\n", msg);
        g_free(msg);
    }
    g_free(bad);
    return ok ? 0 : 1;
}

                                                                                                      
                                                                  
int
pcv_lxc_volume_guard_hook_main(const gchar *config_path, const gchar *lxc_name)
{
    return pcv_lxc_volume_guard_hook_run_with_root(PCV_LXC_VOLSTAGE_ROOT, config_path, lxc_name);
}

                                                                                             
                                                                                   

                                                                                                                      
                                                                              
                                               
gchar *
pcv_lxc_volume_dest_canon(const gchar *dest)
{
    GString *s = g_string_new(NULL);
    gchar **parts = g_strsplit(dest ? dest : "", "/", -1);
    for (gchar **p = parts; *p; p++) {
        if (!**p || g_str_equal(*p, "."))
            continue;                                                                
        if (s->len)
            g_string_append_c(s, '/');
        g_string_append(s, *p);
    }
    g_strfreev(parts);
    return g_string_free(s, FALSE);
}

                                                        
                                                             
gchar *
pcv_lxc_volstage_slot(const gchar *volstage_root, const gchar *name, const gchar *dest, guint ordinal)
{
    gchar *trim = pcv_lxc_volume_dest_canon(dest);
    gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, trim, -1);                    
    gchar *slot = g_strdup_printf("%s/%s/%.16s.%u", volstage_root, name, hex, ordinal);
    g_free(trim);
    g_free(hex);
    return slot;
}

                                                                                                           
                                                           
static void
_escape_field(GString *out, const gchar *v)
{
    for (const gchar *p = v; *p; p++) {
        if (*p == ' ')
            g_string_append(out, "\\040");
        else if (*p == '\t')
            g_string_append(out, "\\011");
        else if (*p == '\n')
            g_string_append(out, "\\012");
        else if (*p == '\\')
            g_string_append(out, "\\134");
        else
            g_string_append_c(out, *p);
    }
}

                                                                                      
                                      
static void
_volstage_line_free(gpointer data)
{
    PcvLxcVolstageLine *l = data;
    g_free(l->source);
    g_free(l->dest);
    g_free(l->container_path);
    g_free(l->slot);
    g_free(l);
}

                                                                                              
                                                                                               
                                                         
static PcvLxcVolstageLine *
_volstage_line_new(const PcvLxcMountEntry *e, gchar *slot)
{
    PcvLxcVolstageLine *l = g_new0(PcvLxcVolstageLine, 1);
    l->source = g_strdup(e->source);
    l->dest = g_strdup(e->dest);
    l->container_path = g_strconcat("/", e->dest + strspn(e->dest, "/"), NULL);                      
    l->slot = slot;
    gchar **items = g_strsplit(e->options, ",", -1);
    for (gchar **i = items; *i; i++) {
        if (strcmp(*i, "ro") == 0)
            l->readonly = TRUE;
        else if (strcmp(*i, "rw") == 0)
            l->readonly = FALSE;
        else if (strcmp(*i, "rbind") == 0)
            l->recursive = TRUE;
    }
    g_strfreev(items);
    return l;
}

                                                                                                      
                                                                                                 
                                                                                         
                        
                                                                       
static const gchar *
_runtime_line(GString *out, const gchar *line, const gchar *root, const gchar *name, GHashTable *ordinals, GPtrArray *lines)
{
    gsize st = 0, len = 0;
    gboolean quote_open = FALSE;
    if (!_config_span(line, "lxc.mount.entry", &st, &len, &quote_open) || len == 0) {                    
        g_string_append(out, line);
        return NULL;
    }
    if (quote_open)                                                         
        return "unterminated quote in a lxc.mount.entry value";
    gchar *value = g_strndup(line + st, len);
    PcvLxcMountEntry *e = _mount_entry_parse(value);
    if (!pcv_lxc_mount_entry_is_volume(e)) {
        if (e)
            _mount_entry_free(e);
        g_free(value);
        g_string_append(out, line);
        return NULL;
    }
    gsize f0 = 0;
    while (value[f0] && g_ascii_isspace(value[f0]))                                
        f0++;
    gsize f1 = f0 + strcspn(value + f0, " \t");                               
    gchar *key = pcv_lxc_volume_dest_canon(e->dest);
    guint ord = GPOINTER_TO_UINT(g_hash_table_lookup(ordinals, key));
    g_hash_table_insert(ordinals, key, GUINT_TO_POINTER(ord + 1));                        
    gchar *slot = pcv_lxc_volstage_slot(root, name, e->dest, ord);
    g_string_append_len(out, line, (gssize)(st + f0));
    _escape_field(out, slot);
    g_string_append(out, line + st + f1);
    g_ptr_array_add(lines, _volstage_line_new(e, slot));                          
    _mount_entry_free(e);
    g_free(value);
    return NULL;
}

                                                                                                 
                                                                                                        
                                                                              
                          
static gchar *
_runtime_build(const gchar *config_text, const gchar *volstage_root, const gchar *name, GPtrArray **lines_out, GError **error)
{
    const gchar *text = config_text ? config_text : "";
    if (!volstage_root || !*volstage_root || !_lxc_name_ok(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "invalid container name or staging root for the runtime config");
        return NULL;
    }
    GString *out = g_string_new(NULL);
    GHashTable *ordinals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *lines = g_ptr_array_new_with_free_func(_volstage_line_free);
    const gchar *why = NULL;
    gsize n = strlen(text), i = 0;
    while (!why && i < n) {                                                    
        gsize j = i + strcspn(text + i, "\r\n");
        gchar *line = g_strndup(text + i, j - i);
        why = _runtime_line(out, line, volstage_root, name, ordinals, lines);
        g_free(line);
        if (!why && j < n)
            g_string_append_c(out, text[j]);
        i = j + 1;
    }
    gchar *result = g_string_free(out, FALSE);
    if (!why) {                                                          
        GPtrArray *eo = pcv_lxc_config_mount_entries(text), *er = pcv_lxc_config_mount_entries(result);
        guint v = 0;
        if (eo->len != er->len)
            why = "the runtime config does not re-read like the persistent config";
        for (guint k = 0; !why && k < eo->len; k++) {
            const PcvLxcMountEntry *o = eo->pdata[k], *r = er->pdata[k];
            gboolean vol = pcv_lxc_mount_entry_is_volume(o);
            const gchar *want = vol ? (v < lines->len ? ((const PcvLxcVolstageLine *)lines->pdata[v])->slot : "") : o->source;
            if (strcmp(o->dest, r->dest) != 0 || strcmp(o->options, r->options) != 0 || o->is_bind != r->is_bind ||
                strcmp(want, r->source) != 0)
                why = "the runtime config does not re-read like the persistent config";
            v += vol;
        }
        if (!why && v != lines->len)
            why = "the runtime config does not re-read like the persistent config";
        g_ptr_array_unref(eo);
        g_ptr_array_unref(er);
    }
    g_hash_table_unref(ordinals);
    if (why) {
        g_ptr_array_unref(lines);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, why);
        g_free(result);
        return NULL;
    }
    if (lines_out)
        *lines_out = lines;
    else
        g_ptr_array_unref(lines);
    return result;
}

                                                          
                                                        
gchar *
pcv_lxc_runtime_config_text(const gchar *config_text, const gchar *volstage_root, const gchar *name, GError **error)
{
    return _runtime_build(config_text, volstage_root, name, NULL, error);
}

                                                                      
                                                                        
GPtrArray *
pcv_lxc_volstage_lines(const gchar *config_text, const gchar *volstage_root, const gchar *name, GError **error)
{
    GPtrArray *lines = NULL;
    gchar *text = _runtime_build(config_text, volstage_root, name, &lines, error);
    g_free(text);
    return lines;
}

                                                     
                                                               
gboolean
pcv_lxc_config_root_gid(const gchar *config_text, gint64 *gid_out)
{
    gchar **lines = g_strsplit_set(config_text ? config_text : "", "\r\n", -1);
    gboolean found = FALSE;
    for (gchar **l = lines; !found && *l; l++) {
        gchar *value = _config_value(*l, "lxc.idmap");
        gchar **tok = value ? g_strsplit_set(value, " \t", -1) : NULL;
        GPtrArray *f = g_ptr_array_new();                           
        for (gchar **t = tok; t && *t; t++)
            if (**t)
                g_ptr_array_add(f, *t);
        guint64 cid = 0, start = 0, count = 0;
        if (f->len >= 4 && strcmp(f->pdata[0], "g") == 0 &&
            g_ascii_string_to_unsigned(f->pdata[1], 10, 0, G_MAXUINT32, &cid, NULL) &&
            g_ascii_string_to_unsigned(f->pdata[2], 10, 0, G_MAXUINT32, &start, NULL) &&
            g_ascii_string_to_unsigned(f->pdata[3], 10, 0, G_MAXUINT32, &count, NULL) && cid == 0 && count > 0) {
            if (gid_out)
                *gid_out = (gint64)start;
            found = TRUE;
        }
        g_ptr_array_free(f, TRUE);
        g_strfreev(tok);
        g_free(value);
    }
    g_strfreev(lines);
    return found;
}

                                                                                        
                                                                                                       
                                                        
static gboolean
_volgen_clear(const gchar *lxc_path, const gchar *name)
{
    gchar *dir = g_build_filename(lxc_path, name, NULL);
    gchar *path = g_build_filename(dir, PCV_LXC_VOLUME_GUARD_VOLGEN, NULL);
    struct stat st;
    gboolean ok;
    if (lstat(dir, &st) != 0)
        ok = errno == ENOENT;
    else if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        ok = FALSE;
    else
        ok = unlink(path) == 0 || errno == ENOENT;
    g_free(dir);
    g_free(path);
    return ok;
}

                                                                                   
                                                                  
gboolean
pcv_lxc_volume_guard_forget(const gchar *lxc_path, const gchar *name)
{
    return _volgen_clear(lxc_path, name);
}

                                                                  
                                                                                         
                                           
static gboolean
_volgen_refuse(const gchar *name, gchar **bad_container_path_out)
{
    g_warning("volume guard fingerprint for container '%s' could not be reset — refusing to start it", name);
    if (bad_container_path_out)
        *bad_container_path_out = g_strdup(VOLGEN_RESET_FAILED);
    return FALSE;
}

                                                                                                
                                                               
gboolean
pcv_lxc_volume_guard_check_start(const gchar *lxc_path, const gchar *name, gchar **bad_container_path_out)
{
    if (bad_container_path_out)
        *bad_container_path_out = NULL;
    if (lxc_path && name && !_volgen_clear(lxc_path, name))                                         
        return _volgen_refuse(name, bad_container_path_out);
    return pcv_lxc_volume_guard_check(lxc_path, name, bad_container_path_out);
}
