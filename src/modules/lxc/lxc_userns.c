                                                                             
                               
                                                                                       
  
                           
                                                                                          
                                                                           
                                                                                
                                                                         
                                                            
  
                                                             
                                       
                                                                                

#include "lxc_userns.h"
#include "lxc_mountinfo.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <linux/openat2.h>

#define USERNS_LOG_DOM "lxc_userns"

                                                                                                          
                          
#define SUBID_LINE_MAX 1024

                                                                                  
                                                                                                
                                                                                 
                                                 
                                                                     
static gboolean
_parse_subid_num(const gchar *s, guint64 min, guint64 max, guint64 *out)
{
    while (*s == ' ' || (*s >= '\t' && *s <= '\r'))                           
        s++;
    gboolean neg = (*s == '-');
    if (*s == '+' || *s == '-')
        s++;
    if (!g_ascii_isdigit(*s))                                                   
        return FALSE;
    gchar *end = NULL;
    errno = 0;
    guint64 v = g_ascii_strtoull(s, &end, 0);
    if (errno != 0 || *end != '\0' || (neg && v != 0) || v < min || v > max)
        return FALSE;
    *out = v;
    return TRUE;
}

                                                                                        
                                                                        
                                                                                      
                                                                       
                                                                           
                             
                                                                            
static gboolean
_subid_root_range(const gchar *raw, guint64 *start, guint64 *count, const gchar **why)
{
    gsize len = strlen(raw);
    *why = NULL;
    if (len >= SUBID_LINE_MAX)
        *why = "line is 1024 characters or longer";
    else if (len == 0 || g_ascii_isspace(raw[0]) || g_ascii_isspace(raw[len - 1]))
        *why = "leading or trailing whitespace or CR (CRLF file?)";
    if (*why)
        return FALSE;
    g_auto(GStrv) f = g_strsplit(raw, ":", -1);
    if (g_strv_length(f) != 3 || !(g_str_equal(f[0], "root") || g_str_equal(f[0], "0")))
        *why = "needs exactly 3 fields name:first:size";
    else if (!_parse_subid_num(f[1], 0, G_MAXUINT32, start))
        *why = "start is not a number in 0-4294967295";
    else if (!_parse_subid_num(f[2], 1, MIN((G_GUINT64_CONSTANT(1) << 32) - *start, G_MAXUINT32), count))
        *why = "count is 0, not a number or past the 32-bit id limit";
    return *why == NULL;
}

                                                                          
                                                                          
                                                                                          
                                                                             
                                                    
                                                                   
                                                                                        
static void
_subid_file_check(const gchar *path, gint64 start, gint64 count, gboolean *root_covers, gboolean *overlap)
{
    *root_covers = FALSE;
    *overlap = FALSE;
    g_autofree gchar *contents = NULL;
    if (!path || !g_file_get_contents(path, &contents, NULL, NULL))
        return;
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        g_autofree gchar *copy = g_strstrip(g_strdup(lines[i]));
        if (!*copy || copy[0] == '#')
            continue;
        g_auto(GStrv) f = g_strsplit(copy, ":", 4);                                                
        guint64 ls = 0, lc = 0;
        if (g_str_equal(f[0], "root") || g_str_equal(f[0], "0")) {
                                                                                    
                                                                              
            const gchar *why = NULL;
            if (!_subid_root_range(lines[i], &ls, &lc, &why))
                PCV_LOG_WARN(USERNS_LOG_DOM, "%s:%d: root line ignored: %s — newuidmap may refuse it, so it does "
                             "not delegate a range", path, i + 1, why);
            else if (ls <= (guint64)start && ls + lc >= (guint64)(start + count))
                *root_covers = TRUE;
            continue;
        }
        if (g_strv_length(f) < 3 || !*f[0])
            continue;
        if (!_parse_subid_num(f[1], 0, G_MAXUINT64, &ls) || !_parse_subid_num(f[2], 0, G_MAXUINT64, &lc))
            continue;
        guint64 last = ls + lc - 1;                                                        
        if (last < ls)
            continue;                                                               
        if (ls <= (guint64)(start + count - 1) && last >= (guint64)start) {
            *overlap = TRUE;
            PCV_LOG_WARN(USERNS_LOG_DOM,
                         "%s:%d: account '%s' range %" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT
                         " overlaps the container range %" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                         path, i + 1, f[0], ls, lc, start, count);
        }
    }
}

PcvLxcUsernsHost
pcv_lxc_userns_host_check(const gchar *subuid_path, const gchar *subgid_path, gint64 start, gint64 count)
{
    if (count < PCV_LXC_USERNS_MIN_COUNT || start < count || start + count > PCV_LXC_USERNS_ID_LIMIT)
        return PCV_LXC_USERNS_BAD_RANGE;
    gboolean root_u, over_u, root_g, over_g;
    _subid_file_check(subuid_path, start, count, &root_u, &over_u);
    _subid_file_check(subgid_path, start, count, &root_g, &over_g);
    if (over_u || over_g)
        return PCV_LXC_USERNS_OVERLAP;                                                
    if (!root_u || !root_g)
        return PCV_LXC_USERNS_NO_ROOT_RANGE;
    return PCV_LXC_USERNS_READY;
}

