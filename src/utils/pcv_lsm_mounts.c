   
                         
                                                                                           
  
                           
                                                
                                                                       
                                                                               
                                                                   
                                                                   
                                                                                  
                               
                                                                               
                                                                    
                                                               
  
                                                                
                                                               
                                                           
   
#include "utils/pcv_lsm_mounts.h"
#include "utils/pcv_log.h"
#include "bpf/pcv_bpf_shared.h"                               
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define LSM_MOUNTS_LOG_DOM     "bpf"
#define PCV_LSM_MOUNTINFO_PATH "/proc/self/mountinfo"

                                                                         
                                                  
static const char *const k_watch_prefixes[] = {
    "/etc/purecvisor",
    "/var/lib/purecvisor",
    "/usr/lib/purecvisor/bpf",
    "/etc/systemd/system/purecvisor",
    "/usr/lib/systemd/system/purecvisor",
    "/lib/systemd/system/purecvisor",
    NULL,
};

                                                                         
struct PcvLsmMountSet {
    guint    n;                     
    gchar  **prefixes;                       
    GArray **devs;                        
};

                                          
struct PcvLsmMountTracker {
    int                fd;                                                      
    gboolean           own_fd;                                
    PcvLsmMountReader  reader;                                              
    gpointer           reader_data;
    gchar            **prefixes;                  
    PcvLsmMountSet    *set;                                                 
    gint64             last_build_us;                                     
};

                                                                                       
static gchar *
_unescape_field(const char *s)
{
    GString *o = g_string_sized_new(strlen(s));
    for (const char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' &&
            p[3] >= '0' && p[3] <= '7') {
            g_string_append_c(o, (char)(((p[1] - '0') << 6) | ((p[2] - '0') << 3) | (p[3] - '0')));
            p += 3;
        } else {
            g_string_append_c(o, *p);
        }
    }
    return g_string_free(o, FALSE);
}

                                                                       
                                                                  
