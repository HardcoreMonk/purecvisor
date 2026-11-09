                                                                             
                                  
                                                                                      
  
                           
                                                                                        
                                       
                                                                   
                                                                                      
                                                                                                     
                                               
                                                                 
  
                                                                     
                               
                                                                                

#include "lxc_mountinfo.h"
#include <string.h>

                                                               
static gboolean
_parse_u64(const char *b, const char *e, guint64 *v)
{
    if (b >= e)
        return FALSE;
    guint64 acc = 0;
    for (; b < e; b++) {
        if (*b < '0' || *b > '9')
            return FALSE;
        guint d = (guint)(*b - '0');
        if (acc > (G_MAXUINT64 - d) / 10)
            return FALSE;
        acc = acc * 10 + d;
    }
    *v = acc;
    return TRUE;
}

gboolean
pcv_mountinfo_parse_dev(const char *s, guint *major, guint *minor)
{
    const char *colon = s ? strchr(s, ':') : NULL;
    guint64 a, b;
    if (!colon || !_parse_u64(s, colon, &a) || !_parse_u64(colon + 1, colon + 1 + strlen(colon + 1), &b))
        return FALSE;
    if (a > G_MAXUINT || b > G_MAXUINT)
        return FALSE;
    *major = (guint)a;
    *minor = (guint)b;
    return TRUE;
}

                                  
                              
                                                             
gsize
pcv_mountinfo_fmt_u64(guint64 v, char *buf, gsize cap)
{
    char tmp[20];
    gsize len = 0;
    do {
        tmp[len++] = (char)('0' + (guint)(v % 10));
        v /= 10;
    } while (v);
    if (len + 1 > cap)
        return 0;
    for (gsize i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

                                                                                            
                                            
                                               
static void
_unescape_inplace(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

                                                                            
                                         
static gboolean
_parse_line(char *line, PcvMountinfoLine *ln)
{
    char *starts[6], *ends[6], *opt_start = NULL, *opt_end = NULL;
    guint idx = 0;
    gboolean dash = FALSE;
    char *s = line;
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        char *b = s;
        while (*s && *s != ' ')
            s++;
        if (idx < 6) {
            starts[idx] = b;
            ends[idx] = s;
            idx++;
        } else if (s - b == 1 && *b == '-') {
            dash = TRUE;
            break;
        } else {
            if (!opt_start)
                opt_start = b;
            opt_end = s;
        }
    }
    if (idx < 6 || !dash)
        return FALSE;
    guint64 id, parent;
    guint ma, mi;
    if (!_parse_u64(starts[0], ends[0], &id) || !_parse_u64(starts[1], ends[1], &parent))
        return FALSE;
                                                    
    for (guint i = 0; i < 6; i++)
        *ends[i] = '\0';
    if (!pcv_mountinfo_parse_dev(starts[2], &ma, &mi))
        return FALSE;
    if (opt_start)
        *opt_end = '\0';
    _unescape_inplace(starts[3]);
    _unescape_inplace(starts[4]);
    ln->mnt_id = id;
    ln->parent_id = parent;
    ln->dev_major = ma;
    ln->dev_minor = mi;
    ln->root = starts[3];
    ln->mount_point = starts[4];
    ln->options = starts[5];
    ln->optional = opt_start ? opt_start : "";
    return TRUE;
}

gsize
pcv_mountinfo_parse(char *text, PcvMountinfoLine *out, gsize cap, gboolean *truncated)
{
    gsize n = 0;
    if (truncated)
        *truncated = FALSE;
    if (!text)
        return 0;
    char *p = text;
    while (*p) {
        char *eol = strchr(p, '\n');
        char *next;
        if (eol) {
            *eol = '\0';
            next = eol + 1;
        } else {
            next = p + strlen(p);
        }
        PcvMountinfoLine ln;
        if (_parse_line(p, &ln)) {
            if (out && n < cap)
                out[n++] = ln;
            else if (truncated)
                *truncated = TRUE;                                    
        }
        p = next;
    }
    return n;
}

                                                                            
                                 
gssize
pcv_mountinfo_last_under(const PcvMountinfoLine *lines, gsize n, const char *prefix,
                         const guint64 *skip_ids, gsize n_skip)
{
    if (!lines || !prefix)
        return -1;
    gsize plen = strlen(prefix);
    while (plen > 1 && prefix[plen - 1] == '/')
        plen--;                                                      
    gssize found = -1;
    for (gsize i = 0; i < n; i++) {
        const char *mp = lines[i].mount_point;
        gboolean under;
        if (plen == 1 && prefix[0] == '/')
            under = mp[0] == '/' && mp[1] != '\0';
        else
            under = strncmp(mp, prefix, plen) == 0 && mp[plen] == '/';
        if (!under)
            continue;
        gboolean skipped = FALSE;
        for (gsize k = 0; k < n_skip && skip_ids; k++)
            if (skip_ids[k] == lines[i].mnt_id)
                skipped = TRUE;
        if (!skipped)
            found = (gssize)i;                                         
    }
    return found;
}

                                                                                                              
                                      
void
pcv_mountinfo_chain(const PcvMountinfoLine *lines, const gsize *members, gsize m, guint *depth_out, guint64 *top_out)
{
    for (gsize k = 0; k < m; k++) {
        const PcvMountinfoLine *cur = &lines[members[k]];
        guint depth = 0;
        for (gboolean up = TRUE; up && depth < m;) {
            up = FALSE;
            for (gsize j = 0; j < m && !up; j++) {
                if (lines[members[j]].mnt_id == cur->parent_id) {
                    cur = &lines[members[j]];                               
                    depth++;
                    up = TRUE;
                }
            }
        }
        depth_out[k] = depth;
        if (top_out)
            top_out[k] = cur->mnt_id;
    }
}

                                                                                                           
static gboolean
_optional_shared(const char *opt)
{
    for (const char *p = opt; p && *p;) {
        while (*p == ' ')
            p++;
        if (strncmp(p, "shared:", 7) == 0)
            return TRUE;
        while (*p && *p != ' ')
            p++;
    }
    return FALSE;
}

                                                                                                                  
                                          
gsize
pcv_mountinfo_stack_visible(const PcvMountinfoLine *lines, const gsize *members, gsize m, gsize k, gboolean *covered_shared)
{
    gsize cur = k;
    gboolean shared = FALSE;
    for (gsize steps = 0; steps < m; steps++) {
        gsize up = m;
        for (gsize j = 0; j < m && up == m; j++)
            if (j != cur && lines[members[j]].parent_id == lines[members[cur]].mnt_id &&
                pcv_mountinfo_path_equal(lines[members[j]].mount_point, lines[members[cur]].mount_point))
                up = j;                                                      
        if (up == m)
            break;
        shared |= _optional_shared(lines[members[cur]].optional);                 
        cur = up;
    }
    if (covered_shared)
        *covered_shared = shared;
    return cur;
}

                                                                                             
                                             
gboolean
pcv_mountinfo_chain_has(const PcvMountinfoLine *lines, const gsize *members, gsize m, gsize k, const guint64 *ids, gsize n_ids)
{
    const PcvMountinfoLine *cur = &lines[members[k]];
    for (gsize steps = 0; steps <= m; steps++) {
        for (gsize i = 0; i < n_ids; i++)
            if (ids[i] == cur->mnt_id)
                return TRUE;
        const PcvMountinfoLine *up = NULL;
        for (gsize j = 0; j < m && !up; j++)
            if (lines[members[j]].mnt_id == cur->parent_id)
                up = &lines[members[j]];
        if (!up)
            return FALSE;
        cur = up;
    }
    return FALSE;
}

                                                                                                  
                                               
gssize
pcv_mountinfo_top_at(const PcvMountinfoLine *lines, gsize n, const char *path)
{
    gssize found = -1;
    for (gsize i = 0; lines && i < n; i++) {
        if (!pcv_mountinfo_path_equal(lines[i].mount_point, path))
            continue;
        gboolean covered = FALSE;                                                          
        for (gsize j = 0; j < n && !covered; j++)
            covered = j != i && lines[j].parent_id == lines[i].mnt_id && pcv_mountinfo_path_equal(lines[j].mount_point, path);
        if (!covered)
            found = (gssize)i;
    }
    return found;
}

gboolean
pcv_mountinfo_path_normalize(const char *in, char *out, gsize cap)
{
    if (!in || !out || in[0] != '/')
        return FALSE;
    gsize w = 0;
    for (const char *r = in; *r; r++) {
        if (*r == '/' && w > 0 && out[w - 1] == '/')
            continue;                                  
        if (w + 1 >= cap)
            return FALSE;                                          
        out[w++] = *r;
    }
    if (w > 1 && out[w - 1] == '/')
        w--;                                                       
    out[w] = '\0';
    return TRUE;
}

                                                                                                    
                                                                                               
                                                                                             
                                                                                   
                                                                 

                                                                                             
                                                                               
                                                 
static char
_norm_next(const char **pp, gboolean *any)
{
    const char *p = *pp;
    if (*p == '/') {
        while (p[1] == '/')
            p++;                                               
        if (p[1] == '\0' && *any) {
            *pp = p + 1;
            return '\0';                                      
        }
    }
    if (*p == '\0')
        return '\0';
    *pp = p + 1;
    *any = TRUE;
    return *p;
}

                                                     
static gsize
_norm_len(const char *path)
{
    gboolean any = FALSE;
    gsize n = 0;
    while (_norm_next(&path, &any) != '\0')
        n++;
    return n;
}

                                                                                                  
                                                                                 
static gboolean
_norm_under_or_same(const char *path, const char *base)
{
    if (!path || !base || path[0] != '/' || base[0] != '/')
        return FALSE;
    gboolean pa = FALSE, ba = FALSE;
    (void)_norm_next(&path, &pa);                                    
    (void)_norm_next(&base, &ba);
    char bc = _norm_next(&base, &ba);
    if (bc == '\0')
        return TRUE;                                               
    for (;;) {
        char pc = _norm_next(&path, &pa);
        if (bc == '\0')
            return pc == '\0' || pc == '/';                                                
        if (pc != bc)
            return FALSE;
        bc = _norm_next(&base, &ba);
    }
}

                                                                                             
static gboolean
_norm_equal_from(const char *a, gboolean aa, const char *b, gboolean ba)
{
    for (;;) {
        char x = _norm_next(&a, &aa), y = _norm_next(&b, &ba);
        if (x != y)
            return FALSE;
        if (x == '\0')
            return TRUE;
    }
}

                                                                             
                                          
gboolean
pcv_mountinfo_path_equal(const char *a, const char *b)
{
    if (!a || !b || a[0] != '/' || b[0] != '/')
        return FALSE;
    return _norm_equal_from(a, FALSE, b, FALSE);
}

                                                                                                         
                                                                   
                                                     
static gboolean
_dest_seg_next(const char **pp, const char **seg, gsize *len)
{
    const char *p = *pp;
    for (;;) {
        while (*p == '/')
            p++;
        if (*p == '\0') {
            *pp = p;
            return FALSE;
        }
        const char *s = p;
        while (*p && *p != '/')
            p++;
        if (p - s == 1 && s[0] == '.')
            continue;                                                  
        *seg = s;
        *len = (gsize)(p - s);
        *pp = p;
        return TRUE;
    }
}

                                                                                              
                                                                   
gboolean
pcv_proc_cgroup_v2_under(const char *text, const char *cgroup)
{
    if (!text || !cgroup || cgroup[0] != '/' || cgroup[1] == '\0')
        return FALSE;
    const gsize cl = strlen(cgroup);
    if (cgroup[cl - 1] == '/')
        return FALSE;                                                  
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        const gsize len = end ? (gsize)(end - line) : strlen(line);
        if (len >= 3 && memcmp(line, "0::", 3) == 0) {                            
            const char *path = line + 3;
            const gsize pl = len - 3;
            return pl >= cl && memcmp(path, cgroup, cl) == 0 && (pl == cl || path[cl] == '/');
        }
        if (!end)
            break;
        line = end + 1;
    }
    return FALSE;                                                                 
}

                                                                                                                
                                                                                                             
                                                                                           
gboolean
pcv_vol_dest_equal(const char *a, const char *b)
{
    if (!a || !b)
        return FALSE;
    for (;;) {
        const char *sa = NULL, *sb = NULL;
        gsize la = 0, lb = 0;
        const gboolean ha = _dest_seg_next(&a, &sa, &la), hb = _dest_seg_next(&b, &sb, &lb);
        if (ha != hb)
            return FALSE;
        if (!ha)
            return TRUE;
        if (la != lb || memcmp(sa, sb, la) != 0)
            return FALSE;
    }
}

                                                                                                    
                             
gsize
pcv_vol_ident_root_cap(const char *fs_root, const char *canon_source)
{
    return (fs_root ? strlen(fs_root) : 0) + (canon_source ? strlen(canon_source) : 0) + 2;
}

                                                                                                  
                                                                               
                                                                                 
                              
                                                                                  
gboolean
pcv_vol_ident_from_mount(guint major, guint minor, const char *fs_root, const char *mount_point,
                         const char *canon_source, char *root_buf, gsize root_cap, PcvVolIdent *out)
{
    if (!out || !root_buf || !fs_root || fs_root[0] != '/' || !mount_point || mount_point[0] != '/' ||
        !canon_source || canon_source[0] != '/')
        return FALSE;
    if (!_norm_under_or_same(canon_source, mount_point))
        return FALSE;                                                  
    const char *rp = canon_source;
    gboolean ra = FALSE;
    const gsize mpn = _norm_len(mount_point);
    if (mpn == 1) {
        if (_norm_len(canon_source) == 1)
            rp = "";                                                                           
    } else {
        for (gsize k = 0; k < mpn; k++)
            (void)_norm_next(&rp, &ra);                                              
    }
    gsize w = 0;
    char c;
    if (_norm_len(fs_root) > 1) {                                                 
        const char *fp = fs_root;
        gboolean fa = FALSE;
        while ((c = _norm_next(&fp, &fa)) != '\0') {
            if (w + 1 >= root_cap)
                return FALSE;
            root_buf[w++] = c;
        }
    }
    while ((c = _norm_next(&rp, &ra)) != '\0') {
        if (w + 1 >= root_cap)
            return FALSE;
        root_buf[w++] = c;
    }
    if (w == 0) {
        if (root_cap < 2)
            return FALSE;
        root_buf[w++] = '/';
    }
    root_buf[w] = '\0';
    out->root = root_buf;
    out->major = major;
    out->minor = minor;
    return TRUE;
}

                                                                                
                                                 
gboolean
pcv_vol_ident_covers(const PcvVolIdent *id, guint major, guint minor, const char *root)
{
    if (!id || !id->root || id->major != major || id->minor != minor)
        return FALSE;
    return _norm_under_or_same(root, id->root);
}

                              
                              
                                                                                  
gint
pcv_vol_attribute(const PcvVolIdent *const *idents, const char *const *dests, gsize n,
                  guint major, guint minor, const char *root, const char *mount_dest)
{
    gsize best_len = 0, n_best = 0;
    gint first_best = PCV_VOL_ATTR_NONE;
    gboolean unjudgeable = FALSE;
    for (gsize i = 0; i < n; i++) {
        if (idents[i] && idents[i]->major == major && idents[i]->minor == minor && (!root || root[0] != '/'))
            unjudgeable = TRUE;                                                                  
        if (!idents[i] || !pcv_vol_ident_covers(idents[i], major, minor, root))
            continue;
        gsize len = _norm_len(idents[i]->root);
        if (n_best == 0 || len > best_len) {
            best_len = len;
            n_best = 1;
            first_best = (gint)i;
        } else if (len == best_len) {
            n_best++;
        }
    }
    if (n_best == 0)
        return unjudgeable ? PCV_VOL_ATTR_AMBIGUOUS : PCV_VOL_ATTR_NONE;
    if (n_best == 1)
        return first_best;                                                           
                                                                                        
                                                              
    gboolean mixed = FALSE;
    gint first = PCV_VOL_ATTR_NONE;
    for (gsize i = 0; i < n; i++) {
        if (!idents[i] || !idents[i]->root || _norm_len(idents[i]->root) != best_len || !pcv_vol_ident_covers(idents[i], major, minor, root))
            continue;
        if (first == PCV_VOL_ATTR_NONE)
            first = (gint)i;
        else if (!dests[i] || !dests[first] || !pcv_vol_dest_equal(dests[i], dests[first]))
            mixed = TRUE;
    }
    if (!mixed)
        return first;
    for (gsize i = 0; i < n; i++) {
        if (!idents[i] || !idents[i]->root || _norm_len(idents[i]->root) != best_len || !pcv_vol_ident_covers(idents[i], major, minor, root))
            continue;
        if (mount_dest && dests[i] && pcv_vol_dest_equal(dests[i], mount_dest))
            return (gint)i;
    }
    return PCV_VOL_ATTR_AMBIGUOUS;
}