gchar *
pcv_lxc_userns_host_message(PcvLxcUsernsHost cause, gint64 start, gint64 count)
{
    static const gchar pfx[] = "unprivileged containers are not available on this host: ";
    static const gchar admin[] = " Or ask an admin to create a privileged container (rootless:false).";
    switch (cause) {
    case PCV_LXC_USERNS_BAD_RANGE:
        return g_strdup_printf("%s[container] rootless_uid_start=%" G_GINT64_FORMAT " rootless_uid_count=%"
                               G_GINT64_FORMAT " is not a valid range (need count >= 65536, start >= count, "
                               "start+count <= 4294967295). Fix daemon.conf.%s", pfx, start, count, admin);
    case PCV_LXC_USERNS_OVERLAP:
        return g_strdup_printf("%sanother account's subordinate id range overlaps %" G_GINT64_FORMAT "-%"
                               G_GINT64_FORMAT ". Move that account's range or change [container] "
                               "rootless_uid_start/rootless_uid_count, then add 'root:%" G_GINT64_FORMAT ":%"
                               G_GINT64_FORMAT "' to /etc/subuid and /etc/subgid (if you changed rootless_uid_start "
                               "or rootless_uid_count, use the new values in that root line).%s",
                               pfx, start, start + count - 1, start, count, admin);
    case PCV_LXC_USERNS_NO_ROOT_RANGE:
        return g_strdup_printf("%s/etc/subuid and /etc/subgid must delegate %" G_GINT64_FORMAT ":%" G_GINT64_FORMAT
                               " to root. Add 'root:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT "' to both files.%s",
                               pfx, start, count, start, count, admin);
    case PCV_LXC_USERNS_READY:
    default:
        return g_strdup("unprivileged containers are available on this host");
    }
}

                                                                                
                                                              
gboolean
pcv_lxc_userns_parse_default(const gchar *raw, gboolean *unknown)
{
    if (unknown)
        *unknown = FALSE;
    if (!raw)
        return TRUE;
    g_autofree gchar *trimmed = g_strstrip(g_strdup(raw));
    g_autofree gchar *v = g_ascii_strdown(trimmed, -1);
    if (g_str_equal(v, "true") || g_str_equal(v, "1") || g_str_equal(v, "yes"))
        return TRUE;
    if (g_str_equal(v, "false") || g_str_equal(v, "0") || g_str_equal(v, "no"))
        return FALSE;
    if (unknown)
        *unknown = TRUE;
    return TRUE;
}

                                                                                             
                                                                                
                                                    
                                                                     
static gint64
_settings_id(const gchar *key, gint64 def)
{
    const gchar *raw = pcv_config_get_string("container", key, NULL);
    if (!raw)
        return def;
    g_autofree gchar *trimmed = g_strstrip(g_strdup(raw));
    gint64 v = 0;
    if (!g_ascii_string_to_signed(trimmed, 10, 0, PCV_LXC_USERNS_ID_LIMIT, &v, NULL)) {
        PCV_LOG_WARN(USERNS_LOG_DOM, "[container] %s='%s' is not an integer in 0-%" G_GINT64_FORMAT
                     " — unprivileged containers are refused until it is fixed", key, raw, PCV_LXC_USERNS_ID_LIMIT);
        return -1;
    }
    return v;
}

PcvLxcUsernsSettings
pcv_lxc_userns_settings_load(void)
{
    PcvLxcUsernsSettings s = {
        .start = _settings_id("rootless_uid_start", 100000),
        .count = _settings_id("rootless_uid_count", 65536),
    };
    const gchar *raw = pcv_config_get_string("container", "rootless", PCV_LXC_ROOTLESS_DEFAULT);
    s.default_on = pcv_lxc_userns_parse_default(raw, &s.default_unknown);
    if (s.default_unknown)
        PCV_LOG_WARN(USERNS_LOG_DOM, "[container] rootless='%s' is not true/false — treating it as true (unprivileged)", raw);
    return s;
}

                                                                                    
                                   
                                                                                          
                                                                                           
                                                                     
PcvLxcRootlessDecision
pcv_lxc_rootless_decide(gint requested, gboolean config_default, gboolean host_ready)
{
    if (requested == 0)
        return PCV_LXC_ROOTLESS_OFF;
    if (requested == 1 || config_default)
        return host_ready ? PCV_LXC_ROOTLESS_ON : PCV_LXC_ROOTLESS_REJECT;
    return PCV_LXC_ROOTLESS_OFF;
}

                                                                                      
                                                                    
                                                                   
static gboolean
_key_matches(const gchar *line, const gchar *key)
{
    const gchar *p = line + strspn(line, " \t");
    gsize len = strcspn(p, "= \t"), n = strlen(key);
    if (*p == '\0' || *p == '#')
        return FALSE;
    return key[n - 1] == '.' ? (len >= n && strncmp(p, key, n) == 0) : (len == n && strncmp(p, key, n) == 0);
}

gchar *
pcv_lxc_userns_resource_lines(const gchar *bridge, guint memory_mb, guint vcpu_count)
{
    if (!bridge || !*bridge || strpbrk(bridge, "\n\r=#"))
        return NULL;
    return g_strdup_printf("lxc.cgroup.memory.limit_in_bytes = %uM\nlxc.cgroup2.memory.max = %uM\n"
                           "lxc.cgroup2.cpu.max = %" G_GUINT64_FORMAT " 100000\nlxc.net.0.type = veth\nlxc.net.0.link = %s\n"
                           "lxc.net.0.flags = up\nlxc.net.0.hwaddr = 00:16:3e:xx:xx:xx\n",                                
                           memory_mb, memory_mb, (guint64)vcpu_count * 100000, bridge);                             
}