static gboolean
_parse_majmin(const char *s, guint32 *out)
{
    const char *colon = strchr(s, ':');
    if (!colon || !g_ascii_isdigit(s[0]) || !g_ascii_isdigit(colon[1])) return FALSE;
    gchar *end = NULL;
    guint64 maj = g_ascii_strtoull(s, &end, 10);
    if (end != colon) return FALSE;
    guint64 min = g_ascii_strtoull(colon + 1, &end, 10);
    if (*end != '\0') return FALSE;
    if (maj >= (G_GUINT64_CONSTANT(1) << 12) || min >= (G_GUINT64_CONSTANT(1) << 20)) return FALSE;
    *out = (guint32)((maj << 20) | min);
    return TRUE;
}

                                                                             
static gboolean
_is_ancestor_or_self(const char *mp, const char *path)
{
    if (strcmp(mp, "/") == 0) return path[0] == '/';
    size_t n = strlen(mp);
    return strncmp(path, mp, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

                                                   
static void
_add_unique(GArray *a, guint32 dev)
{
    for (guint i = 0; i < a->len; i++)
        if (g_array_index(a, guint32, i) == dev) return;
    g_array_append_val(a, dev);
}

PcvLsmMountSet *
pcv_lsm_mount_set_parse(const char *mountinfo, const char *const *prefixes)
{
    if (!mountinfo) return NULL;
    PcvLsmMountSet *set = g_new0(PcvLsmMountSet, 1);
    set->prefixes = prefixes ? g_strdupv((gchar **)prefixes) : g_new0(gchar *, 1);
    set->n = g_strv_length(set->prefixes);
    set->devs = g_new0(GArray *, set->n + 1);
    for (guint i = 0; i < set->n; i++)
        set->devs[i] = g_array_new(FALSE, FALSE, sizeof(guint32));

    gchar **lines = g_strsplit(mountinfo, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (**l == '\0') continue;
                                                                                   
        gchar **f = g_strsplit(*l, " ", 6);
        guint32 dev = 0;
        if (g_strv_length(f) >= 5 && f[4][0] == '/' && _parse_majmin(f[2], &dev)) {
            gchar *mp = _unescape_field(f[4]);
            for (guint i = 0; i < set->n; i++) {
                const char *p = set->prefixes[i];
                if (*p && (_is_ancestor_or_self(mp, p) || g_str_has_prefix(mp, p)))
                    _add_unique(set->devs[i], dev);
            }
            g_free(mp);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    return set;
}

void
pcv_lsm_mount_set_free(PcvLsmMountSet *set)
{
    if (!set) return;
    for (guint i = 0; i < set->n; i++)
        g_array_unref(set->devs[i]);
    g_free(set->devs);
    g_strfreev(set->prefixes);
    g_free(set);
}

PcvLsmClass
pcv_lsm_mount_set_classify(const PcvLsmMountSet *set, guint32 hook, guint32 s_dev, const char *path)
{
    if (!set || hook != PCV_LSM_HOOK_FILE_OPEN || s_dev == 0 || !path)
        return PCV_LSM_CLASS_SUBMIT;
    for (guint i = 0; i < set->n; i++) {
        const char *p = set->prefixes[i];
        if (!*p || !g_str_has_prefix(path, p)) continue;                            
        const GArray *a = set->devs[i];
        if (a->len == 0) return PCV_LSM_CLASS_SUBMIT;                                 
        for (guint k = 0; k < a->len; k++)
            if (g_array_index(a, guint32, k) == s_dev) return PCV_LSM_CLASS_SUBMIT;
        return PCV_LSM_CLASS_FOREIGN_FS;
    }
    return PCV_LSM_CLASS_SUBMIT;                      
}

                                                                          
static gchar *
_read_fd_all(int fd)
{
    if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0) return NULL;
    GString *s = g_string_sized_new(16384);
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) { g_string_append_len(s, buf, n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        g_string_free(s, TRUE);
        return NULL;
    }
    return g_string_free(s, FALSE);
}

                                                                    
static void
_rebuild(PcvLsmMountTracker *t, gint64 now_us)
{
    gchar *text = t->reader ? t->reader(t->reader_data) : _read_fd_all(t->fd);
    PcvLsmMountSet *ns = pcv_lsm_mount_set_parse(text, (const char *const *)t->prefixes);
    g_free(text);
    if (!ns && t->set)
        PCV_LOG_WARN(LSM_MOUNTS_LOG_DOM,
                     "LSM 마운트 표를 다시 읽지 못함 — 다른 파일시스템 구분 없이 감시 경로 사건을 모두 제출한다");
    pcv_lsm_mount_set_free(t->set);
    t->set = ns;
    t->last_build_us = now_us;
}

                          
static PcvLsmMountTracker *
_tracker_new(int fd, gboolean own_fd, PcvLsmMountReader reader, gpointer reader_data,
             const char *const *prefixes, gint64 now_us)
{
    PcvLsmMountTracker *t = g_new0(PcvLsmMountTracker, 1);
    t->fd = fd;
    t->own_fd = own_fd;
    t->reader = reader;
    t->reader_data = reader_data;
    t->prefixes = prefixes ? g_strdupv((gchar **)prefixes) : g_new0(gchar *, 1);
    _rebuild(t, now_us);
    return t;
}

  
                                                                                                 
                                                                              
                                       
                                                             
   
PcvLsmMountTracker *
pcv_lsm_mount_tracker_new_for_test(int watch_fd, PcvLsmMountReader reader, gpointer user_data,
                                   const char *const *prefixes, gint64 now_us)
{
    return _tracker_new(watch_fd, FALSE, reader, user_data, prefixes, now_us);
}

PcvLsmMountTracker *
pcv_lsm_mount_tracker_open(void)
{
    int fd = open(PCV_LSM_MOUNTINFO_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        PCV_LOG_WARN(LSM_MOUNTS_LOG_DOM,
                     "%s 열기 실패(%s) — 다른 파일시스템 구분 없이 감시 경로 사건을 모두 제출한다",
                     PCV_LSM_MOUNTINFO_PATH, g_strerror(errno));
    PcvLsmMountTracker *t = _tracker_new(fd, TRUE, NULL, NULL, k_watch_prefixes, g_get_monotonic_time());
    if (t->set) {
        guint ndev = 0;
        for (guint i = 0; i < t->set->n; i++) ndev += t->set->devs[i]->len;
        PCV_LOG_INFO(LSM_MOUNTS_LOG_DOM,
                     "LSM 감시 접두사 장치 집합 준비: 접두사 %u개, 장치 %u개(mountinfo, 변경 통지로 갱신)",
                     t->set->n, ndev);
    } else if (fd >= 0) {
        PCV_LOG_WARN(LSM_MOUNTS_LOG_DOM,
                     "%s 읽기 실패 — 다른 파일시스템 구분 없이 감시 경로 사건을 모두 제출한다",
                     PCV_LSM_MOUNTINFO_PATH);
    }
    return t;
}

void
pcv_lsm_mount_tracker_free(PcvLsmMountTracker *t)
{
    if (!t) return;
    if (t->own_fd && t->fd >= 0) close(t->fd);
    pcv_lsm_mount_set_free(t->set);
    g_strfreev(t->prefixes);
    g_free(t);
}

int
pcv_lsm_mount_tracker_poll(PcvLsmMountTracker *t, int ready_fd, int timeout_ms)
{
    struct pollfd p[2];
    nfds_t n = 0;
    int ri = -1, mi = -1;
    if (ready_fd >= 0) {
        ri = (int)n;
        p[n++] = (struct pollfd){ .fd = ready_fd, .events = POLLIN };
    }
    if (t && t->fd >= 0) {
        mi = (int)n;
        p[n++] = (struct pollfd){ .fd = t->fd, .events = POLLPRI };                                      
    }
    int rc = poll(p, n, timeout_ms);
    if (rc < 0) return -errno;
    if (mi >= 0 && p[mi].revents) {
        if (p[mi].revents & POLLNVAL) {
            PCV_LOG_WARN(LSM_MOUNTS_LOG_DOM,
                         "LSM 마운트 표 fd 무효 — 변경 통지 감시를 끈다(불일치 재읽기만 남는다)");
            t->own_fd = FALSE;                              
            t->fd = -1;
        } else if (p[mi].revents & (POLLPRI | POLLERR)) {
            _rebuild(t, g_get_monotonic_time());                                          
        }
    }
    return (ri >= 0 && (p[ri].revents & (POLLIN | POLLERR | POLLHUP))) ? 1 : 0;
}

PcvLsmClass
pcv_lsm_mount_tracker_classify(PcvLsmMountTracker *t, guint32 hook, guint32 s_dev,
                               const char *path, gint64 now_us)
{
    if (!t) return PCV_LSM_CLASS_SUBMIT;
    PcvLsmClass c = pcv_lsm_mount_set_classify(t->set, hook, s_dev, path);
                                                                    
                                                                   
    gboolean candidate = (c == PCV_LSM_CLASS_FOREIGN_FS) ||
                         (!t->set && hook == PCV_LSM_HOOK_FILE_OPEN && s_dev != 0);
    if (candidate && now_us - t->last_build_us >= PCV_LSM_MOUNT_REREAD_MIN_US) {
        _rebuild(t, now_us);
        c = pcv_lsm_mount_set_classify(t->set, hook, s_dev, path);
    }
    return c;
}