gchar *
pcv_lxc_userns_build_create_config(const gchar *default_text, gint64 start, gint64 count, const gchar *resource_text,
                                   gboolean *dropped_idmap)
{
    GString *out = g_string_new(NULL);
    gboolean dropped = FALSE;
    if (default_text) {
        g_auto(GStrv) lines = g_strsplit(default_text, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            if (_key_matches(lines[i], "lxc.idmap") || _key_matches(lines[i], "lxc.id_map")) {
                dropped = TRUE;                                                
                continue;
            }
            if (_key_matches(lines[i], "lxc.net."))
                continue;                                                                                 
            if (!lines[i + 1] && !*lines[i])
                break;                                
            g_string_append(out, lines[i]);
            g_string_append_c(out, '\n');
        }
    }
    if (resource_text && *resource_text) {
        g_string_append(out, resource_text);
        if (!g_str_has_suffix(resource_text, "\n"))
            g_string_append_c(out, '\n');
    }
    g_string_append_printf(out, "lxc.idmap = u 0 %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n", start, count);
    g_string_append_printf(out, "lxc.idmap = g 0 %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n", start, count);
    g_string_append(out, "lxc.init.uid = 0\nlxc.init.gid = 0\n");
    if (dropped_idmap)
        *dropped_idmap = dropped;
    return g_string_free(out, FALSE);
}

                                                                                               
static gboolean
_has_control(const gchar *s)
{
    for (const guchar *p = (const guchar *)s; p && *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return TRUE;
    return FALSE;
}

gchar *
pcv_lxc_userns_config_apply_edits(const gchar *text, const PcvLxcConfigEdit *edits, gsize n_edits, GError **error)
{
                                                                                            
    for (gsize i = 0; i < n_edits; i++) {
        const gchar *k = edits[i].key, *v = edits[i].value;
        if (!k || !*k || _has_control(k) || _has_control(v)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "config edit %" G_GSIZE_FORMAT ": key and value must be single non-empty lines without control characters", i);
            return NULL;
        }
    }
    g_auto(GStrv) split = g_strsplit(text ? text : "", "\n", -1);
    g_autoptr(GPtrArray) lines = g_ptr_array_new();                                               
    g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; split[i]; i++)
        if (split[i + 1] || *split[i])                                                           
            g_ptr_array_add(lines, split[i]);
    for (gsize e = 0; e < n_edits; e++) {
        for (guint i = lines->len; !edits[e].append && i-- > 0;)
            if (_key_matches(g_ptr_array_index(lines, i), edits[e].key))
                g_ptr_array_remove_index(lines, i);                                
        if (edits[e].value) {
            gchar *line = g_strdup_printf("%s = %s", edits[e].key, edits[e].value);
            g_ptr_array_add(added, line);
            g_ptr_array_add(lines, line);
        }
    }
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < lines->len; i++) {
        g_string_append(out, g_ptr_array_index(lines, i));
        g_string_append_c(out, '\n');
    }
    return g_string_free(out, FALSE);
}

gboolean
pcv_lxc_userns_idmap_complete(const gchar *idmap_text)
{
    gboolean u = FALSE, g = FALSE;
    if (!idmap_text)
        return FALSE;
    g_auto(GStrv) lines = g_strsplit(idmap_text, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        const gchar *p = lines[i] + strspn(lines[i], " \t");
        guint64 ns = 0, host = 0, range = 0;
        if ((p[0] != 'u' && p[0] != 'g') || (p[1] != ' ' && p[1] != '\t') ||
            sscanf(p + 2, "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT, &ns, &host, &range) != 3 ||
            range == 0)
            continue;
        if (p[0] == 'u') u = TRUE; else g = TRUE;
    }
    return u && g;
}

gboolean
pcv_lxc_userns_config_has_include(const gchar *config_text)
{
    if (!config_text)
        return FALSE;
    g_auto(GStrv) lines = g_strsplit(config_text, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        const gchar *p = lines[i] + strspn(lines[i], " \t");
        if (g_str_has_prefix(p, "lxc.include") && strstr(p, "userns.conf"))
            return TRUE;
    }
    return FALSE;
}

PcvLxcIdClass
pcv_lxc_userns_classify_id(gint64 id, gint64 start, gint64 count)
{
    if (id >= start && id < start + count)
        return PCV_LXC_ID_MAPPED;
    if (id >= 0 && id < count)
        return PCV_LXC_ID_UNSHIFTED;
    return PCV_LXC_ID_OUTSIDE;
}

gint64
pcv_lxc_userns_shift_id(gint64 id, gint64 start, gint64 count)
{
    return pcv_lxc_userns_classify_id(id, start, count) == PCV_LXC_ID_UNSHIFTED ? start + id : id;
}

gchar *
pcv_lxc_userns_getcap_text(const gchar *getcap_out, const gchar *path)
{
    if (!getcap_out || !path || !g_str_has_prefix(getcap_out, path))
        return NULL;
    const gchar *p = getcap_out + strlen(path);
    if (*p != ' ' && *p != '\t')
        return NULL;
    p += strspn(p, " \t");
    if (p[0] == '=' && (p[1] == ' ' || p[1] == '\t'))                           
        p += 1 + strspn(p + 1, " \t");
    gchar *text = g_strstrip(g_strdup(p));
    if (!*text || strchr(text, '\n')) {
        g_free(text);
        return NULL;
    }
    return text;
}

gboolean
pcv_lxc_mountinfo_is_readonly(const gchar *mountinfo_text, const gchar *mount_point, gboolean *found)
{
    gboolean hit = FALSE, ro = FALSE;
    if (mountinfo_text && mount_point && mount_point[0] == '/') {
                                                                           
        gchar **parts = g_strsplit(mount_point, "/", -1);
        GString *want = g_string_new(NULL);
        for (gchar **q = parts; *q; q++)
            if (**q && strcmp(*q, ".") != 0)
                g_string_append_printf(want, "/%s", *q);
        if (!want->len)
            g_string_append_c(want, '/');
        g_strfreev(parts);
                                                                                       
        gchar *copy = g_strdup(mountinfo_text);
        gsize cap = 1;
        for (const gchar *c = copy; *c; c++)
            if (*c == '\n')
                cap++;
        PcvMountinfoLine *lines = g_new(PcvMountinfoLine, cap);
        gsize n = pcv_mountinfo_parse(copy, lines, cap, NULL);
        for (gsize i = 0; i < n; i++) {
            if (strcmp(lines[i].mount_point, want->str) == 0) {                                     
                gchar **opts = g_strsplit(lines[i].options, ",", -1);
                hit = TRUE;
                ro = g_strv_contains((const gchar * const *)opts, "ro");
                g_strfreev(opts);
            }
        }
        g_free(lines);
        g_free(copy);
        g_string_free(want, TRUE);
    }
    if (found)
        *found = hit;
    return hit && ro;
}

                                                                                       
                                                                                       
                                         
                                               
static int
_openat2_dir(int dirfd, const gchar *name)
{
    struct open_how how = {
        .flags   = O_PATH | O_DIRECTORY | O_CLOEXEC,
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,
    };
    return (int)syscall(SYS_openat2, dirfd, name, &how, sizeof how);
}

                                                                                    

PcvLxcMountErrorClass
pcv_lxc_mount_error_classify(const GError *error)
{
    if (error && error->domain == PCV_LXC_MOUNT_ERROR) {
        if (error->code == PCV_LXC_MOUNT_ERROR_BAD_PATH)
            return PCV_LXC_MOUNT_CLASS_BAD_REQUEST;
        if (error->code == PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT)
            return PCV_LXC_MOUNT_CLASS_NO_KERNEL_SUPPORT;
    }
    return PCV_LXC_MOUNT_CLASS_INTERNAL;                                                                             
}

                                                                                      
                                                             
                                                     
static void
_set_mount_errno(GError **error, const gchar *what)
{
    int e = errno;
    if (e == ENOSYS)
        g_set_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT, "%s: %s", what, g_strerror(e));
    else
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", what, g_strerror(e));
}

                                                                                     
                                                                                                             
                                                                                                   
                                                         
                                                                             
gboolean
pcv_lxc_rootfs_mkdir_at(int dir_fd, const gchar *name, GError **error)
{
    if (mkdirat(dir_fd, name, 0755) < 0) {
        int e = errno;
        if (e == EEXIST)
            return TRUE;                                                           
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "create container mount target: %s", g_strerror(e));
        return FALSE;
    }
    struct open_how how = {
        .flags   = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,                                
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS,
    };
    int fd = (int)syscall(SYS_openat2, dir_fd, name, &how, sizeof how);
    int e = fd < 0 ? errno : 0;
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) < 0)
        e = errno;
    else if (fd >= 0 && st.st_uid == geteuid() && fchmod(fd, 0755) < 0)                                       
        e = errno;
    if (fd >= 0)
        close(fd);
    if (e == ELOOP)
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                            "container_path must not traverse symbolic links inside the container");
    else if (e != 0)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "set container mount target mode: %s", g_strerror(e));
    return e == 0;
}

int
pcv_lxc_rootfs_walk_dir(int rootfs_fd, const gchar *container_path, gboolean create, GError **error)
{
    if (rootfs_fd < 0 || !container_path || container_path[0] != '/') {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                            "container_path must be an absolute path");
        return -1;
    }
    int cur = openat(rootfs_fd, ".", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);                                
    if (cur < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "open rootfs: %s", g_strerror(errno));
        return -1;
    }
    gchar **parts = g_strsplit(container_path, "/", -1);
    gboolean ok = TRUE;
    for (gchar **p = parts; ok && *p; p++) {
        if (!**p || strcmp(*p, ".") == 0)
            continue;
        if (strcmp(*p, "..") == 0) {                                
            g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                                "container_path must not contain '..'");
            ok = FALSE;
            break;
        }
        int nx = _openat2_dir(cur, *p);
        if (nx < 0 && errno == ENOENT && create) {
            if (!pcv_lxc_rootfs_mkdir_at(cur, *p, error)) {                                               
                ok = FALSE;
                break;
            }
            nx = _openat2_dir(cur, *p);
        }
        if (nx < 0) {
            if (errno == ELOOP)
                g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                                    "container_path must not traverse symbolic links inside the container");
            else if (errno == ENOSYS)                                            
                g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT,
                                    "kernel lacks openat2");
            else if (errno == ENOENT)
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                    "container_path does not exist in the container");
            else
                g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                            "walk container_path: %s", g_strerror(errno));
            ok = FALSE;
            break;
        }
        close(cur);
        cur = nx;
    }
    g_strfreev(parts);
    if (!ok) {
        close(cur);
        return -1;
    }
    return cur;
}

                                                                                                                                                    
                                                                                                  
                                                                                                       
                                                                                       
                                                  
                                                               
int
pcv_lxc_rootfs_open_leaf(int rootfs_fd, const gchar *container_path, GError **error)
{
    const gchar *slash = container_path && container_path[0] == '/' ? strrchr(container_path, '/') : NULL;
    const gchar *leaf = slash ? slash + 1 : NULL;
    if (!leaf || !*leaf || g_str_equal(leaf, ".") || g_str_equal(leaf, "..")) {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                            "container_path must name an entry below the root");
        return -1;
    }
    g_autofree gchar *parent = slash == container_path ? g_strdup("/") : g_strndup(container_path, (gsize)(slash - container_path));
    int dir_fd = pcv_lxc_rootfs_walk_dir(rootfs_fd, parent, FALSE, error);                                     
    if (dir_fd < 0)
        return -1;
    struct open_how how = {
        .flags   = O_PATH | O_NOFOLLOW | O_CLOEXEC,                                                    
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
    };
    int fd = (int)syscall(SYS_openat2, dir_fd, leaf, &how, sizeof how);
    int e = errno;
    close(dir_fd);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) < 0) {
        e = errno;
        close(fd);
        fd = -1;
    } else if (fd >= 0 && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        close(fd);
        if (S_ISLNK(st.st_mode))
            g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                                "container_path must not traverse symbolic links inside the container");
        else
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                                "mount point is not a directory or regular file");
        return -1;
    }
    if (fd >= 0)
        return fd;
    if (e == ELOOP)
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH,
                            "container_path must not traverse symbolic links inside the container");
    else if (e == ENOSYS)
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT, "kernel lacks openat2");
    else if (e == ENOENT)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "container_path does not exist in the container");
    else
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "open mount point: %s", g_strerror(e));
    return -1;
}

                                                                                                                       
                                                                                                   
                                                                                                           
                                                                                                      
                                                                                        
                                                                                                    
                                                                               
                                                                                 
                                                
gboolean
pcv_lxc_userns_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error)
{
    const unsigned int rec = (flags & PCV_LXC_MOUNT_RECURSIVE) ? AT_RECURSIVE : 0;
    int tree = open_tree(src_fd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH | rec);                         
    if (tree < 0) {
        _set_mount_errno(error, "open_tree");
        return FALSE;
    }
    gboolean ok = TRUE;
    if (flags & PCV_LXC_MOUNT_RO) {                                
        struct mount_attr ro = { .attr_set = MOUNT_ATTR_RDONLY };
        if (mount_setattr(tree, "", AT_EMPTY_PATH | rec, &ro, sizeof ro) < 0) {
            _set_mount_errno(error, "mount_setattr(MOUNT_ATTR_RDONLY)");
            ok = FALSE;
        }
    }
    if (ok && mnt_id_out) {                                                      
        struct statx stx;
        if (statx(tree, "", AT_EMPTY_PATH, STATX_MNT_ID, &stx) < 0) {
            _set_mount_errno(error, "statx(STATX_MNT_ID)");
            ok = FALSE;
        } else if (!(stx.stx_mask & STATX_MNT_ID)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "statx(STATX_MNT_ID): mount id not reported");
            ok = FALSE;
        } else {
            *mnt_id_out = stx.stx_mnt_id;
        }
    }
    if (ok && move_mount(tree, "", target_fd, "", MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) < 0) {
        _set_mount_errno(error, "move_mount");
        ok = FALSE;
    } else if (ok) {
        struct mount_attr slave = { .propagation = MS_SLAVE };
        if (mount_setattr(tree, "", AT_EMPTY_PATH | rec, &slave, sizeof slave) < 0) {
            _set_mount_errno(error, "mount_setattr(MS_SLAVE)");
                                                                           
            struct mount_attr priv = { .propagation = MS_PRIVATE };
            if (mount_setattr(tree, "", AT_EMPTY_PATH | AT_RECURSIVE, &priv, sizeof priv) < 0)
                PCV_LOG_WARN(USERNS_LOG_DOM, "volume mount: cannot make the failed mount private: %s", g_strerror(errno));
            gchar *path = g_strdup_printf("/proc/self/fd/%d", tree);
            if (umount2(path, MNT_DETACH) < 0)
                PCV_LOG_WARN(USERNS_LOG_DOM, "volume mount: cannot remove the mount after the slave switch failed: %s",
                             g_strerror(errno));
            g_free(path);
            ok = FALSE;
        }
    }
    close(tree);
    return ok;
}

                                                                                                        
                                                                                                                         
                                                      
static gboolean
_mount_entry_dest_is(const gchar *line, const gchar *dest)
{
    static const char key[] = "lxc.mount.entry";
    const gchar *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, key, sizeof key - 1) != 0)                              
        return FALSE;
    p += sizeof key - 1;
    if (*p != ' ' && *p != '\t' && *p != '=')                                 
        return FALSE;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return FALSE;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && *p != '\t')                      
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    const gchar *b = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    g_autofree gchar *field2 = g_strndup(b, (gsize)(p - b));
    return pcv_vol_dest_equal(field2, dest);                                          
}

gchar *
pcv_lxc_config_remove_mount_entry(const gchar *text, const gchar *dest, guint *removed)
{
    if (removed)
        *removed = 0;
    if (!text || !dest)
        return NULL;
    const gboolean empty = pcv_vol_dest_equal(dest, "");                                        
    GString *out = g_string_new(NULL);
    guint dropped = 0;
    for (const gchar *p = text; *p;) {
                                                                                             
        gsize len = strcspn(p, "\r\n");
        gsize end = p[len] == '\r' && p[len + 1] == '\n' ? 2 : p[len] ? 1 : 0;
        gchar *line = g_strndup(p, len);
        if (!empty && _mount_entry_dest_is(line, dest))                            
            dropped++;                                                               
        else
            g_string_append_len(out, p, (gssize)(len + end));                           
        g_free(line);
        p += len + end;
    }
    if (removed)
        *removed = dropped;
    return g_string_free(out, FALSE);
}

                                                                             
                                                 
                                                                               
                                                                                      
                                                                                         
                                                              
                                                                                

gchar *
pcv_lxc_userns_write_create_config(const gchar *default_conf_path, gint64 start, gint64 count,
                                   const gchar *resource_text, GError **error)
{
    g_autofree gchar *base = NULL;
    GError *read_err = NULL;
    if (default_conf_path && !g_file_get_contents(default_conf_path, &base, NULL, &read_err)) {
        if (!g_error_matches(read_err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_propagate_error(error, read_err);
            return NULL;
        }
        g_clear_error(&read_err);                                             
    }
    gboolean dropped = FALSE;
    g_autofree gchar *text = pcv_lxc_userns_build_create_config(base, start, count, resource_text, &dropped);
    if (dropped)
        PCV_LOG_WARN(USERNS_LOG_DOM, "%s has lxc.idmap lines — replaced by [container] range %" G_GINT64_FORMAT ":%"
                     G_GINT64_FORMAT, default_conf_path, start, count);
    gchar *dir = g_dir_make_tmp("pcv-lxc-userns-XXXXXX", error);                                            
    if (!dir)
        return NULL;
    g_autofree gchar *path = g_build_filename(dir, "config", NULL);
    if (!g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
        pcv_lxc_userns_remove_create_config(dir);
        g_free(dir);
        return NULL;
    }
    return dir;
}

void
pcv_lxc_userns_remove_create_config(const gchar *dir)
{
    if (!dir)
        return;
    g_autofree gchar *path = g_build_filename(dir, "config", NULL);
    (void)g_unlink(path);
    if (g_rmdir(dir) != 0)
        PCV_LOG_WARN(USERNS_LOG_DOM, "cannot remove temporary idmap config dir %s: %s", dir, g_strerror(errno));
}

                                                                                             
                                                
static gboolean
_owned_plain(const GStatBuf *st, mode_t type)
{
    return (st->st_mode & S_IFMT) == type && st->st_uid == geteuid() && (type != S_IFREG || st->st_nlink == 1);
}

                                                                     
                                                                                
GMutex *
pcv_lxc_config_mutex(void)
{
                                                                                                                 
                                                                                                   
                                                                                 
                                                                                          
                                                                                
    static GMutex mutex;                                                          
    return &mutex;
}

gboolean
pcv_lxc_userns_config_rewrite(const gchar *lxc_path, const gchar *name, PcvLxcConfigRewriteFunc rewrite, gconstpointer data,
                              GError **error)
{
    g_autofree gchar *dir = g_build_filename(lxc_path, name, NULL);
    g_autofree gchar *path = g_build_filename(dir, "config", NULL);
    GStatBuf st;
    if (g_lstat(dir, &st) != 0 || !_owned_plain(&st, S_IFDIR) || g_lstat(path, &st) != 0 || !_owned_plain(&st, S_IFREG)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "refusing to rewrite %s: the directory and the file "
                    "must be non-link, owned by uid %u, and the file must have a single link", path, (guint)geteuid());
        return FALSE;
    }
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, error))
        return FALSE;
    GError *rw_err = NULL;
    g_autofree gchar *out = rewrite(text, data, &rw_err);
    if (!out) {                                             
        gboolean unchanged = rw_err == NULL;
        if (rw_err)
            g_propagate_error(error, rw_err);                                             
        return unchanged;
    }
                                                                                     
                                                                                       
    return g_file_set_contents_full(path, out, -1, G_FILE_SET_CONTENTS_CONSISTENT, st.st_mode & 0777, error);
}

                                                                    
                                                      
typedef struct {
    const PcvLxcConfigEdit *edits;
    gsize                   n_edits;
} ConfigEdits;

                                                                                               
static gchar *
_apply_edits_rewrite(const gchar *text, gconstpointer data, GError **error)
{
    const ConfigEdits *e = data;
    return pcv_lxc_userns_config_apply_edits(text, e->edits, e->n_edits, error);                           
}

gboolean
pcv_lxc_userns_config_edit(const gchar *lxc_path, const gchar *name, const PcvLxcConfigEdit *edits, gsize n_edits,
                           GError **error)
{
    const ConfigEdits e = { edits, n_edits };
    return pcv_lxc_userns_config_rewrite(lxc_path, name, _apply_edits_rewrite, &e, error);
}

                                                                 
                                    
static void
_scan_file_free(gpointer p)
{
    PcvLxcUsernsFile *f = p;
    g_free(f->path);
    g_free(f);
}

                                                                    
static thread_local PcvLxcUsernsScan *tl_scan;
static thread_local gint64 tl_start, tl_count;

                                                                     
                                                                    
                                                           
                                            
static int
_scan_cb(const char *path, const struct stat *st, int type, struct FTW *ftw G_GNUC_UNUSED)
{
    PcvLxcUsernsScan *s = tl_scan;
    if (type == FTW_NS || type == FTW_DNR) {
        s->unreadable++;
        return 0;
    }
    PcvLxcIdClass cu = pcv_lxc_userns_classify_id(st->st_uid, tl_start, tl_count);
    PcvLxcIdClass cg = pcv_lxc_userns_classify_id(st->st_gid, tl_start, tl_count);
    if (cu == PCV_LXC_ID_OUTSIDE || cg == PCV_LXC_ID_OUTSIDE) {
        s->outside++;
        if (!s->first_outside)
            s->first_outside = g_strdup_printf("%s (uid %u gid %u)", path, (guint)st->st_uid, (guint)st->st_gid);
    } else if (cu == PCV_LXC_ID_UNSHIFTED || cg == PCV_LXC_ID_UNSHIFTED) {
        s->unshifted++;
                                                                      
        gint64 key = (gint64)(((guint64)st->st_uid << 32) | (guint64)st->st_gid);
        if (!g_hash_table_contains(s->pairs, &key))
            g_hash_table_add(s->pairs, g_memdup2(&key, sizeof key));
        if (S_ISREG(st->st_mode) && (st->st_mode & (S_ISUID | S_ISGID))) {
            PcvLxcUsernsFile *f = g_new0(PcvLxcUsernsFile, 1);
            f->path = g_strdup(path);
            f->mode = st->st_mode & 07777;                                                        
            g_ptr_array_add(s->suid_files, f);
        }
        if (S_ISREG(st->st_mode) && lgetxattr(path, "security.capability", NULL, 0) > 0)
            g_ptr_array_add(s->cap_files, g_strdup(path));
    } else {
        s->mapped++;
    }
    if (!S_ISLNK(st->st_mode) &&
        (lgetxattr(path, "system.posix_acl_access", NULL, 0) >= 0 ||
         (S_ISDIR(st->st_mode) && lgetxattr(path, "system.posix_acl_default", NULL, 0) >= 0)))
        s->acl++;
    return 0;
}

                                                                       
                                             
gboolean
pcv_lxc_userns_scan(const gchar *rootfs, gint64 start, gint64 count, PcvLxcUsernsScan *out, GError **error)
{
    *out = (PcvLxcUsernsScan){
        .pairs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL),
        .suid_files = g_ptr_array_new_with_free_func(_scan_file_free),
        .cap_files = g_ptr_array_new_with_free_func(g_free),
    };
    tl_scan = out;
    tl_start = start;
    tl_count = count;
    int rc = nftw(rootfs, _scan_cb, 32, FTW_PHYS | FTW_MOUNT);                                   
    int saved = errno;
    tl_scan = NULL;
    if (rc != 0 || out->unreadable > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot scan %s (%s, %" G_GUINT64_FORMAT " unreadable entries)",
                    rootfs, rc != 0 ? g_strerror(saved) : "walk ok", out->unreadable);
        return FALSE;
    }
    return TRUE;
}

                                      
                        
void
pcv_lxc_userns_scan_clear(PcvLxcUsernsScan *scan)
{
    if (!scan)
        return;
    g_clear_pointer(&scan->pairs, g_hash_table_unref);
    g_clear_pointer(&scan->suid_files, g_ptr_array_unref);
    g_clear_pointer(&scan->cap_files, g_ptr_array_unref);
    g_clear_pointer(&scan->first_outside, g_free);
}

                                                                              
                              
                                          
static gboolean
_child_failed(const gchar *const *argv, GError *local, gchar *err_out, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed: %s", argv[0],
                local ? local->message : (err_out && *err_out ? g_strstrip(err_out) : "unknown"));
    g_clear_error(&local);
    return FALSE;
}

                                                                                     
                                                                               
                                                                                                     
                                                         
static gboolean
_child_storage(const gchar *const *argv, GError **error)
{
    g_autofree gchar *err_out = NULL;
    GError *local = NULL;
    if (pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_STORAGE, NULL, &err_out, &local))
        return TRUE;
    return _child_failed(argv, local, err_out, error);
}

static gboolean
_child_runtime(const gchar *const *argv, GError **error)
{
    g_autofree gchar *err_out = NULL;
    GError *local = NULL;
    if (pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_RUNTIME, NULL, &err_out, &local))
        return TRUE;
    return _child_failed(argv, local, err_out, error);
}

                                                   
gboolean
pcv_lxc_userns_shift_rootfs(const gchar *rootfs, const PcvLxcUsernsScan *scan, gint64 start, gint64 count, GError **error)
{
    if (scan->acl > 0) {                                                                 
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "rootfs %s has %" G_GUINT64_FORMAT " entries with POSIX ACLs; the chown fallback refuses them",
                    rootfs, scan->acl);
        return FALSE;
    }
                                                                                      
    g_autoptr(GPtrArray) caps = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < scan->cap_files->len; i++) {
        const gchar *path = g_ptr_array_index(scan->cap_files, i);
        const gchar *argv[] = { "getcap", path, NULL };
        g_autofree gchar *out = NULL;
        if (!pcv_spawn_sync(argv, &out, NULL, error))
            return FALSE;
        gchar *text = pcv_lxc_userns_getcap_text(out, path);
        if (!text) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read file capabilities of %s", path);
            return FALSE;
        }
        g_ptr_array_add(caps, text);
    }
                                                                                                
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, scan->pairs);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        guint64 packed = *(guint64 *)key;
        gint64 uid = (gint64)(packed >> 32), gid = (gint64)(packed & G_GUINT64_CONSTANT(0xffffffff));
        g_autofree gchar *from = g_strdup_printf("--from=%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, uid, gid);
        g_autofree gchar *to = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                                               pcv_lxc_userns_shift_id(uid, start, count),
                                               pcv_lxc_userns_shift_id(gid, start, count));
        const gchar *argv[] = { "chown", "-R", "-h", from, to, "--", rootfs, NULL };
        if (!_child_storage(argv, error))
            return FALSE;
    }
                                                                           
    for (guint i = 0; i < scan->suid_files->len; i++) {
        const PcvLxcUsernsFile *f = g_ptr_array_index(scan->suid_files, i);
        g_autofree gchar *mode = g_strdup_printf("%04o", f->mode);
        const gchar *argv[] = { "chmod", mode, "--", f->path, NULL };
        if (!_child_storage(argv, error))
            return FALSE;
    }
                                                                           
    g_autofree gchar *map = g_strdup_printf("b:0:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, start, count);
    for (guint i = 0; i < caps->len; i++) {
        const gchar *argv[] = { "lxc-usernsexec", "-m", map, "--", "setcap",
                                g_ptr_array_index(caps, i), g_ptr_array_index(scan->cap_files, i), NULL };
        if (!_child_runtime(argv, error))
            return FALSE;
    }
    return TRUE;
}

                                                                          
                                                      
gchar *
pcv_lxc_userns_unsearchable_ancestor(const gchar *dir)
{
    g_autofree gchar *cur = g_canonicalize_filename(dir, "/");
    for (;;) {
        GStatBuf st;
        if (g_stat(cur, &st) != 0 || !(st.st_mode & S_IXOTH))
            return g_steal_pointer(&cur);
        if (g_str_equal(cur, "/"))
            return NULL;
        gchar *parent = g_path_get_dirname(cur);
        g_free(cur);
        cur = parent;
    }
}

gboolean
pcv_lxc_userns_settle_dirs(const gchar *lxc_path, const gchar *name, GError **error)
{
    g_autofree gchar *above = g_path_get_dirname(lxc_path);
    g_autofree gchar *blocked = pcv_lxc_userns_unsearchable_ancestor(above);
    if (blocked) {                           
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s must be searchable by others (o+x) for unprivileged containers", blocked);
        return FALSE;
    }
    g_autofree gchar *dir = g_build_filename(lxc_path, name, NULL);
    GStatBuf st;
    if (g_lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "container directory %s is missing", dir);
        return FALSE;
    }
    if (st.st_uid != 0 || st.st_gid != 0) {
                                                                                                   
        const gchar *argv[] = { "chown", "root:root", "--", dir, NULL };
        if (!_child_storage(argv, error))
            return FALSE;
    }
                                                                                   
                                                                     
    if (g_chmod(dir, 0711) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot set traversal permission on %s: %s",
                    dir, g_strerror(errno));
        return FALSE;
    }
                                                                           
                                                              
    if (g_lstat(lxc_path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s must be a root-owned directory", lxc_path);
        return FALSE;
    }
    if ((st.st_mode & 0011) != 0011 && g_chmod(lxc_path, (st.st_mode & 07777) | 0011) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot set traversal permission on %s: %s",
                    lxc_path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_lxc_userns_check_top(const gchar *lxc_path, const gchar *name, GError **error)
{
    g_autofree gchar *dir = g_build_filename(lxc_path, name, NULL);
    GStatBuf st;
    if (g_lstat(dir, &st) != 0 || !_owned_plain(&st, S_IFDIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s: not a non-link directory owned by uid %u",
                    name, (guint)geteuid());
        return FALSE;
    }
    GDir *d = g_dir_open(dir, 0, error);
    if (!d)
        return FALSE;
    const gchar *ent, *why = NULL;
    gboolean have_config = FALSE;
    while (!why && (ent = g_dir_read_name(d))) {
        g_autofree gchar *path = g_build_filename(dir, ent, NULL);
        gboolean is_config = g_str_equal(ent, "config");
        have_config |= is_config;
        if (g_lstat(path, &st) != 0)
            why = "cannot stat";
        else if (S_ISLNK(st.st_mode))
            why = "symbolic link";
        else if (g_str_equal(ent, "rootfs"))
            why = S_ISDIR(st.st_mode) ? NULL : "not a directory";
        else if (st.st_uid != geteuid())
            why = "not owned by the daemon";                                                      
        else if (is_config && !_owned_plain(&st, S_IFREG))
            why = "not a regular file with a single link";
        if (why)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s: %s", ent, why);
    }
    g_dir_close(d);
    if (!why && !have_config)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "config: missing");
    return !why && have_config;
}

                                                        
                                                          
static gboolean
_rootfs_verdict(const PcvLxcUsernsScan *scan, const gchar *rootfs, gint64 start, gint64 count, GError **error)
{
    if (scan->outside == 0)
        return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "rootfs %s has %" G_GUINT64_FORMAT " entries with ids outside 0-%" G_GINT64_FORMAT " and %" G_GINT64_FORMAT
                "-%" G_GINT64_FORMAT " (first: %s)", rootfs, scan->outside, count - 1, start, start + count - 1,
                scan->first_outside ? scan->first_outside : "?");
    return FALSE;
}

gboolean
pcv_lxc_userns_settle_rootfs(const gchar *lxc_path, const gchar *name, gint64 start, gint64 count,
                             gboolean *fell_back, GError **error)
{
    *fell_back = FALSE;
    g_autofree gchar *rootfs = g_build_filename(lxc_path, name, "rootfs", NULL);
    PcvLxcUsernsScan scan;
    gboolean ok = pcv_lxc_userns_scan(rootfs, start, count, &scan, error) &&
                  _rootfs_verdict(&scan, rootfs, start, count, error);
    if (ok && scan.unshifted > 0) {
        PCV_LOG_WARN(USERNS_LOG_DOM, "lxc-create left %" G_GUINT64_FORMAT " unshifted entries in %s — chown fallback",
                     scan.unshifted, rootfs);
        ok = pcv_lxc_userns_shift_rootfs(rootfs, &scan, start, count, error);
        pcv_lxc_userns_scan_clear(&scan);
        ok = ok && pcv_lxc_userns_scan(rootfs, start, count, &scan, error) &&
             _rootfs_verdict(&scan, rootfs, start, count, error);
        if (ok && scan.unshifted > 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%" G_GUINT64_FORMAT " entries still unshifted after the chown fallback",
                        scan.unshifted);
            ok = FALSE;
        }
        *fell_back = ok;
    }
    pcv_lxc_userns_scan_clear(&scan);
    return ok;
}
