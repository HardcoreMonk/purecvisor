                                                                                     
                                                                                                              
                                                                                                  
                                                                                     
                                                                
                                                                                        
                                                                                   
                                                                                                     
                                                                                        
                                                                                       
                                                                                                            
                                                                                                               
                                                                                   
                                                                                                             
                                                                                                                           
                                                                                                   
                                                                                                                   
                                                                                                               
                                                                                         
                                                                                            
                                                                                                
                                                                                                                              
                                                                                                        
                             
  
                                                                              
                                                                             
                                                                           
                                                                                     
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ftw.h>
#include <limits.h>
#include "modules/lxc/lxc_volume_mount.h"
#include "modules/lxc/lxc_userns.h"                                                        
#include "modules/lxc/lxc_volume_guard.h"                                                                              
#include "modules/lxc/lxc_volume_detach.h"                                                                    

                                                                                                      

                                                                                                       
                                                                                                      
                                                                                                            
typedef struct { guint64 id; gchar *mp; gboolean alive; guint64 parent; guint peer; gchar *dev, *root; guint master; } FakeMount;

typedef struct {
    GArray     *mounts;                                                         
    GHashTable *dirs;                                                                
    GHashTable *types;                                                                 
    GHashTable *fd_path;                       
    GHashTable *fd_mount;                                
    GPtrArray  *log;                                                                                                                   
    GArray     *made;                            
    int         base_fd;                                                  
    struct stat base_st;
    gchar      *alias_from, *alias_to;                                    
    gchar      *denied;                                               
    guint64     lie_id;                                                      
    gboolean    lie_all;                                 
    guint64     lie_once_id;                                                   
    guint64     private_fail_id;                                
    gboolean    umount_sticky;                                             
    gboolean    umount_fail;                                   
    guint       umounts;
    GArray     *umount_ids;                                     
} Fake;

static Fake F;
static gchar *T_base, *T_lxc, *T_stage;                                              

                                                                                   
static gchar *
fake_norm(const gchar *p)
{
    GString *s = g_string_new(NULL);
    for (const gchar *c = p; *c; c++)
        if (!(*c == '/' && s->len && s->str[s->len - 1] == '/'))
            g_string_append_c(s, *c);
    while (s->len > 1 && s->str[s->len - 1] == '/')
        g_string_truncate(s, s->len - 1);
    return g_string_free(s, FALSE);
}

                                    
static FakeMount *
fake_find(guint64 id)
{
    for (guint i = 0; i < F.mounts->len; i++)
        if (g_array_index(F.mounts, FakeMount, i).id == id)
            return &g_array_index(F.mounts, FakeMount, i);
    return NULL;
}

                                                                                           
                                                                             
static guint64
fake_resolve(const gchar *path)
{
    if (F.mounts->len == 0)
        return 0;
    guint64 cur = g_array_index(F.mounts, FakeMount, 0).id;
    gsize len = strlen(path);
    for (gsize i = 1; i <= len; i++) {
        if (i < len && path[i] != '/')
            continue;
        g_autofree gchar *pre = g_strndup(path, i);
        for (gboolean down = TRUE; down;) {
            down = FALSE;
            for (guint j = 0; j < F.mounts->len && !down; j++) {
                FakeMount *m = &g_array_index(F.mounts, FakeMount, j);
                if (m->alive && m->parent == cur && strcmp(m->mp, pre) == 0) {
                    cur = m->id;
                    down = TRUE;
                }
            }
        }
    }
    return cur;
}

                                                                
static int
fake_new_fd(const gchar *path)
{
    int fd = fcntl(F.base_fd, F_DUPFD_CLOEXEC, 0);
    g_assert_cmpint(fd, >=, 0);
    g_hash_table_insert(F.fd_path, GINT_TO_POINTER(fd), g_strdup(path));
    g_hash_table_insert(F.fd_mount, GINT_TO_POINTER(fd), g_memdup2(&(guint64){ fake_resolve(path) }, sizeof(guint64)));
    g_array_append_val(F.made, fd);
    return fd;
}

static guint64
fake_fd_mount(int fd)
{
    guint64 *id = g_hash_table_lookup(F.fd_mount, GINT_TO_POINTER(fd));
    g_assert_nonnull(id);
    return *id;
}

static gchar *
fake_read_mountinfo(GError **error)
{
    (void)error;
    g_ptr_array_add(F.log, g_strdup("read"));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < F.mounts->len; i++) {
        FakeMount *m = &g_array_index(F.mounts, FakeMount, i);
        if (!m->alive)
            continue;
        g_autofree gchar *dev = m->dev ? g_strdup(m->dev) : g_strdup_printf("0:%" G_GUINT64_FORMAT, m->id);
        GString *og = g_string_new(NULL);
        if (m->peer)
            g_string_append_printf(og, " shared:%u", m->peer);
        if (m->master)
            g_string_append_printf(og, " master:%u", m->master);
        g_autofree gchar *opt = g_string_free(og, FALSE);
        g_string_append_printf(s, "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %s %s %s rw,relatime%s - zfs pool/x rw\n", m->id,
                               m->parent, dev, m->root ? m->root : "/", m->mp, opt);
    }
    return g_string_free(s, FALSE);
}

static int
fake_open_dir(const gchar *path, GError **error)
{
    g_autofree gchar *n = fake_norm(path);
    if (F.alias_from && g_str_has_prefix(n, F.alias_from) &&
        (n[strlen(F.alias_from)] == '/' || n[strlen(F.alias_from)] == '\0')) {
        gchar *r = g_strconcat(F.alias_to, n + strlen(F.alias_from), NULL);
        g_free(n);
        n = r;
    }
    g_ptr_array_add(F.log, g_strdup_printf("open %s", n));
    if (F.denied && strcmp(F.denied, n) == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Permission denied");
        return -1;
    }
    if (!g_hash_table_contains(F.dirs, n)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such file or directory");
        return -1;
    }
    return fake_new_fd(n);
}

static gchar *
fake_fd_path(int fd)
{
    return g_strdup(g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(fd)));
}

                                                                                                
                                                                  
static gboolean
fake_types_ok(const gchar *root, const gchar *full, gboolean dir_only, GError **error)
{
    gsize len = strlen(full);
    for (gsize i = strlen(root) + 1; i <= len; i++) {
        if (i < len && full[i] != '/')
            continue;
        g_autofree gchar *pre = g_strndup(full, i);
        gchar kind = (gchar)GPOINTER_TO_INT(g_hash_table_lookup(F.types, pre));
        gboolean last = i == len;
        if (kind == 'l') {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS, "symbolic link (not followed)");
            return FALSE;
        }
        if ((kind == 'f' && (!last || dir_only)) || kind == 'p') {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "not a directory or regular file");
            return FALSE;
        }
    }
    return TRUE;
}

static int
fake_walk(int root_fd, const gchar *rel, gboolean create, GError **error)
{
    (void)error;
    g_assert_false(create);                                            
    g_assert_true(rel && rel[0] == '/');                                             
    const gchar *root = g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(root_fd));
    g_assert_nonnull(root);
    g_autofree gchar *raw = g_strconcat(root, rel, NULL);
    g_autofree gchar *full = fake_norm(raw);
    g_ptr_array_add(F.log, g_strdup_printf("walk %s", full));
    if (!fake_types_ok(root, full, TRUE, error))                                  
        return -1;
    return fake_new_fd(full);
}

                                                                                                      
static int
fake_open_leaf(int root_fd, const gchar *rel, GError **error)
{
    g_assert_true(rel && rel[0] == '/');
    const gchar *root = g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(root_fd));
    g_assert_nonnull(root);
    g_autofree gchar *raw = g_strconcat(root, rel, NULL);
    g_autofree gchar *full = fake_norm(raw);
    g_ptr_array_add(F.log, g_strdup_printf("walk %s", full));
    if (!fake_types_ok(root, full, FALSE, error))
        return -1;
    return fake_new_fd(full);
}

static gboolean
fake_mnt_id(int fd, guint64 *id_out)
{
    g_ptr_array_add(F.log, g_strdup_printf("mnt_id %d", fd));
    guint64 id = fake_fd_mount(fd);
    gboolean once = F.lie_once_id && id == F.lie_once_id;
    if (once)
        F.lie_once_id = 0;
    *id_out = (F.lie_all || once || (F.lie_id && id == F.lie_id)) ? id + 1000 : id;
    return TRUE;
}

                                                                                                   
static void
fake_subtree(guint64 id, void (*fn)(FakeMount *m))
{
    FakeMount *top = fake_find(id);
    if (!top || !top->alive)
        return;
    for (guint i = 0; i < F.mounts->len; i++) {
        FakeMount *m = &g_array_index(F.mounts, FakeMount, i);
        if (m->alive && m->parent == id && m->id != id)
            fake_subtree(m->id, fn);
    }
    fn(top);
}

static void
fake_set_private(FakeMount *m)
{
    m->peer = 0;
}

static void
fake_kill(FakeMount *m)
{
    m->alive = FALSE;
}

                                           
static gchar *
fake_join(const gchar *a, const gchar *b)
{
    g_autofree gchar *raw = g_strconcat(a, "/", b, NULL);
    return fake_norm(raw);
}

                                                                                                            
                                                                                            
                                                                                              
static void
fake_propagate(const FakeMount *x)
{
    const FakeMount *p = fake_find(x->parent);
    if (!p || !p->alive || !p->peer)
        return;
    const gchar *proot = p->root ? p->root : "/";
    const gchar *rel = strcmp(p->mp, "/") == 0 ? x->mp : x->mp + strlen(p->mp);
    g_autofree gchar *dentry = fake_join(proot, rel);
    for (guint i = 0; i < F.mounts->len; i++) {
        const FakeMount *q = &g_array_index(F.mounts, FakeMount, i);
        if (!q->alive || q == p || q->peer != p->peer)
            continue;
        const gchar *qroot = q->root ? q->root : "/";
        gsize ql = strcmp(qroot, "/") == 0 ? 0 : strlen(qroot);
        if (ql && !(g_str_has_prefix(dentry, qroot) && (dentry[ql] == '/' || dentry[ql] == '\0')))
            continue;                                                   
        g_autofree gchar *qpath = fake_join(q->mp, dentry + ql);
        for (guint j = 0; j < F.mounts->len; j++) {
            FakeMount *r = &g_array_index(F.mounts, FakeMount, j);
            if (r->alive && r->parent == q->id && strcmp(r->mp, qpath) == 0) {
                g_ptr_array_add(F.log, g_strdup_printf("propagated %" G_GUINT64_FORMAT, r->id));
                fake_subtree(r->id, fake_kill);
            }
        }
    }
}

static gboolean
fake_make_private(int fd, GError **error)
{
    guint64 id = fake_fd_mount(fd);
    g_ptr_array_add(F.log, g_strdup_printf("private %d %" G_GUINT64_FORMAT, fd, id));
    if (F.private_fail_id && id == F.private_fail_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "mount_setattr private: Invalid argument");
        return FALSE;
    }
    fake_subtree(id, fake_set_private);                                            
    return TRUE;
}

                                                                                                           
static gboolean
fake_umount_fd(int fd, GError **error)
{
    g_ptr_array_add(F.log, g_strdup_printf("umount %d %s", fd, (const gchar *)g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(fd))));
    F.umounts++;
    guint64 id = fake_fd_mount(fd);
    g_array_append_val(F.umount_ids, id);
    if (F.umount_fail) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "umount: Device or resource busy");
        return FALSE;
    }
    if (F.umount_sticky)
        return TRUE;
    FakeMount *x = fake_find(id);
    if (x && x->alive) {
        fake_propagate(x);
        fake_subtree(id, fake_kill);
    }
    return TRUE;
}

static gboolean
fake_remove_at(int dir_fd, const gchar *name, GError **error)
{
    (void)error;
    const gchar *dir = g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(dir_fd));
    g_assert_nonnull(dir);
    g_autofree gchar *child = g_strdup_printf("%s/%s", dir, name);
    g_ptr_array_add(F.log, g_strdup_printf("remove %s", child));
    GPtrArray *ents = g_hash_table_lookup(F.dirs, dir);
    if (ents)
        for (guint i = 0; i < ents->len; i++)
            if (strcmp(g_ptr_array_index(ents, i), name) == 0) {
                g_ptr_array_remove_index(ents, i);
                break;
            }
    g_hash_table_remove(F.dirs, child);
    return TRUE;
}

static GPtrArray *
fake_list_at(int dir_fd, GError **error)
{
    (void)error;
    const gchar *dir = g_hash_table_lookup(F.fd_path, GINT_TO_POINTER(dir_fd));
    g_ptr_array_add(F.log, g_strdup_printf("list %s", dir));
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *ents = g_hash_table_lookup(F.dirs, dir);
    for (guint i = 0; ents && i < ents->len; i++)
        g_ptr_array_add(out, g_strdup(g_ptr_array_index(ents, i)));
    return out;
}

static const PcvLxcMountOps FAKE_OPS = {
    .read_mountinfo = fake_read_mountinfo,
    .open_dir       = fake_open_dir,
    .fd_path        = fake_fd_path,
    .walk           = fake_walk,
    .open_leaf      = fake_open_leaf,
    .mnt_id         = fake_mnt_id,
    .make_private   = fake_make_private,
    .umount_fd      = fake_umount_fd,
    .remove_at      = fake_remove_at,
    .list_at        = fake_list_at,
};

                                                                                               

                                                                             
static void
fake_mount_ex(guint64 id, const gchar *mp, guint64 parent, guint peer, const gchar *dev, const gchar *root)
{
    FakeMount m = { id, g_strdup(mp), TRUE, parent, peer, g_strdup(dev), g_strdup(root), 0 };
    g_array_append_val(F.mounts, m);
}

                                                                   
static void
fake_mount(guint64 id, const gchar *mp)
{
    fake_mount_ex(id, mp, fake_resolve(mp), 0, NULL, NULL);
}

                                                                                                              
static void
fake_slave(guint64 id, guint master)
{
    FakeMount *m = fake_find(id);
    g_assert_nonnull(m);
    m->peer = 0;
    m->master = master;
}

                                                                                                     
static gchar *
fake_pu_seq(void)
{
    GString *s = g_string_new(NULL);
    guint u = 0;
    for (guint i = 0; i < F.log->len; i++) {
        const gchar *e = g_ptr_array_index(F.log, i);
        if (g_str_has_prefix(e, "private "))
            g_string_append_printf(s, "%sp%s", s->len ? "," : "", strchr(e + 8, ' ') + 1);
        else if (g_str_has_prefix(e, "umount ") && u < F.umount_ids->len)
            g_string_append_printf(s, "%su%" G_GUINT64_FORMAT, s->len ? "," : "", g_array_index(F.umount_ids, guint64, u++));
    }
    return g_string_free(s, FALSE);
}

                                                                                         
static void
fake_shared(guint64 id, guint peer, const gchar *dev)
{
    FakeMount *m = fake_find(id);
    g_assert_nonnull(m);
    m->peer = peer;
    g_free(m->dev);
    m->dev = g_strdup(dev);
}

                                                           
static void
fake_dir(const gchar *path, ...)
{
    GPtrArray *ents = g_ptr_array_new_with_free_func(g_free);
    va_list ap;
    va_start(ap, path);
    for (const gchar *e = va_arg(ap, const gchar *); e; e = va_arg(ap, const gchar *))
        g_ptr_array_add(ents, g_strdup(e));
    va_end(ap);
    g_hash_table_insert(F.dirs, g_strdup(path), ents);
}

static void
fake_mount_clear(gpointer p)
{
    FakeMount *m = p;
    g_free(m->mp);
    g_free(m->dev);
    g_free(m->root);
}

                                                                                              
static void
fake_begin(void)
{
    memset(&F, 0, sizeof F);
    F.mounts = g_array_new(FALSE, TRUE, sizeof(FakeMount));
    g_array_set_clear_func(F.mounts, fake_mount_clear);
    F.dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    F.types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    F.fd_path = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    F.fd_mount = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    F.log = g_ptr_array_new_with_free_func(g_free);
    F.made = g_array_new(FALSE, FALSE, sizeof(int));
    F.umount_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    F.base_fd = memfd_create("pcv-fake-mount-fd", MFD_CLOEXEC);
    g_assert_cmpint(F.base_fd, >=, 0);
    g_assert_cmpint(fstat(F.base_fd, &F.base_st), ==, 0);
    T_base = g_dir_make_tmp("pcv-vmount-XXXXXX", NULL);
    g_assert_nonnull(T_base);
    T_lxc = g_strdup_printf("%s/lxc", T_base);
    T_stage = g_strdup_printf("%s/volstage", T_base);
    fake_mount(1, "/");
    g_autofree gchar *ds = g_strdup_printf("%s/c", T_lxc);
    fake_mount(2, ds);
    g_autofree gchar *rootfs = g_strdup_printf("%s/c/rootfs", T_lxc);
    fake_dir(rootfs, NULL);
    pcv_lxc_volume_set_ops_for_test(&FAKE_OPS);
    pcv_lxc_volume_set_roots_for_test(T_stage, NULL);
}

                                                                                         
static void
fake_assert_no_leak(void)
{
    for (guint i = 0; i < F.made->len; i++) {
        int fd = g_array_index(F.made, int, i);
        struct stat st;
        if (fcntl(fd, F_GETFD) >= 0 && fstat(fd, &st) == 0)
            g_assert_false(st.st_ino == F.base_st.st_ino && st.st_dev == F.base_st.st_dev);
    }
}

static void
fake_end(void)
{
    fake_assert_no_leak();
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    close(F.base_fd);
    g_array_unref(F.mounts);
    g_hash_table_unref(F.dirs);
    g_hash_table_unref(F.types);
    g_hash_table_unref(F.fd_path);
    g_hash_table_unref(F.fd_mount);
    g_ptr_array_unref(F.log);
    g_array_unref(F.made);
    g_array_unref(F.umount_ids);
    g_free(F.alias_from);
    g_free(F.alias_to);
    g_free(F.denied);
    g_assert_cmpint(g_rmdir(T_base), ==, 0);
    g_clear_pointer(&T_base, g_free);
    g_clear_pointer(&T_lxc, g_free);
    g_clear_pointer(&T_stage, g_free);
}

                                    
static guint
fake_count(const gchar *head)
{
    guint n = 0;
    for (guint i = 0; i < F.log->len; i++)
        n += g_str_has_prefix(g_ptr_array_index(F.log, i), head);
    return n;
}

                                                               
static gchar *
fake_umount_paths(void)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < F.log->len; i++) {
        const gchar *e = g_ptr_array_index(F.log, i);
        if (g_str_has_prefix(e, "umount ")) {
            const gchar *p = strchr(e + 7, ' ');
            g_string_append_printf(s, "%s%s", s->len ? "," : "", p ? p + 1 : "");
        }
    }
    return g_string_free(s, FALSE);
}

static gboolean
fake_alive(guint64 id)
{
    for (guint i = 0; i < F.mounts->len; i++)
        if (g_array_index(F.mounts, FakeMount, i).id == id)
            return g_array_index(F.mounts, FakeMount, i).alive;
    return FALSE;
}

                                                    
static gchar *
rp(const gchar *rel)
{
    return g_strdup_printf("%s/c/rootfs%s", T_lxc, rel);
}

static void
assert_left(GError *err, guint n)
{
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_autofree gchar *want = g_strdup_printf("%u volume mount(s) left under the container rootfs could not be removed", n);
    g_assert_cmpstr(err->message, ==, want);
}

                                                                                                   

                                                            
static void
test_residue_none(void)
{
    fake_begin();
    g_autofree gchar *self = rp("");
    fake_mount(3, self);                                                   
    guint cleared = 99;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 0);
    g_assert_cmpuint(F.umounts, ==, 0);
    g_assert_cmpuint(fake_count("walk "), ==, 0);
    g_assert_true(fake_alive(3));
    fake_end();
}

                                                                                                                  
                                                                                           
                        
static guint
fake_assert_two_phase(void)
{
    gint first_umount = -1;
    guint privs = 0;
    GArray *priv_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    for (guint i = 0; i < F.log->len; i++) {
        const gchar *e = g_ptr_array_index(F.log, i);
        gboolean is_priv = g_str_has_prefix(e, "private "), is_um = g_str_has_prefix(e, "umount ");
        if (!is_priv && !is_um)
            continue;
        g_assert_cmpuint(i, >=, 1);
        int fd = atoi(e + (is_priv ? 8 : 7));
        g_autofree gchar *mid = g_strdup_printf("mnt_id %d", fd);
        g_assert_cmpstr(g_ptr_array_index(F.log, i - 1), ==, mid);                           
        if (is_um && first_umount < 0)
            first_umount = (gint)i;
        if (is_priv) {
            g_assert_cmpint(first_umount, <, 0);                                                   
            guint64 id = g_ascii_strtoull(strchr(e + 8, ' ') + 1, NULL, 10);
            g_array_append_val(priv_ids, id);
            privs++;
        }
    }
    for (guint u = 0; u < F.umount_ids->len; u++) {                                                   
        gboolean covered = FALSE;
        for (guint64 cur = g_array_index(F.umount_ids, guint64, u); cur && !covered;) {
            for (guint k = 0; k < priv_ids->len && !covered; k++)
                covered = g_array_index(priv_ids, guint64, k) == cur;
            const FakeMount *m = fake_find(cur);
            cur = m ? m->parent : 0;
        }
        g_assert_true(covered);
    }
    g_array_unref(priv_ids);
    return privs;
}

                                                                                                  
                                                                                                        
                                                                                                           
                            
static void
test_residue_reverse_order_id_match(void)
{
    fake_begin();
    g_autofree gchar *a = rp("/mnt/a"), *b = rp("/mnt/b");
    fake_mount(20, a);
    fake_mount(21, b);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", b, a);
    g_assert_cmpstr(order, ==, want);
    g_assert_cmpuint(fake_assert_two_phase(), ==, 2);
    g_assert_cmpuint(F.umounts, ==, 2);
    g_assert_false(fake_alive(20) || fake_alive(21));
    fake_end();

    fake_begin();                                                                       
    g_autofree gchar *a2 = rp("/mnt/a"), *b2 = rp("/mnt/b");
    fake_mount(20, a2);
    fake_mount(21, b2);
    F.lie_id = 21;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 1);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *order2 = fake_umount_paths();
    g_assert_cmpstr(order2, ==, a2);
    g_assert_cmpuint(fake_assert_two_phase(), ==, 1);                                  
    g_assert_true(fake_alive(21));
    fake_end();
}

                                                                                                                      
                                                                                                       
                                                                                             
                                                                       
static void
test_residue_parent_shared_child_no_propagation(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *b = rp("/mnt/v17b"), *tp = rp("/mnt/v17b/sub");
    fake_mount_ex(346, b, 2, 1, "8:1", "/var/tmp/s17b");                                        
    fake_mount_ex(357, "/var/tmp/s17b/sub", 1, 380, "0:90", "/");                        
    fake_mount_ex(358, tp, 346, 380, "0:90", "/");                                          
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", tp, b);
    g_assert_cmpstr(order, ==, want);
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    g_assert_true(fake_alive(357));                                                 
    g_assert_cmpuint(fake_assert_two_phase(), ==, 1);                                         
    fake_end();
}

                                                                                                                       
                                                                                                   
                                                                                                        
                                                                              
static void
test_residue_moved_child_listed_first(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *c = rp("/mnt/m2/x"), *pm = rp("/mnt/m2");
    fake_mount_ex(40, c, 41, 381, "0:91", "/");                                                   
    fake_mount_ex(41, pm, 2, 1, "8:1", "/var/tmp/m2src");
    fake_mount_ex(50, "/var/tmp/m2src/x", 1, 381, "0:91", "/");
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", c, pm);
    g_assert_cmpstr(order, ==, want);
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    g_assert_true(fake_alive(50));
    g_assert_cmpuint(fake_assert_two_phase(), ==, 1);
    fake_end();
}

                                                                                                              
                                                                                                       
                                                                                                         
                                                                                    
static void
test_detach_running_shared_legacy_copy_no_propagation(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *v = rp("/mnt/v"), *tp = rp("/mnt/v/sub");
    fake_mount_ex(70, v, 2, 1, "8:1", "/srv/data");
    fake_mount_ex(71, "/srv/data/sub", 1, 383, "8:1", "/srv/data/a");
    fake_mount_ex(72, tp, 70, 383, "8:1", "/srv/data/a");
    GError *err = NULL;
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(T_lxc, "c", "lxc.uts.name = c\n", "/mnt/v", &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_cmpuint(set->lines->len, ==, 1);                                              
    g_assert_true(pcv_lxc_volume_unmount_attributed(T_lxc, "c", set, "/mnt/v", &err));
    g_assert_no_error(err);
    pcv_lxc_volume_set_free(set);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", tp, v);
    g_assert_cmpstr(order, ==, want);
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    g_assert_true(fake_alive(71));                                                    
    g_assert_cmpuint(fake_assert_two_phase(), ==, 1);
    fake_end();
}

                                                                                                 
                                                                     
static void
test_make_private_fail_parent_skips_subtree(void)
{
    fake_begin();
    g_autofree gchar *pp = rp("/mnt/p"), *cc = rp("/mnt/p/c"), *q = rp("/mnt/q");
    fake_mount(80, pp);
    fake_mount(81, cc);
    fake_mount(82, q);
    F.private_fail_id = 80;
    guint cleared = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 2);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *order = fake_umount_paths();
    g_assert_cmpstr(order, ==, q);
    g_assert_true(fake_alive(80) && fake_alive(81));
    fake_end();
}

                                                                                                                           
                                                                                                   
                                                                                                      
static void
test_residue_stack_slave_pair_cleared(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *a = rp("/mnt/a");
    fake_mount_ex(20, a, 2, 0, "8:1", "/srv/a");
    fake_mount_ex(21, a, 20, 0, "8:1", "/srv/b");
    fake_slave(20, 1);
    fake_slave(21, 1);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *seq = fake_pu_seq();
    g_assert_cmpstr(seq, ==, "p21,u21,p20,u20");
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    g_assert_false(fake_alive(20) || fake_alive(21));
    fake_end();
}

                                                                                                                  
                                                                                                               
                                                
static void
test_residue_stack_shared_covered_refused(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *a = rp("/mnt/a"), *q = rp("/mnt/q");
    fake_mount_ex(20, a, 2, 1, "8:1", "/srv/a");                                            
    fake_mount_ex(21, a, 20, 0, "8:1", "/srv/b");                                     
    fake_slave(21, 1);
    fake_mount_ex(22, "/srv/a", 1, 0, "0:92", "/");                                             
    fake_mount_ex(30, q, 2, 0, NULL, NULL);                                   
    guint cleared = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_autofree gchar *want = g_strdup_printf("2 volume mount(s) left under the container rootfs could not be removed; " PCV_LXC_VOLUME_STACK_SHARED_FMT,
                                             a);
    g_assert_cmpstr(err->message, ==, want);
    g_assert_null(strstr(err->message, "umount"));                                                          
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *seq = fake_pu_seq();
    g_assert_cmpstr(seq, ==, "p30,u30");                                               
    g_assert_true(fake_alive(20) && fake_alive(21) && fake_alive(22));
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    fake_end();
}

                                                                                                                            
                                                                                         
static void
test_detach_running_stack_same_dest(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *s0 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/a", 0), *s1 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/a", 1);
    fake_mount_ex(60, s0, 1, 0, "8:1", "/srv/a");                                                           
    fake_mount_ex(61, s1, 1, 0, "8:1", "/srv/b");
    g_autofree gchar *v = rp("/mnt/a");
    fake_mount_ex(70, v, 2, 0, "8:1", "/srv/a");
    fake_mount_ex(71, v, 70, 0, "8:1", "/srv/b");
    fake_slave(70, 1);
    fake_slave(71, 1);
    GError *err = NULL;
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(T_lxc, "c",
                                                           "lxc.uts.name = c\nlxc.mount.entry = /srv/a mnt/a none bind 0 0\n"
                                                           "lxc.mount.entry = /srv/b mnt/a none bind 0 0\n", "/mnt/a", &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_cmpuint(set->lines->len, ==, 2);
    g_assert_true(pcv_lxc_volume_unmount_attributed(T_lxc, "c", set, "/mnt/a", &err));
    g_assert_no_error(err);
    pcv_lxc_volume_set_free(set);
    g_autofree gchar *seq = fake_pu_seq();
    g_assert_cmpstr(seq, ==, "p71,u71,p70,u70");
    g_assert_false(fake_alive(70) || fake_alive(71));
    g_assert_true(fake_alive(60) && fake_alive(61));                                                  
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    fake_end();
}

                                                                                                                  
                                                                                                        
                                                                                                             
                                                                            
static void
test_detach_running_stack_shared_refused(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    g_autofree gchar *s0 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/a", 0), *s1 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/a", 1);
    fake_mount_ex(60, s0, 1, 0, "8:1", "/srv/a");
    fake_mount_ex(61, s1, 1, 0, "8:1", "/srv/b");
    g_autofree gchar *v = rp("/mnt/a");
    fake_mount_ex(70, v, 2, 1, "8:1", "/srv/a");                                               
    fake_mount_ex(71, v, 70, 0, "8:1", "/srv/b");                                     
    fake_slave(71, 1);
    fake_mount_ex(72, "/srv/a", 1, 0, "0:93", "/");                                             
    GError *err = NULL;
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(T_lxc, "c",
                                                           "lxc.uts.name = c\nlxc.mount.entry = /srv/a mnt/a none bind 0 0\n"
                                                           "lxc.mount.entry = /srv/b mnt/a none bind 0 0\n", "/mnt/a", &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_false(pcv_lxc_volume_unmount_attributed(T_lxc, "c", set, "/mnt/a", &err));
    pcv_lxc_volume_set_free(set);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_autofree gchar *want = g_strdup_printf(PCV_LXC_VOLUME_STACK_SHARED_FMT, v);
    g_assert_cmpstr(err->message, ==, want);
    g_assert_null(strstr(err->message, "umount"));                                             
    g_clear_error(&err);
    g_autofree gchar *seq = fake_pu_seq();
    g_assert_cmpstr(seq, ==, "");                                                 
    g_assert_true(fake_alive(70) && fake_alive(71) && fake_alive(72));
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    fake_end();
}

                                                                                                           
                                                                                                             
static void
test_residue_stack_bottom_under_shared_parent(void)
{
    fake_begin();
    fake_shared(1, 1, "8:1");
    fake_shared(2, 5, "0:2");                                                             
    fake_mount_ex(9, "/mirror", 1, 5, "0:2", "/");                            
    g_autofree gchar *a = rp("/mnt/a");
    fake_mount_ex(20, a, 2, 0, "8:1", "/srv/a");
    fake_mount_ex(21, a, 20, 0, "8:1", "/srv/b");
    fake_slave(20, 1);
    fake_slave(21, 1);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *seq = fake_pu_seq();
    g_assert_cmpstr(seq, ==, "p21,u21,p20,u20");                                                          
    g_assert_cmpuint(fake_find(2)->peer, ==, 5);
    g_assert_cmpuint(fake_find(9)->peer, ==, 5);
    g_assert_cmpuint(fake_count("propagated "), ==, 0);
    fake_end();
}

                                                             
static void
test_residue_shadowed_deep_mount(void)
{
    fake_begin();
    g_autofree gchar *deep = rp("/mnt/a/b"), *shallow = rp("/mnt/a");
    fake_mount(30, deep);                                        
    fake_mount(31, shallow);                                            
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
                                                                                                 
    g_assert_cmpuint(fake_count("walk "), <=, 5);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", shallow, deep);
    g_assert_cmpstr(order, ==, want);
    fake_end();
}

                                                                
static void
test_residue_id_mismatch_skipped(void)
{
    fake_begin();
    g_autofree gchar *x = rp("/mnt/x");
    fake_mount(40, x);
    F.lie_id = 40;
    guint cleared = 7;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 1);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 0);
    g_assert_cmpuint(F.umounts, ==, 0);
    g_assert_cmpuint(fake_count("private "), ==, 0);
    g_assert_true(fake_alive(40));
    fake_end();
}

                                                                   
static void
test_residue_skip_until_next_success(void)
{
    fake_begin();
    g_autofree gchar *a = rp("/mnt/a"), *b = rp("/mnt/b");
    fake_mount(45, a);
    fake_mount(46, b);
    F.lie_once_id = 46;
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", a, b);
    g_assert_cmpstr(order, ==, want);
    fake_end();
}

                                               
static void
test_residue_private_failure_no_umount(void)
{
    fake_begin();
    g_autofree gchar *p = rp("/mnt/p"), *q = rp("/mnt/q");
    fake_mount(50, p);
    fake_mount(51, q);
    F.private_fail_id = 50;
    guint cleared = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 1);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *order = fake_umount_paths();
    g_assert_cmpstr(order, ==, q);                               
    g_assert_true(fake_alive(50));
    fake_end();
}

                                                         
static void
test_residue_prefix_boundary(void)
{
    fake_begin();
    g_autofree gchar *self = rp(""), *in = rp("/in");
    g_autofree gchar *sib = g_strdup_printf("%s/c/rootfs2/x", T_lxc);
    fake_mount(60, self);
    fake_mount(61, sib);
    fake_mount(62, in);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *order = fake_umount_paths();
    g_assert_cmpstr(order, ==, in);
    g_assert_cmpuint(fake_count("walk "), ==, 2);                                                       
    g_assert_true(fake_alive(60) && fake_alive(61));
    fake_end();
}

                                                                                          
static void
test_residue_prefix_from_fd_path(void)
{
    fake_begin();
    g_autofree gchar *a = rp("/mnt/a");
    fake_mount(70, a);
    g_autofree gchar *messy = g_strdup_printf("/%s//lxc/", T_base);                        
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(messy, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 1);
    g_assert_false(fake_alive(70));
    fake_end();

    fake_begin();                                                                           
    g_autofree gchar *a2 = rp("/mnt/a");
    fake_mount(71, a2);
    F.alias_from = g_strdup_printf("%s/link", T_base);
    F.alias_to = g_strdup(T_lxc);
    g_assert_true(pcv_lxc_rootfs_residue_clear(F.alias_from, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 1);
    g_assert_false(fake_alive(71));
    fake_end();
}

                                                                                 
                                     
static void
test_residue_staging_prefix_and_dir_removal(void)
{
    fake_begin();
    g_autofree gchar *sc = g_strdup_printf("%s/c", T_stage), *sc2 = g_strdup_printf("%s/c2", T_stage);
    g_autofree gchar *k = g_strdup_printf("%s/abcd.0", sc), *k2 = g_strdup_printf("%s/ef01.0", sc2);
    fake_dir(T_stage, "c", "c2", NULL);
    fake_dir(sc, "abcd.0", NULL);
    fake_dir(sc2, "ef01.0", NULL);
    fake_dir(k, NULL);
    fake_dir(k2, NULL);
    fake_mount(80, k);
    fake_mount(81, k2);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 1);
    g_autofree gchar *order = fake_umount_paths();
    g_assert_cmpstr(order, ==, k);
    g_autofree gchar *rm_k = g_strdup_printf("remove %s", k), *rm_c = g_strdup_printf("remove %s", sc);
    gint at_k = -1, at_c = -1;
    for (guint i = 0; i < F.log->len; i++) {
        const gchar *e = g_ptr_array_index(F.log, i);
        if (strcmp(e, rm_k) == 0)
            at_k = (gint)i;
        if (strcmp(e, rm_c) == 0)
            at_c = (gint)i;
        if (!g_str_has_prefix(e, "read"))
            g_assert_null(strstr(e, sc2));                                            
    }
    g_assert_cmpint(at_k, >=, 0);
    g_assert_cmpint(at_c, >, at_k);
    g_assert_cmpuint(fake_count("remove "), ==, 2);
    g_assert_true(fake_alive(81));
    g_assert_true(g_hash_table_contains(F.dirs, sc2));
    fake_end();

    fake_begin();                                                                  
    g_autofree gchar *sc_b = g_strdup_printf("%s/c", T_stage), *k_b = g_strdup_printf("%s/abcd.0", sc_b);
    fake_dir(T_stage, "c", NULL);
    fake_dir(sc_b, "abcd.0", NULL);
    fake_mount(82, k_b);
    F.lie_id = 82;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 1);
    g_clear_error(&err);
    g_assert_cmpuint(fake_count("remove "), ==, 0);
    g_assert_cmpuint(fake_count("list "), ==, 0);
    fake_end();
}

                                                                                                           
                                                                                            
                                                                                             
                                                                                          
                                                                                    
             
static void
test_residue_attempt_cap(void)
{
    fake_begin();
    g_autofree gchar *s = rp("/mnt/s");
    fake_mount(90, s);
    F.umount_sticky = TRUE;
    guint cleared = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 1);
    g_clear_error(&err);
    g_assert_cmpuint(F.umounts, ==, 1 + PCV_LXC_RESIDUE_MAX_ATTEMPTS);
    g_assert_cmpuint(fake_count("private "), ==, 1 + PCV_LXC_RESIDUE_MAX_ATTEMPTS);
    g_assert_cmpuint(fake_count("walk "), ==, 2 * (1 + PCV_LXC_RESIDUE_MAX_ATTEMPTS));
    fake_end();

    for (guint mode = 0; mode < 3; mode++) {
        fake_begin();
        for (guint i = 0; i < 70; i++) {
            g_autofree gchar *rel = g_strdup_printf("/mnt/m%u", i);
            g_autofree gchar *mp = rp(rel);
            fake_mount(1000 + i, mp);
        }
        if (mode == 0)
            F.umount_fail = TRUE;                                                                      
        else if (mode == 1)
            F.lie_all = TRUE;                                              
        else
            F.umount_sticky = TRUE;                                            
        g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
        assert_left(err, 70);
        g_clear_error(&err);
        const guint want_umounts = mode == 0 ? 70 : mode == 1 ? 0 : 70 + PCV_LXC_RESIDUE_MAX_ATTEMPTS;
        g_assert_cmpuint(F.umounts, ==, want_umounts);
        if (mode < 2) {
            g_assert_cmpuint(cleared, ==, 0);
            g_assert_cmpuint(fake_count("private "), ==, mode == 0 ? 70 : 0);
            g_assert_cmpuint(fake_count("walk "), ==, mode == 0 ? 70 + 70 : 70);
        }
        fake_end();
    }
}

                                                                                        
                                                              
                                                                                          
                                                 
                                                                                                               
                        
                                                                                                 
                                                                                                                         
                                                          
static void
test_residue_many_members_one_call(void)
{
    const guint N = 70;
    guint cleared = 0;
    GError *err = NULL;

    fake_begin();                                                 
    g_autofree gchar *s = rp("/mnt/s");
    fake_mount(3000, s);
    for (guint i = 1; i < N; i++) {
        g_autofree gchar *rel = g_strdup_printf("/mnt/s/c%u", i);
        g_autofree gchar *mp = rp(rel);
        fake_mount(3000 + i, mp);
    }
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, N);
    g_assert_cmpuint(F.umounts, ==, N);
    g_assert_cmpuint(fake_count("private "), ==, 1);
    for (guint i = 0; i < N; i++)
        g_assert_false(fake_alive(3000 + i));
    fake_end();

    fake_begin();                                              
    for (guint i = 0; i < N; i++) {
        g_autofree gchar *rel = g_strdup_printf("/mnt/m%u", i);
        g_autofree gchar *mp = rp(rel);
        fake_mount(4000 + i, mp);
    }
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, N);
    g_assert_cmpuint(F.umounts, ==, N);
    g_assert_cmpuint(fake_count("private "), ==, N);
    fake_end();

    fake_begin();                                                             
    fake_shared(1, 1, "8:1");
    g_autofree gchar *s0 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/s", 0);
    fake_mount_ex(60, s0, 1, 0, "8:1", "/srv/s");                                          
    g_autofree gchar *v = rp("/mnt/s");
    fake_mount_ex(5000, v, 2, 0, "8:1", "/srv/s");
    fake_slave(5000, 1);
    for (guint i = 1; i < N; i++) {
        g_autofree gchar *rel = g_strdup_printf("/mnt/s/c%u", i), *root = g_strdup_printf("/srv/s/c%u", i);
        g_autofree gchar *mp = rp(rel);
        fake_mount_ex(5000 + i, mp, 5000, 0, "8:1", root);
        fake_slave(5000 + i, 1);
    }
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(T_lxc, "c", "lxc.mount.entry = /srv/s mnt/s none rbind 0 0\n", "/mnt/s",
                                                           &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_true(pcv_lxc_volume_unmount_attributed(T_lxc, "c", set, "/mnt/s", &err));
    g_assert_no_error(err);
    pcv_lxc_volume_set_free(set);
    g_assert_cmpuint(F.umounts, ==, N);
    for (guint i = 0; i < N; i++)
        g_assert_false(fake_alive(5000 + i));
    g_assert_true(fake_alive(60));                                                                    
    fake_end();

    fake_begin();                                                              
    g_autofree gchar *sc = g_strdup_printf("%s/c", T_stage);
    g_autofree gchar *k0 = pcv_lxc_volstage_slot(T_stage, "c", "mnt/s", 0);
    fake_dir(sc, NULL);
    fake_mount(6000, k0);
    for (guint i = 1; i < N; i++) {
        g_autofree gchar *mp = g_strdup_printf("%s/c%u", k0, i);
        fake_mount(6000 + i, mp);
    }
    g_assert_true(pcv_lxc_volume_staging_unmount("c", "mnt/s", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(F.umounts, ==, N);
    for (guint i = 0; i < N; i++)
        g_assert_false(fake_alive(6000 + i));
    fake_end();
}

                                              
static void
fake_type(const gchar *path, gchar kind)
{
    g_hash_table_insert(F.types, g_strdup(path), GINT_TO_POINTER((gint)kind));
}

                                                                                   
                                                                
static void
test_residue_file_mount_points(void)
{
    fake_begin();
    g_autofree gchar *file = rp("/etc/app.conf");
    g_autofree gchar *sc = g_strdup_printf("%s/c", T_stage), *k = g_strdup_printf("%s/abcd.0", sc);
    fake_dir(T_stage, "c", NULL);
    fake_dir(sc, "abcd.0", NULL);
    fake_type(file, 'f');
    fake_type(k, 'f');
    fake_mount(120, file);
    fake_mount(121, k);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 2);
    g_autofree gchar *order = fake_umount_paths();
    g_autofree gchar *want = g_strdup_printf("%s,%s", file, k);
    g_assert_cmpstr(order, ==, want);
    g_assert_cmpuint(fake_count("private "), ==, 2);
    g_assert_false(fake_alive(120) || fake_alive(121));
    g_assert_false(g_hash_table_contains(F.dirs, sc));                                   
    fake_end();
}

                                                                     
static void
test_residue_symlink_leaf_not_followed(void)
{
    fake_begin();
    g_autofree gchar *leaf = rp("/mnt/l"), *dir = rp("/lnk"), *under = rp("/lnk/x");
    fake_type(leaf, 'l');
    fake_type(dir, 'l');
    fake_mount(130, leaf);
    fake_mount(131, under);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    assert_left(err, 2);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 0);
    g_assert_cmpuint(fake_count("mnt_id "), ==, 0);
    g_assert_cmpuint(fake_count("private "), ==, 0);
    g_assert_cmpuint(F.umounts, ==, 0);
    fake_end();
}

                                                                               
static void
test_residue_missing_rootfs_ok(void)
{
    fake_begin();
    g_autofree gchar *rootfs = rp("");
    g_hash_table_remove(F.dirs, rootfs);
    g_autofree gchar *sc = g_strdup_printf("%s/c", T_stage), *k = g_strdup_printf("%s/k.0", sc);
    fake_dir(T_stage, "c", NULL);
    fake_dir(sc, "k.0", NULL);
    fake_mount(100, k);
    guint cleared = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(cleared, ==, 1);
    g_assert_false(fake_alive(100));
    g_assert_false(g_hash_table_contains(F.dirs, sc));
    fake_end();
}

                                                                  
static void
test_residue_prefix_open_error(void)
{
    fake_begin();
    g_autofree gchar *rootfs = rp(""), *a = rp("/mnt/a");
    fake_mount(110, a);
    F.denied = g_strdup(rootfs);
    guint cleared = 5;
    GError *err = NULL;
    g_assert_false(pcv_lxc_rootfs_residue_clear(T_lxc, "c", &cleared, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_autofree gchar *want = g_strdup_printf("cannot open %s: Permission denied", rootfs);
    g_assert_cmpstr(err->message, ==, want);
    g_clear_error(&err);
    g_assert_cmpuint(cleared, ==, 0);
    g_assert_cmpuint(F.umounts, ==, 0);
    fake_end();
}

                                                                                           
                                                                                                    
static void
test_open_leaf_real_types(void)
{
    g_autofree gchar *base = g_dir_make_tmp("pcv-leaf-XXXXXX", NULL);
    g_assert_nonnull(base);
    g_autofree gchar *root = g_build_filename(base, "root", NULL), *outside = g_build_filename(base, "outside", NULL);
    g_autofree gchar *d = g_build_filename(root, "d", NULL), *sub = g_build_filename(d, "sub", NULL);
    g_autofree gchar *file = g_build_filename(d, "file", NULL), *top = g_build_filename(root, "top", NULL);
    g_autofree gchar *link = g_build_filename(d, "link", NULL), *abs = g_build_filename(d, "abs", NULL);
    g_autofree gchar *fifo = g_build_filename(d, "fifo", NULL), *lnkdir = g_build_filename(root, "lnkdir", NULL);
    g_assert_cmpint(g_mkdir_with_parents(sub, 0700), ==, 0);
    g_assert_true(g_file_set_contents(file, "x", 1, NULL));
    g_assert_true(g_file_set_contents(top, "x", 1, NULL));
    g_assert_true(g_file_set_contents(outside, "secret", 6, NULL));
    g_assert_cmpint(symlink("file", link), ==, 0);
    g_assert_cmpint(symlink(outside, abs), ==, 0);
    g_assert_cmpint(symlink("d", lnkdir), ==, 0);
    g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
    int rfd = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    g_assert_cmpint(rfd, >=, 0);

    static const struct { const gchar *rel; gchar want; } ok_cases[] = {
        { "/d/file", 'f' }, { "/d/sub", 'd' }, { "/top", 'f' }, { "/d", 'd' },
    };
    for (guint i = 0; i < G_N_ELEMENTS(ok_cases); i++) {
        GError *err = NULL;
        int fd = pcv_lxc_rootfs_open_leaf(rfd, ok_cases[i].rel, &err);
        g_assert_no_error(err);
        g_assert_cmpint(fd, >=, 0);
        struct stat st;
        g_assert_cmpint(fstat(fd, &st), ==, 0);
        g_assert_true(ok_cases[i].want == 'f' ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode));
        close(fd);
    }
    static const struct { const gchar *rel; gboolean bad_path; gint io; } bad_cases[] = {
        { "/d/link", TRUE, 0 },                                                  
        { "/d/abs", TRUE, 0 },                                                     
        { "/lnkdir/file", TRUE, 0 },                                 
        { "/d/fifo", FALSE, G_IO_ERROR_NOT_REGULAR_FILE },
        { "/d/missing", FALSE, G_IO_ERROR_NOT_FOUND },
        { "/", TRUE, 0 }, { "/d/", TRUE, 0 }, { "/d/..", TRUE, 0 },
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad_cases); i++) {
        GError *err = NULL;
        g_assert_cmpint(pcv_lxc_rootfs_open_leaf(rfd, bad_cases[i].rel, &err), ==, -1);
        if (bad_cases[i].bad_path)
            g_assert_error(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
        else
            g_assert_error(err, G_IO_ERROR, bad_cases[i].io);
        g_clear_error(&err);
    }
    close(rfd);
    const gchar *rm[] = { fifo, abs, link, lnkdir, file, top, outside, sub, d, root, base };
    for (guint i = 0; i < G_N_ELEMENTS(rm); i++)
        g_assert_cmpint(g_remove(rm[i]), ==, 0);
}

                                                                                                              
                                                                                                          
                                                                                                     
                                                                                                                         
                                                                                                
                                                                                                    
                                                                                                                   

typedef struct {
    gchar      *base, *lxc, *stage, *rt, *src;                                           
    PcvLxcMountOps ops;                                              
    const PcvLxcMountOps *real;
    GPtrArray  *log;                                                                                                                                
                                                                                                                     
    GHashTable *fail;                                                                                       
    GHashTable *fd_src;                                                             
    gboolean    real_dir, clone_fail;
    GPtrArray  *child;                                                                
    int         child_mode;                                                      
    int         fd_probe;                                                                    
    gboolean    slow_root;                                                                                                  
    GMutex      mu;                                                                      
} StageEnv;

static StageEnv SE;

static void se_log(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);
static void
se_log(const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_mutex_lock(&SE.mu);
    g_ptr_array_add(SE.log, line);
    g_mutex_unlock(&SE.mu);
}

static gchar *
se_read_mountinfo(GError **error)
{
    (void)error;
    return g_strdup("");
}

static int
se_open_dir(const gchar *path, GError **error)
{
    g_autofree gchar *dir = g_path_get_dirname(path);
    if (g_str_equal(dir, SE.stage))                                                                  
        se_log("residue");
    return SE.real->open_dir(path, error);
}

static int
se_walk(int root_fd, const gchar *rel, gboolean create, GError **error)
{
    g_assert_true(create);                                                                
    se_log("target walk %s", rel);
    return SE.real->walk(root_fd, rel, create, error);
}

static int
se_open_leaf(int root_fd, const gchar *rel, GError **error)
{
    se_log("target leaf %s", rel);
    return SE.real->open_leaf(root_fd, rel, error);
}

static int
se_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    g_assert_null(canon_out);
    se_log("source_open %s", host_path);
    gchar kind = (gchar)GPOINTER_TO_INT(g_hash_table_lookup(SE.fail, host_path));
    if (kind == 'b') {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, "host_path must not traverse symbolic links");
        return -1;
    }
    if (kind == 'n' || kind == 'e') {
        g_set_error_literal(error, G_IO_ERROR, kind == 'n' ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_PERMISSION_DENIED, "cannot open");
        return -1;
    }
    int fd = open(host_path, O_PATH | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    g_mutex_lock(&SE.mu);
    g_hash_table_insert(SE.fd_src, GINT_TO_POINTER(fd), g_strdup(host_path));
    g_mutex_unlock(&SE.mu);
    return fd;
}

static int
se_prepare_dir(int parent_fd, const gchar *name, guint mode, gint64 gid, GError **error)
{
    se_log("prepare_dir %s %o %" G_GINT64_FORMAT, name, mode, gid);
    if (SE.slow_root && mode == 0711) {                                                             
        se_log("stage-root enter");
        g_usleep(100 * 1000);
        int fd = SE.real->prepare_dir(parent_fd, name, mode, gid, error);
        se_log("stage-root exit");
        return fd;
    }
    if (SE.real_dir)
        return SE.real->prepare_dir(parent_fd, name, mode, gid, error);
    if (mkdirat(parent_fd, name, 0700) < 0)
        g_assert_cmpint(errno, ==, EEXIST);
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

static int
se_prepare_slot(int dir_fd, const gchar *name, gboolean is_dir, GError **error)
{
    se_log("prepare_slot %s %s", name, is_dir ? "dir" : "file");
    return SE.real->prepare_slot(dir_fd, name, is_dir, error);
}

static gboolean
se_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error)
{
    g_assert_null(mnt_id_out);                                                        
    g_mutex_lock(&SE.mu);
    g_autofree gchar *src = g_strdup(g_hash_table_lookup(SE.fd_src, GINT_TO_POINTER(src_fd)));
    g_mutex_unlock(&SE.mu);
    g_assert_nonnull(src);                                                                       
    struct stat a, b;
    g_assert_cmpint(fstat(src_fd, &a), ==, 0);
    g_assert_cmpint(stat(src, &b), ==, 0);
    g_assert_true(a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    g_assert_cmpint(fcntl(target_fd, F_GETFD), >=, 0);
    g_autofree gchar *rtcfg = g_build_filename(SE.rt, "c1", "config", NULL);
    se_log("mount_clone %s %u rt=%d", src, flags, g_file_test(rtcfg, G_FILE_TEST_EXISTS));
    if (SE.clone_fail) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "mount_setattr(MS_SLAVE): Invalid argument");
        return FALSE;
    }
    return TRUE;
}

static gboolean
se_child(const gchar * const *argv, gchar **stderr_out, GError **error)
{
    g_ptr_array_add(SE.child, g_strjoinv(" ", (gchar **)argv));
    if (SE.child_mode == 1) {                                  
        if (stderr_out)
            *stderr_out = g_strdup("chown: changing group: Operation not permitted\n");
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Child process exited with code 1");
        return FALSE;
    }
    if (SE.child_mode == 2) {                                                                   
        g_assert_true(g_str_has_prefix(argv[1], "0:"));
        g_assert_cmpint(lchown(argv[3], (uid_t)-1, (gid_t)g_ascii_strtoull(argv[1] + 2, NULL, 10)), ==, 0);
    }
    return TRUE;
}

                                                                     
static void
se_config(const gchar *body)
{
    g_auto(GStrv) parts = g_strsplit(body, "@S@", -1);
    g_autofree gchar *text = g_strjoinv(SE.src, parts);
    g_autofree gchar *path = g_build_filename(SE.lxc, "c1", "config", NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static void
se_begin(void)
{
    memset(&SE, 0, sizeof SE);
    g_mutex_init(&SE.mu);
    SE.fd_probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(SE.fd_probe, >=, 0);
    close(SE.fd_probe);
    SE.base = g_dir_make_tmp("pcv-vstage-XXXXXX", NULL);
    g_assert_nonnull(SE.base);
    SE.lxc = g_build_filename(SE.base, "lxc", NULL);
    SE.stage = g_build_filename(SE.base, "volstage", NULL);
    SE.rt = g_build_filename(SE.base, "rt", NULL);
    SE.src = g_build_filename(SE.base, "src", NULL);
    g_autofree gchar *ctr = g_build_filename(SE.lxc, "c1", "rootfs", NULL), *a = g_build_filename(SE.src, "a", NULL),
                     *b = g_build_filename(SE.src, "b", NULL), *f = g_build_filename(SE.src, "f", NULL);
    g_assert_cmpint(g_mkdir_with_parents(ctr, 0755), ==, 0);                                             
    g_assert_cmpint(g_mkdir_with_parents(a, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(b, 0700), ==, 0);
    g_assert_true(g_file_set_contents(f, "file volume\n", -1, NULL));
    SE.real = pcv_lxc_volume_real_ops_for_test();
    SE.ops = *SE.real;
    SE.ops.read_mountinfo = se_read_mountinfo;
    SE.ops.open_dir = se_open_dir;
    SE.ops.walk = se_walk;
    SE.ops.open_leaf = se_open_leaf;
    SE.ops.source_open = se_source_open;
    SE.ops.prepare_dir = se_prepare_dir;
    SE.ops.prepare_slot = se_prepare_slot;
    SE.ops.mount_clone = se_mount_clone;
    SE.log = g_ptr_array_new_with_free_func(g_free);
    SE.child = g_ptr_array_new_with_free_func(g_free);
    SE.fail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    SE.fd_src = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    pcv_lxc_volume_set_ops_for_test(&SE.ops);
    pcv_lxc_volume_set_roots_for_test(SE.stage, SE.rt);
    pcv_lxc_volume_set_storage_child_for_test(se_child);
}

static int
se_rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

static void
se_end(void)
{
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    pcv_lxc_volume_set_storage_child_for_test(NULL);
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(probe, ==, SE.fd_probe);                                           
    close(probe);
    g_assert_cmpint(nftw(SE.base, se_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_ptr_array_unref(SE.log);
    g_ptr_array_unref(SE.child);
    g_hash_table_unref(SE.fail);
    g_hash_table_unref(SE.fd_src);
    g_free(SE.base); g_free(SE.lxc); g_free(SE.stage); g_free(SE.rt); g_free(SE.src);
    g_mutex_clear(&SE.mu);
}

                                    
static gchar *
se_log_text(void)
{
    g_ptr_array_add(SE.log, NULL);
    gchar *s = g_strjoinv("\n", (gchar **)SE.log->pdata);
    g_ptr_array_remove_index(SE.log, SE.log->len - 1);
    return s;
}

static guint
se_count(const gchar *head)
{
    guint n = 0;
    for (guint i = 0; i < SE.log->len; i++)
        n += g_str_has_prefix(g_ptr_array_index(SE.log, i), head);
    return n;
}

                                                            
static gchar *
se_k(const gchar *dest, guint ord)
{
    g_autofree gchar *slot = pcv_lxc_volstage_slot(SE.stage, "c1", dest, ord);
    return g_strdup(strrchr(slot, '/') + 1);
}

static gchar *
se_path(const gchar *a, const gchar *b)
{
    return g_build_filename(SE.base, a, b, NULL);
}

static guint
se_mode(const gchar *path)
{
    struct stat st;
    g_assert_cmpint(lstat(path, &st), ==, 0);
    return (guint)(st.st_mode & 07777);
}

                                                                                              
                                                                                                      
                                                                                                     
                              
static void
test_start_prepare_order(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\n"
              "# 볼륨 줄의 원본 문자열은 살아 있는 줄에만 있다\n"
              "lxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n"
              "lxc.mount.entry = proc proc proc nodev,noexec,nosuid 0 0\n"
              "lxc.mount.entry = @S@/b mnt/b none rbind 0 0\n"
              "lxc.mount.entry = @S@/f etc/f none bind,ro,create=file 0 0\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_autofree gchar *ka = se_k("mnt/a", 0), *kb = se_k("mnt/b", 0), *kf = se_k("etc/f", 0);
    g_autofree gchar *want = g_strdup_printf(
        "residue\n"
        "source_open %1$s/a\ntarget walk /mnt\ntarget leaf /a\ntarget leaf /a\n"
        "prepare_dir volstage 711 0\nprepare_dir c1 700 0\nprepare_slot %2$s dir\nmount_clone %1$s/a 1 rt=0\n"
        "source_open %1$s/b\ntarget walk /mnt\ntarget leaf /b\ntarget leaf /b\nprepare_slot %3$s dir\nmount_clone %1$s/b 2 rt=0\n"
        "source_open %1$s/f\ntarget walk /etc\ntarget leaf /f\ntarget leaf /f\nprepare_slot %4$s file\nmount_clone %1$s/f 1 rt=0",
        SE.src, ka, kb, kf);
    g_autofree gchar *got = se_log_text();
    g_assert_cmpstr(got, ==, want);

    g_autofree gchar *rtcfg = se_path("rt", "c1/config"), *rt = NULL;
    g_assert_true(g_file_get_contents(rtcfg, &rt, NULL, NULL));
    g_assert_null(strstr(rt, SE.src));                                               
    g_autofree gchar *prefix = g_strdup_printf("%s/c1/", SE.stage);
    GPtrArray *ents = pcv_lxc_config_mount_entries(rt);
    const gchar *dests[] = { "mnt/a", "mnt/b", "etc/f" };
    guint vol = 0;
    g_assert_cmpuint(ents->len, ==, 4);
    for (guint i = 0; i < ents->len; i++) {
        const PcvLxcMountEntry *e = ents->pdata[i];
        if (!pcv_lxc_mount_entry_is_volume(e)) {
            g_assert_cmpstr(e->source, ==, "proc");
            continue;
        }
        g_autofree gchar *slot = pcv_lxc_volstage_slot(SE.stage, "c1", dests[vol++], 0);
        g_assert_true(g_str_has_prefix(e->source, prefix));
        g_assert_cmpstr(e->source, ==, slot);
    }
    g_assert_cmpuint(vol, ==, 3);
    g_ptr_array_unref(ents);
    g_autofree gchar *sa = se_path("volstage/c1", ka), *sb = se_path("volstage/c1", kb), *sf = se_path("volstage/c1", kf);
    g_assert_true(g_file_test(sa, G_FILE_TEST_IS_DIR) && g_file_test(sb, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(sf, G_FILE_TEST_IS_REGULAR));
    g_assert_cmpuint(se_mode(sa), ==, 0700);
    g_assert_cmpuint(se_mode(sf), ==, 0600);
    se_end();
}

                                                                                                                         
static void
test_start_prepare_modes(void)
{
    for (guint u = 0; u < 2; u++) {
        se_begin();
        g_autofree gchar *body = g_strdup_printf("lxc.uts.name = c1\n%s"
                                                 "lxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n",
                                                 u ? "lxc.idmap = u 0 100000 65536\nlxc.idmap = g 0 100000 65536\n" : "");
        se_config(body);
        GError *err = NULL;
        g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
        g_assert_no_error(err);
        guint at = 0;                                                                   
        while (at < SE.log->len && !g_str_has_prefix(g_ptr_array_index(SE.log, at), "prepare_dir "))
            at++;
        g_assert_cmpuint(at + 1, <, SE.log->len);
        g_assert_cmpstr(g_ptr_array_index(SE.log, at), ==, "prepare_dir volstage 711 0");
        g_assert_cmpstr(g_ptr_array_index(SE.log, at + 1), ==, u ? "prepare_dir c1 710 100000" : "prepare_dir c1 700 0");
        g_assert_cmpuint(se_count("prepare_dir "), ==, 2);
        se_end();
    }
}

                                                                                                            
                                                                                                   
static void
test_prepare_dir_chown_via_child(void)
{
    se_begin();
    mode_t old_mask = umask(077);                                                             
    int base = open(SE.base, O_PATH | O_DIRECTORY | O_CLOEXEC);
    g_assert_cmpint(base, >=, 0);
    char real[PATH_MAX];
    g_assert_nonnull(realpath(SE.base, real));
    GError *err = NULL;
    g_autofree gchar *want = g_strdup_printf("chown 0:100000 -- %s/c1", real);
    g_assert_cmpint(SE.real->prepare_dir(base, "c1", 0710, 100000, &err), ==, -1);                          
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH);
    g_assert_cmpstr(err->message, ==, "volume staging directory has unexpected owner or mode");
    g_clear_error(&err);
    g_assert_cmpuint(SE.child->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(SE.child, 0), ==, want);
    g_autofree gchar *c1 = se_path("c1", NULL);
    g_assert_cmpint(g_rmdir(c1), ==, 0);
    SE.child_mode = 1;                                      
    g_assert_cmpint(SE.real->prepare_dir(base, "c1", 0710, 100000, &err), ==, -1);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_true(g_str_has_prefix(err->message, "chown failed: "));
    g_clear_error(&err);
    g_assert_cmpuint(SE.child->len, ==, 2);
    g_assert_cmpint(g_rmdir(c1), ==, 0);
    SE.child_mode = 0;
    int fd = SE.real->prepare_dir(base, "c1p", 0700, 0, &err);                           
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    fd = SE.real->prepare_dir(base, "vs", 0711, 0, &err);                               
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree gchar *c1p = se_path("c1p", NULL), *vs = se_path("vs", NULL);
    g_assert_cmpuint(se_mode(c1p), ==, 0700);
    g_assert_cmpuint(se_mode(vs), ==, 0711);
    g_assert_cmpuint(SE.child->len, ==, 2);
    fd = SE.real->prepare_dir(base, "c1p", 0700, 0, &err);                          
    g_assert_no_error(err);
    close(fd);
    g_assert_cmpint(g_chmod(c1p, 0755), ==, 0);                                                
    g_assert_cmpint(SE.real->prepare_dir(base, "c1p", 0700, 0, &err), ==, -1);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH);
    g_clear_error(&err);
    g_assert_cmpuint(se_mode(c1p), ==, 0755);
    gid_t ok_gid = geteuid() == 0 ? 100000 : getegid();                                                 
    if (ok_gid > 0) {
        SE.child_mode = 2;
        fd = SE.real->prepare_dir(base, "c2", 0710, ok_gid, &err);
        g_assert_no_error(err);
        g_assert_cmpint(fd, >=, 0);
        struct stat st;
        g_assert_cmpint(fstat(fd, &st), ==, 0);
        g_assert_cmpuint(st.st_gid, ==, ok_gid);
        g_assert_cmpuint(st.st_mode & 07777, ==, 0710);
        close(fd);
        g_assert_cmpuint(SE.child->len, ==, 3);
    }
    close(base);
    umask(old_mask);
    se_end();

    se_begin();                                                                                      
    se_config("lxc.uts.name = c1\nlxc.idmap = g 0 100000 65536\nlxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n");
    SE.real_dir = TRUE;
    SE.child_mode = 1;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_true(g_str_has_prefix(err->message, "cannot prepare the volume staging for /mnt/a: chown failed: "));
    g_clear_error(&err);
    g_assert_cmpuint(SE.child->len, ==, 1);
    g_assert_cmpuint(se_count("mount_clone "), ==, 0);
    g_assert_cmpuint(se_count("residue"), ==, 2);                                
    g_autofree gchar *rtdir = se_path("rt", "c1"), *stdir = se_path("volstage", "c1");
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(stdir, G_FILE_TEST_EXISTS));
    se_end();
}

                                                                                             
static void
test_start_prepare_link_source_fails(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\nlxc.mount.entry = @S@/b mnt/b none bind 0 0\n");
    g_hash_table_insert(SE.fail, g_strdup_printf("%s/b", SE.src), GINT_TO_POINTER('b'));
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpstr(err->message, ==, "volume source changed since attach: /mnt/b");
    g_clear_error(&err);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);
    g_assert_cmpuint(se_count("residue"), ==, 2);
    g_assert_cmpstr(g_ptr_array_index(SE.log, SE.log->len - 1), ==, "residue");                                      
    g_autofree gchar *stdir = se_path("volstage", "c1"), *rtdir = se_path("rt", "c1");
    g_assert_false(g_file_test(stdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
    se_end();
}

                                                                                        
static void
test_start_prepare_missing_source_skips(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\n"
              "lxc.mount.entry = @S@/gone mnt/g none bind,optional 0 0\n"
              "lxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n"
              "lxc.mount.entry = @S@/deny mnt/d none bind 0 0\n");
    g_hash_table_insert(SE.fail, g_strdup_printf("%s/gone", SE.src), GINT_TO_POINTER('n'));
    g_hash_table_insert(SE.fail, g_strdup_printf("%s/deny", SE.src), GINT_TO_POINTER('e'));
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(se_count("source_open "), ==, 3);
    g_assert_cmpuint(se_count("prepare_slot "), ==, 1);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);
    g_autofree gchar *rtcfg = se_path("rt", "c1/config"), *rt = NULL;
    g_assert_true(g_file_get_contents(rtcfg, &rt, NULL, NULL));
    g_assert_null(strstr(rt, SE.src));
    g_autofree gchar *kg = pcv_lxc_volstage_slot(SE.stage, "c1", "mnt/g", 0), *kd = pcv_lxc_volstage_slot(SE.stage, "c1", "mnt/d", 0);
    g_autofree gchar *lg = g_strdup_printf("lxc.mount.entry = %s mnt/g none bind,optional 0 0\n", kg);
    g_autofree gchar *ld = g_strdup_printf("lxc.mount.entry = %s mnt/d none bind 0 0\n", kd);
    g_assert_nonnull(strstr(rt, lg));
    g_assert_nonnull(strstr(rt, ld));
    g_assert_false(g_file_test(kg, G_FILE_TEST_EXISTS));                                                
    se_end();
}

                                                                                                        
static void
test_start_prepare_mount_failure_aborts(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n");
    SE.clone_fail = TRUE;
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "cannot prepare the volume staging for /mnt/a: mount_setattr(MS_SLAVE): Invalid argument");
    g_clear_error(&err);
    g_autofree gchar *rtdir = se_path("rt", "c1"), *stdir = se_path("volstage", "c1");
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(stdir, G_FILE_TEST_EXISTS));
    se_end();
}

                                                                                                 
               
static void
test_start_prepare_refuses_foreign_stage_dir(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n");
    SE.real_dir = TRUE;
    g_assert_cmpint(g_mkdir(SE.stage, 0700), ==, 0);
    g_assert_cmpint(g_chmod(SE.stage, 0755), ==, 0);
    GError *err = NULL;
    g_autofree gchar *want = g_strdup_printf("staging path %s has unexpected type, owner or mode; refusing to start", SE.stage);
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, want);
    g_clear_error(&err);
    g_assert_cmpuint(se_count("mount_clone "), ==, 0);
    g_assert_cmpuint(se_mode(SE.stage), ==, 0755);
    g_assert_cmpint(g_rmdir(SE.stage), ==, 0);
    g_autofree gchar *other = se_path("other", NULL);
    g_assert_cmpint(g_mkdir(other, 0711), ==, 0);
    g_assert_cmpint(symlink(other, SE.stage), ==, 0);                         
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_cmpstr(err->message, ==, want);
    g_clear_error(&err);
    g_assert_cmpuint(se_count("mount_clone "), ==, 0);
    int dfd = open(other, O_PATH | O_DIRECTORY | O_CLOEXEC);                                       
    g_assert_cmpint(dfd, >=, 0);
    g_autofree gchar *kf = se_path("other", "k.0"), *kl = se_path("other", "l.0");
    g_assert_true(g_file_set_contents(kf, "", -1, NULL));
    g_assert_cmpint(symlink(SE.src, kl), ==, 0);
    g_assert_cmpint(SE.real->prepare_slot(dfd, "k.0", TRUE, &err), ==, -1);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH);
    g_clear_error(&err);
    g_assert_cmpint(SE.real->prepare_slot(dfd, "l.0", TRUE, &err), ==, -1);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH);
    g_clear_error(&err);
    close(dfd);
    se_end();
}

static gpointer
se_start_thread(gpointer name)
{
    GError *err = NULL;
    gboolean ok = pcv_lxc_volume_start_prepare(SE.lxc, name, &err);
    if (!ok)
        g_printerr("start_prepare(%s): %s\n", (const gchar *)name, err ? err->message : "?");
    g_clear_error(&err);
    return GINT_TO_POINTER(ok);
}

                                                                                
                                                                                           
                                                                             
static void
test_start_prepare_concurrent_stage_root(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind,ro 0 0\n");
    g_autofree gchar *c2dir = g_build_filename(SE.lxc, "c2", NULL), *c2cfg = g_build_filename(c2dir, "config", NULL);
    g_autofree gchar *c2body = g_strdup_printf("lxc.uts.name = c2\nlxc.mount.entry = %s/b mnt/b none bind 0 0\n", SE.src);
    g_autofree gchar *c2root = g_build_filename(c2dir, "rootfs", NULL);
    g_assert_cmpint(g_mkdir_with_parents(c2root, 0755), ==, 0);
    g_assert_true(g_file_set_contents(c2cfg, c2body, -1, NULL));
    SE.real_dir = TRUE;
    SE.slow_root = TRUE;
    mode_t old_mask = umask(077);                                                   
    GThread *a = g_thread_new("stage-c1", se_start_thread, (gpointer)"c1");
    GThread *b = g_thread_new("stage-c2", se_start_thread, (gpointer)"c2");
    gboolean ok_a = GPOINTER_TO_INT(g_thread_join(a)), ok_b = GPOINTER_TO_INT(g_thread_join(b));
    umask(old_mask);
    g_assert_true(ok_a && ok_b);
    GString *seq = g_string_new(NULL);
    for (guint i = 0; i < SE.log->len; i++) {
        const gchar *l = g_ptr_array_index(SE.log, i);
        if (g_str_has_prefix(l, "stage-root "))
            g_string_append_printf(seq, "%s%s", seq->len ? "," : "", l + strlen("stage-root "));
    }
    g_assert_cmpstr(seq->str, ==, "enter,exit,enter,exit");
    g_string_free(seq, TRUE);
    struct stat st;
    g_assert_cmpint(lstat(SE.stage, &st), ==, 0);
    g_assert_true(S_ISDIR(st.st_mode));
    g_assert_cmpuint(st.st_mode & 07777, ==, 0711);
    g_assert_cmpuint(st.st_uid, ==, geteuid());                                     
    g_assert_cmpuint(st.st_gid, ==, getegid());
    g_autofree gchar *r1 = se_path("rt", "c1/config"), *r2 = se_path("rt", "c2/config");
    g_assert_true(g_file_test(r1, G_FILE_TEST_IS_REGULAR) && g_file_test(r2, G_FILE_TEST_IS_REGULAR));
    se_end();
}

                                                                                                          
                                                                                          
                                                                                               
static void
test_start_prepare_creates_missing_target_dir(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/new1/new2/data none bind 0 0\n");
    mode_t old_mask = umask(077);
    GError *err = NULL;
    gboolean ok = pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err);
    umask(old_mask);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);
    const gchar *rels[] = { "mnt", "mnt/new1", "mnt/new1/new2", "mnt/new1/new2/data" };
    for (guint i = 0; i < G_N_ELEMENTS(rels); i++) {
        g_autofree gchar *d = g_build_filename(SE.lxc, "c1", "rootfs", rels[i], NULL);
        g_assert_true(g_file_test(d, G_FILE_TEST_IS_DIR));
        g_assert_cmpuint(se_mode(d), ==, 0755);                                       
    }
    g_autofree gchar *rtcfg = se_path("rt", "c1/config");
    g_assert_true(g_file_test(rtcfg, G_FILE_TEST_IS_REGULAR));
    se_end();
}

                                                                                          
static void
test_start_prepare_creates_missing_target_file(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/f etc/conf.d/app.conf none bind,ro 0 0\n");
    mode_t old_mask = umask(077);
    GError *err = NULL;
    gboolean ok = pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err);
    umask(old_mask);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_autofree gchar *dir = g_build_filename(SE.lxc, "c1", "rootfs", "etc", "conf.d", NULL);
    g_autofree gchar *leaf = g_build_filename(dir, "app.conf", NULL);
    g_assert_cmpuint(se_mode(dir), ==, 0755);
    struct stat st;
    g_assert_cmpint(lstat(leaf, &st), ==, 0);
    g_assert_true(S_ISREG(st.st_mode));
    g_assert_cmpint(st.st_size, ==, 0);
    g_assert_cmpuint(st.st_mode & 07777, ==, 0644);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);
    se_end();
}

                                                                                              
                                                                                             
static void
test_start_prepare_target_type_mismatch_fails(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\n");
    g_autofree gchar *mnt = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", NULL), *file = g_build_filename(mnt, "a", NULL);
    g_assert_cmpint(g_mkdir(mnt, 0755), ==, 0);
    g_assert_true(g_file_set_contents(file, "keep\n", -1, NULL));
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "cannot prepare the volume staging for /mnt/a: " PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
    g_clear_error(&err);
    g_assert_cmpuint(se_count("mount_clone "), ==, 0);
    g_assert_cmpuint(se_count("residue"), ==, 2);
    g_autofree gchar *rtdir = se_path("rt", "c1"), *stdir = se_path("volstage", "c1"), *text = NULL;
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(stdir, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_get_contents(file, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "keep\n");
    se_end();
}

                                                                                              
                                                                                
static void
test_start_prepare_target_symlink_refused(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/zz none bind 0 0\n");
    g_autofree gchar *mnt = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", NULL), *escape = g_build_filename(SE.src, "zz", NULL);
    g_assert_cmpint(symlink(SE.src, mnt), ==, 0);
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "cannot prepare the volume staging for /mnt/zz: container_path must not traverse symbolic links inside the container");
    g_clear_error(&err);
    g_assert_false(g_file_test(escape, G_FILE_TEST_EXISTS));                               
    g_assert_cmpuint(se_count("mount_clone "), ==, 0);
    g_assert_cmpuint(se_count("residue"), ==, 2);
    se_end();
}

                                                                                                    
                                                                                               
static void
test_start_prepare_missing_rootfs_skips_target(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\n");
    g_autofree gchar *rootfs = g_build_filename(SE.lxc, "c1", "rootfs", NULL);
    g_assert_cmpint(g_rmdir(rootfs), ==, 0);
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(se_count("target "), ==, 0);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);
    g_assert_false(g_file_test(rootfs, G_FILE_TEST_EXISTS));                           
    se_end();
}

                                                                                                                    
                                                                                                              
                                                                                                                       
static void
test_start_prepare_nested_missing_fails_clean(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\nlxc.mount.entry = @S@/b mnt/a/sub none bind 0 0\n");
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "cannot prepare the volume staging for /mnt/a/sub: container_path is inside the attached volume at /mnt/a, "
                                      "but sub does not exist in that volume's source; create it there first");
    g_clear_error(&err);
    g_assert_cmpuint(se_count("mount_clone "), ==, 1);                                      
    g_assert_cmpuint(se_count("residue"), ==, 2);
    g_assert_cmpstr(g_ptr_array_index(SE.log, SE.log->len - 1), ==, "residue");
    g_autofree gchar *rtdir = se_path("rt", "c1"), *stdir = se_path("volstage", "c1"), *asub = se_path("src", "a/sub");
    g_autofree gchar *rsub = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", "a", "sub", NULL);
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(stdir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(asub, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(rsub, G_FILE_TEST_EXISTS));
    se_end();
}

                                                                                                  
                                                                                                           
                                                                                                        
                                                                                       
static void
test_start_prepare_nested_present_ok_no_rootfs_ensure(void)
{
    se_begin();
    g_autofree gchar *asub = se_path("src", "a/sub"), *rsub = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", "a", "sub", NULL);
    g_assert_cmpint(g_mkdir(asub, 0700), ==, 0);
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\nlxc.mount.entry = @S@/b mnt//a/./sub none bind 0 0\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_autofree gchar *ka = se_k("mnt/a", 0), *kb = se_k("mnt/a/sub", 0);
    g_autofree gchar *want = g_strdup_printf(
        "residue\n"
        "source_open %1$s/a\ntarget walk /mnt\ntarget leaf /a\ntarget leaf /a\n"
        "prepare_dir volstage 711 0\nprepare_dir c1 700 0\nprepare_slot %2$s dir\nmount_clone %1$s/a 0 rt=0\n"
        "source_open %1$s/b\nsource_open %1$s/a\ntarget leaf /sub\nprepare_slot %3$s dir\nmount_clone %1$s/b 0 rt=0",
        SE.src, ka, kb);
    g_autofree gchar *got = se_log_text();
    g_assert_cmpstr(got, ==, want);
    g_assert_false(g_file_test(rsub, G_FILE_TEST_EXISTS));
    g_autofree gchar *rtcfg = se_path("rt", "c1/config");
    g_assert_true(g_file_test(rtcfg, G_FILE_TEST_IS_REGULAR));
    se_end();

    se_begin();                                                                                      
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/b mnt/a/sub none bind 0 0\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\n");
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(se_count("target walk /mnt/a"), ==, 1);
    g_assert_cmpuint(se_count("source_open "), ==, 2);
    g_autofree gchar *rsub2 = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", "a", "sub", NULL);
    g_assert_true(g_file_test(rsub2, G_FILE_TEST_IS_DIR));
    se_end();
}

                                                                                                                      
                                                                                                      
                                                                                                         
                                                                                                                   
                                                 
static void
test_start_prepare_nested_legacy_cover_uses_topmost(void)
{
    se_begin();
    g_autofree gchar *xsub = se_path("src", "x/a/sub"), *asub = se_path("src", "a/sub"), *xsubsub = se_path("src", "x/a/sub/sub");
    g_autofree gchar *rsub = g_build_filename(SE.lxc, "c1", "rootfs", "mnt", "a", "sub", NULL);
    g_assert_cmpint(g_mkdir_with_parents(xsub, 0700), ==, 0);
    se_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/a mnt/a none bind 0 0\nlxc.mount.entry = @S@/x mnt none bind 0 0\n"
              "lxc.mount.entry = @S@/b mnt/a/sub none bind 0 0\n");
    GError *err = NULL;
    const gboolean ok = pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err);
    g_assert_no_error(err);                                                                   
    g_assert_true(ok);
    g_autofree gchar *ka = se_k("mnt/a", 0), *kx = se_k("mnt", 0), *kb = se_k("mnt/a/sub", 0);
    g_autofree gchar *want = g_strdup_printf(
        "residue\n"
        "source_open %1$s/a\ntarget walk /mnt\ntarget leaf /a\ntarget leaf /a\n"
        "prepare_dir volstage 711 0\nprepare_dir c1 700 0\nprepare_slot %2$s dir\nmount_clone %1$s/a 0 rt=0\n"
        "source_open %1$s/x\ntarget walk /\ntarget leaf /mnt\nprepare_slot %3$s dir\nmount_clone %1$s/x 0 rt=0\n"
        "source_open %1$s/b\nsource_open %1$s/x\ntarget leaf /a/sub\nprepare_slot %4$s dir\nmount_clone %1$s/b 0 rt=0",
        SE.src, ka, kx, kb);
    g_autofree gchar *got = se_log_text();
    g_assert_cmpstr(got, ==, want);
    g_assert_cmpuint(se_count("residue"), ==, 1);                                                     
    g_assert_false(g_file_test(asub, G_FILE_TEST_EXISTS));                             
    g_assert_false(g_file_test(xsubsub, G_FILE_TEST_EXISTS));                               
    g_assert_false(g_file_test(rsub, G_FILE_TEST_EXISTS));
    g_autofree gchar *rtcfg = se_path("rt", "c1/config");
    g_assert_true(g_file_test(rtcfg, G_FILE_TEST_IS_REGULAR));
    se_end();
}

                                                                                          
                                           
static void
test_runtime_config_file_modes(void)
{
    se_begin();
    const gchar *body = "lxc.uts.name = c1\nlxc.net.0.type = veth\n";
    se_config(body);
    mode_t old_mask = umask(077);
    for (guint round = 0; round < 2; round++) {
        GError *err = NULL;
        g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
        g_assert_no_error(err);
        g_autofree gchar *dir = se_path("rt", "c1"), *cfg = se_path("rt", "c1/config"), *text = NULL;
        g_assert_cmpuint(se_mode(SE.rt), ==, 0700);
        g_assert_cmpuint(se_mode(dir), ==, 0700);
        g_assert_cmpuint(se_mode(cfg), ==, 0600);
        g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
        g_assert_cmpstr(text, ==, body);
        GDir *d = g_dir_open(dir, 0, NULL);
        g_assert_nonnull(d);
        g_assert_cmpstr(g_dir_read_name(d), ==, "config");
        g_assert_null(g_dir_read_name(d));                                     
        g_dir_close(d);
    }
    umask(old_mask);
    se_end();
}

                                                                  
static void
test_runtime_config_remove_ok(void)
{
    se_begin();
    se_config("lxc.uts.name = c1\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_start_prepare(SE.lxc, "c1", &err));
    g_assert_no_error(err);
    g_autofree gchar *dir = se_path("rt", "c1"), *stray = se_path("rt", "c1/.config.deadbeef");
    g_assert_true(g_file_set_contents(stray, "", -1, NULL));
    pcv_lxc_runtime_config_remove("c1");
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(SE.rt, G_FILE_TEST_IS_DIR));                                
    pcv_lxc_runtime_config_remove("c1");                                      
    g_assert_cmpint(g_rmdir(SE.rt), ==, 0);
    pcv_lxc_runtime_config_remove("c1");                                          
    pcv_lxc_runtime_config_remove("..");                                                          
    se_end();
}

                                                                                                          

static struct { guint opens; gboolean missing; } VS;

                                                                                              
static int
vs_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    VS.opens++;
    if (VS.missing) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "fake: host source missing");
        return -1;
    }
    if (canon_out)
        *canon_out = g_strdup(host_path);
    return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

                                                                      
static gboolean
vs_mnt_id(int fd, guint64 *id_out)
{
    (void)fd;
    *id_out = 77;
    return TRUE;
}

                                                                                                       
                                                                                                              
                                                                                                       
static void
test_volume_set_staging_ident(void)
{
    int probe0 = open("/dev/null", O_RDONLY | O_CLOEXEC);
    close(probe0);
    memset(&VS, 0, sizeof VS);
    PcvLxcMountOps ops = *pcv_lxc_volume_real_ops_for_test();
    ops.source_open = vs_source_open;
    ops.mnt_id = vs_mnt_id;
    pcv_lxc_volume_set_ops_for_test(&ops);
    pcv_lxc_volume_set_roots_for_test("/vstage", "/rt-unused");                                   
    const gchar *cfg = "lxc.mount.entry = proc proc proc nodev 0 0\n"
                       "lxc.mount.entry = /srv/a /mnt/a/ none bind,create=dir 0 0\n"
                       "lxc.mount.entry = rel mnt/r none bind 0 0\n"
                       "lxc.mount.entry = /srv/b mnt/a none rbind,ro 0 0\n";
    g_autofree gchar *slot0 = pcv_lxc_volstage_slot("/vstage", "c1", "mnt/a", 0);
    g_autofree gchar *slot1 = pcv_lxc_volstage_slot("/vstage", "c1", "mnt/a", 1);
    g_assert_true(g_str_has_suffix(slot0, ".0"));
    g_autofree gchar *host = g_strdup_printf("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                             "62 1 0:62 /sub %s rw,relatime master:3 - zfs pool/v rw\n"
                                             "77 1 0:63 /data /srv rw - zfs pool/s rw\n", slot0);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load(cfg, "c1", host);
    g_assert_nonnull(set);
    g_assert_cmpuint(set->entries->len, ==, 4);
    g_assert_cmpuint(set->lines->len, ==, 2);
    const PcvLxcVolumeLine *l0 = set->lines->pdata[0], *l1 = set->lines->pdata[1];
    g_assert_true(l0->entry == set->entries->pdata[1]);
    g_assert_cmpstr(l0->dest, ==, "mnt/a");
    g_assert_cmpuint(l0->ordinal, ==, 0);
    g_assert_cmpstr(l0->slot, ==, slot0);
    g_assert_true(l0->ident_ok);
    g_assert_cmpuint(l0->ident.major, ==, 0);
    g_assert_cmpuint(l0->ident.minor, ==, 62);
    g_assert_cmpstr(l0->ident.root, ==, "/sub");
    g_assert_true(l1->entry == set->entries->pdata[3]);
    g_assert_cmpstr(l1->dest, ==, "mnt/a");
    g_assert_cmpuint(l1->ordinal, ==, 1);
    g_assert_cmpstr(l1->slot, ==, slot1);
    g_assert_true(l1->ident_ok);
    g_assert_cmpuint(l1->ident.minor, ==, 63);
    g_assert_cmpstr(l1->ident.root, ==, "/data/b");
    g_assert_cmpuint(VS.opens, ==, 1);                                 
    pcv_lxc_volume_set_free(set);

    VS.opens = 0;
    VS.missing = TRUE;                                        
    set = pcv_lxc_volume_set_load(cfg, "c1", "1 0 8:1 / / rw - ext4 /dev/sda1 rw\n");
    g_assert_nonnull(set);
    g_assert_cmpuint(set->lines->len, ==, 2);
    g_assert_false(((const PcvLxcVolumeLine *)set->lines->pdata[0])->ident_ok);
    g_assert_false(((const PcvLxcVolumeLine *)set->lines->pdata[1])->ident_ok);
    g_assert_cmpuint(VS.opens, ==, 2);
    pcv_lxc_volume_set_free(set);

    g_assert_null(pcv_lxc_volume_set_load(cfg, "", host));
    g_assert_null(pcv_lxc_volume_set_load(cfg, "a/b", host));
    g_assert_null(pcv_lxc_volume_set_load(cfg, "..", host));
    g_assert_null(pcv_lxc_volume_set_load(cfg, NULL, host));
    pcv_lxc_volume_set_free(NULL);
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(probe, ==, probe0);                   
    close(probe);
}

                                                                                                                   
                                                                                                               
                                        
static void
test_volume_set_dot_spelling_shares_slots(void)
{
    int probe0 = open("/dev/null", O_RDONLY | O_CLOEXEC);
    close(probe0);
    memset(&VS, 0, sizeof VS);
    PcvLxcMountOps ops = *pcv_lxc_volume_real_ops_for_test();
    ops.source_open = vs_source_open;
    ops.mnt_id = vs_mnt_id;
    pcv_lxc_volume_set_ops_for_test(&ops);
    pcv_lxc_volume_set_roots_for_test("/vstage", "/rt-unused");                         
    const gchar *cfg = "lxc.mount.entry = /srv/a mnt/./a none bind 0 0\nlxc.mount.entry = /srv/b /mnt//a/ none bind 0 0\n";
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load(cfg, "c1", "1 0 8:1 / / rw - ext4 /dev/sda1 rw\n77 1 0:63 /data /srv rw - zfs pool/s rw\n");
    g_assert_nonnull(set);
    g_assert_cmpuint(set->lines->len, ==, 2);
    GError *err = NULL;
    GPtrArray *st = pcv_lxc_volstage_lines(cfg, "/vstage", "c1", &err);
    g_assert_no_error(err);
    g_assert_nonnull(st);
    g_assert_cmpuint(st->len, ==, 2);
    for (guint i = 0; i < 2; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        g_autofree gchar *want = pcv_lxc_volstage_slot("/vstage", "c1", "mnt/a", i);
        g_assert_cmpstr(l->dest, ==, "mnt/a");
        g_assert_cmpuint(l->ordinal, ==, i);
        g_assert_cmpstr(l->slot, ==, want);
        g_assert_cmpstr(((const PcvLxcVolstageLine *)st->pdata[i])->slot, ==, want);
    }
    g_ptr_array_unref(st);
    pcv_lxc_volume_set_free(set);
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(probe, ==, probe0);
    close(probe);
}

                                                                                                                                   
                                                                                               
                                                                                                   
                                                                                   
                                                                                                             
                                                                                                    
                                                                                                                        
                                                                                                                   
                                                                                  
                                                                                  

typedef struct { guint64 id; gchar *dev, *root, *mp; gboolean alive; } AMount;

static struct {
    gchar          *base, *lxc, *rootfs, *cfg, *srcdir;                                                                    
    GArray         *mounts;                                           
    GHashTable     *fd_mount;                                                             
    GHashTable     *missing;                                                                 
    GPtrArray      *log;                                                                                                                               
    guint64         next_id, umount_fail_id, src_id;
    gboolean        leaf_fail_after_clone, clone_fail;                                                   
    guint           clones;
    int             src_fd;
    const gchar    *src_dev, *src_root;                                      
    const PcvLxcMountOps *real;
    PcvLxcMountOps  ops;
    int             fd_probe;
} AT;

static void
at_log(gchar *line)
{
    g_ptr_array_add(AT.log, line);
}

                                                                                              
static guint64
at_resolve(const gchar *path)
{
    guint cur = 0;
    gsize len = strlen(path);
    for (gsize i = 1; i <= len; i++) {
        if (i < len && path[i] != '/')
            continue;
        g_autofree gchar *pre = g_strndup(path, i);
        for (guint j = cur + 1; j < AT.mounts->len; j++) {
            AMount *m = &g_array_index(AT.mounts, AMount, j);
            if (m->alive && strcmp(m->mp, pre) == 0)
                cur = j;
        }
    }
    return g_array_index(AT.mounts, AMount, cur).id;
}

                                                            
static gchar *
at_fd_path(int fd)
{
    g_autofree gchar *link = g_strdup_printf("/proc/self/fd/%d", fd);
    return g_file_read_link(link, NULL);
}

                                                           
static int
at_track(int fd)
{
    if (fd < 0)
        return fd;
    g_autofree gchar *p = at_fd_path(fd);
    g_assert_nonnull(p);
    g_hash_table_insert(AT.fd_mount, GINT_TO_POINTER(fd), g_memdup2(&(guint64){ at_resolve(p) }, sizeof(guint64)));
    return fd;
}

static guint64
at_fd_mount(int fd)
{
    guint64 *id = g_hash_table_lookup(AT.fd_mount, GINT_TO_POINTER(fd));
    g_assert_nonnull(id);
    return *id;
}

static gchar *
at_read_mountinfo(GError **error)
{
    (void)error;
    at_log(g_strdup("read"));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < AT.mounts->len; i++) {
        AMount *m = &g_array_index(AT.mounts, AMount, i);
        if (m->alive)
            g_string_append_printf(s, "%" G_GUINT64_FORMAT " 1 %s %s %s rw,relatime master:1 - zfs pool/x rw\n", m->id, m->dev, m->root,
                                   m->mp);
    }
    return g_string_free(s, FALSE);
}

static int
at_open_dir(const gchar *path, GError **error)
{
    return at_track(AT.real->open_dir(path, error));
}

static int
at_walk(int root_fd, const gchar *rel, gboolean create, GError **error)
{
    return at_track(AT.real->walk(root_fd, rel, create, error));
}

static int
at_open_leaf(int root_fd, const gchar *rel, GError **error)
{
    if (AT.leaf_fail_after_clone && AT.clones > 0) {                                     
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES, "open mount point: Too many open files");
        return -1;
    }
    return at_track(AT.real->open_leaf(root_fd, rel, error));
}

static gboolean
at_mnt_id(int fd, guint64 *id_out)
{
    *id_out = fd == AT.src_fd ? AT.src_id : at_fd_mount(fd);
    return TRUE;
}

static int
at_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    at_log(g_strdup_printf("source_open %s", host_path));
    if (g_hash_table_contains(AT.missing, host_path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "fake: host source missing");
        return -1;
    }
    if (g_str_has_prefix(host_path, AT.srcdir))                                                                          
        return at_track(AT.real->source_open(host_path, canon_out, error));
    g_assert_true(g_str_has_prefix(host_path, "/srv/") || g_str_has_prefix(host_path, "/zz/"));
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    g_hash_table_insert(AT.fd_mount, GINT_TO_POINTER(fd), g_memdup2(&(guint64){ g_str_has_prefix(host_path, "/srv/") ? 77 : 78 },
                                                                    sizeof(guint64)));
    if (canon_out)
        *canon_out = g_strdup(host_path);
    return fd;
}

static gboolean
at_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error)
{
    g_assert_cmpint(src_fd, ==, AT.src_fd);                                                    
    g_autofree gchar *tp = at_fd_path(target_fd);
    g_assert_nonnull(tp);
    g_assert_true(g_str_has_prefix(tp, AT.rootfs));
    at_log(g_strdup_printf("mount_clone %s %u", tp + strlen(AT.rootfs), flags));
    struct stat s, t;
    g_assert_cmpint(fstat(src_fd, &s), ==, 0);
    g_assert_cmpint(fstat(target_fd, &t), ==, 0);
    if (S_ISDIR(s.st_mode) != S_ISDIR(t.st_mode)) {                                               
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "move_mount: Not a directory");
        return FALSE;
    }
    if (AT.clone_fail) {                                                                               
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "statx(STATX_MNT_ID): Operation not supported");
        return FALSE;
    }
    AMount m = { AT.next_id++, g_strdup(AT.src_dev), g_strdup(AT.src_root), g_steal_pointer(&tp), TRUE };
    g_array_append_val(AT.mounts, m);
    AT.clones++;
    if (mnt_id_out)
        *mnt_id_out = m.id;                                                                                              
    return TRUE;
}

static gboolean
at_make_private(int fd, GError **error)
{
    (void)error;
    at_log(g_strdup_printf("private %" G_GUINT64_FORMAT, at_fd_mount(fd)));
    return TRUE;
}

static gboolean
at_umount_fd(int fd, GError **error)
{
    const guint64 id = at_fd_mount(fd);
    at_log(g_strdup_printf("umount %" G_GUINT64_FORMAT, id));
    if (AT.umount_fail_id && id == AT.umount_fail_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "umount: Device or resource busy");
        return FALSE;
    }
    for (guint i = 0; i < AT.mounts->len; i++)
        if (g_array_index(AT.mounts, AMount, i).id == id)
            g_array_index(AT.mounts, AMount, i).alive = FALSE;
    return TRUE;
}

static void
at_mount_add(guint64 id, const gchar *dev, const gchar *root, const gchar *mp)
{
    AMount m = { id, g_strdup(dev), g_strdup(root), g_strdup(mp), TRUE };
    g_array_append_val(AT.mounts, m);
}

static void
at_mount_clear(gpointer p)
{
    AMount *m = p;
    g_free(m->dev);
    g_free(m->root);
    g_free(m->mp);
}

                                                                                     
static void
at_rootfs_mount(guint64 id, const gchar *dev, const gchar *root, const gchar *rel)
{
    g_autofree gchar *mp = g_strconcat(AT.rootfs, rel, NULL);
    g_assert_cmpint(g_mkdir_with_parents(mp, 0755), ==, 0);
    at_mount_add(id, dev, root, mp);
}

static gboolean
at_alive(guint64 id)
{
    for (guint i = 0; i < AT.mounts->len; i++)
        if (g_array_index(AT.mounts, AMount, i).id == id)
            return g_array_index(AT.mounts, AMount, i).alive;
    return FALSE;
}

                                                                                                      
                                                                                                             
static void
at_begin(const gchar *cfg)
{
    memset(&AT, 0, sizeof AT);
    AT.fd_probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(AT.fd_probe, >=, 0);
    close(AT.fd_probe);
    g_autofree gchar *made = g_dir_make_tmp("pcv-vattach-XXXXXX", NULL);
    g_assert_nonnull(made);
    char real[PATH_MAX];
    g_assert_nonnull(realpath(made, real));                                                   
    AT.base = g_strdup(real);
    AT.lxc = g_build_filename(AT.base, "lxc", NULL);
    AT.rootfs = g_build_filename(AT.lxc, "c1", "rootfs", NULL);
    AT.cfg = g_build_filename(AT.lxc, "c1", "config", NULL);
    AT.srcdir = g_build_filename(AT.base, "src", NULL);
    g_autofree gchar *a = g_build_filename(AT.srcdir, "a", NULL), *f = g_build_filename(AT.srcdir, "f", NULL);
    g_assert_cmpint(g_mkdir_with_parents(AT.rootfs, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(a, 0755), ==, 0);
    g_assert_true(g_file_set_contents(f, "file volume\n", -1, NULL));
    g_assert_true(g_file_set_contents(AT.cfg, cfg, -1, NULL));
    AT.mounts = g_array_new(FALSE, TRUE, sizeof(AMount));
    g_array_set_clear_func(AT.mounts, at_mount_clear);
    AT.fd_mount = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    AT.missing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    AT.log = g_ptr_array_new_with_free_func(g_free);
    at_mount_add(1, "8:1", "/", "/");
    g_autofree gchar *ds = g_build_filename(AT.lxc, "c1", NULL);
    at_mount_add(2, "0:40", "/", ds);
    at_mount_add(77, "0:63", "/data", "/srv");
    at_mount_add(78, "0:64", "/", "/zz");
    AT.next_id = 500;
    AT.src_id = 77;
    AT.src_dev = "0:63";
    AT.src_root = "/data/a";
    AT.src_fd = -1;
    AT.real = pcv_lxc_volume_real_ops_for_test();
    AT.ops = *AT.real;
    AT.ops.read_mountinfo = at_read_mountinfo;
    AT.ops.open_dir = at_open_dir;
    AT.ops.walk = at_walk;
    AT.ops.open_leaf = at_open_leaf;
    AT.ops.mnt_id = at_mnt_id;
    AT.ops.source_open = at_source_open;
    AT.ops.mount_clone = at_mount_clone;
    AT.ops.make_private = at_make_private;
    AT.ops.umount_fd = at_umount_fd;
    g_autofree gchar *stage = g_build_filename(AT.base, "volstage", NULL);
    pcv_lxc_volume_set_ops_for_test(&AT.ops);
    pcv_lxc_volume_set_roots_for_test(stage, NULL);
}

                                                                            
static int
at_src(const gchar *name)
{
    g_autofree gchar *p = g_build_filename(AT.srcdir, name, NULL);
    AT.src_fd = open(p, O_PATH | O_CLOEXEC);
    g_assert_cmpint(AT.src_fd, >=, 0);
    return AT.src_fd;
}

static void
at_end(void)
{
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    if (AT.src_fd >= 0)
        close(AT.src_fd);
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(probe, ==, AT.fd_probe);                                                        
    close(probe);
    g_assert_cmpint(nftw(AT.base, se_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_array_unref(AT.mounts);
    g_hash_table_unref(AT.fd_mount);
    g_hash_table_unref(AT.missing);
    g_ptr_array_unref(AT.log);
    g_free(AT.base); g_free(AT.lxc); g_free(AT.rootfs); g_free(AT.cfg); g_free(AT.srcdir);
}

static guint
at_count(const gchar *head)
{
    guint n = 0;
    for (guint i = 0; i < AT.log->len; i++)
        n += g_str_has_prefix(g_ptr_array_index(AT.log, i), head);
    return n;
}

                                                                         
static gchar *
at_seq(const gchar *head1, const gchar *head2)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < AT.log->len; i++) {
        const gchar *e = g_ptr_array_index(AT.log, i);
        if (g_str_has_prefix(e, head1) || (head2 && g_str_has_prefix(e, head2)))
            g_string_append_printf(s, "%s%s", s->len ? "," : "", e);
    }
    return g_string_free(s, FALSE);
}

static gchar *
at_cfg_text(void)
{
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(AT.cfg, &text, NULL, NULL));
    return text;
}

                                         
static guint
at_lines(const gchar *head)
{
    g_autofree gchar *text = at_cfg_text();
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    guint n = 0;
    for (guint i = 0; lines[i]; i++)
        n += g_str_has_prefix(lines[i], head);
    return n;
}

                                                                                
static void
at_hardlink_config(void)
{
    g_autofree gchar *ln = g_build_filename(AT.base, "config.link", NULL);
    g_assert_cmpint(link(AT.cfg, ln), ==, 0);
}

#define AT_HOOK_LINE "lxc.hook.pre-start = " PCV_LXC_VOLUME_GUARD_HOOK_VALUE

                                                                                                          
                                                                                                                      
static void
test_attach_dup_same_source(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n");
    g_autofree gchar *before = at_cfg_text();
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/b", FALSE, TRUE, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_assert_cmpstr(err->message, ==, "host_path is already attached to this container at /mnt/a");
    g_clear_error(&err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_autofree gchar *after = at_cfg_text();
    g_assert_cmpstr(after, ==, before);
    at_end();
}

                                                                                                       
static void
test_attach_dup_subdir_not_dup(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a/b", "/mnt/b", TRUE, FALSE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_lines("lxc.mount.entry = "), ==, 2);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a/b mnt/b none bind,ro 0 0"), ==, 1);
    at_end();
}

                                                                                                       
                                                   
static void
test_attach_dup_ignores_unidentifiable(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/gone mnt/g none bind 0 0\n");
    g_hash_table_add(AT.missing, g_strdup("/srv/gone"));
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, FALSE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_count("source_open /srv/gone"), ==, 1);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a mnt/a none bind 0 0"), ==, 1);
    at_end();
}

                                                                                                  
                                                                                                                      
                                                                 
static void
test_attach_persist_failure_reverts_mount(void)
{
    at_begin("lxc.uts.name = c1\n");
    at_hardlink_config();
    g_autofree gchar *before = at_cfg_text();
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, TRUE, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_cmpstr(err->message, ==, "Failed to persist container volume config");
    g_clear_error(&err);
    g_autofree gchar *seq = at_seq("mount_clone ", "umount ");
    g_assert_cmpstr(seq, ==, "mount_clone /mnt/a 0,umount 500");
    g_autofree gchar *pairs = at_seq("private ", "umount ");
    g_assert_cmpstr(pairs, ==, "private 500,umount 500");                               
    g_assert_false(at_alive(500));
    g_autofree gchar *target = g_build_filename(AT.rootfs, "mnt", "a", NULL);
    g_assert_true(g_file_test(target, G_FILE_TEST_IS_DIR));
    g_autofree gchar *after = at_cfg_text();
    g_assert_cmpstr(after, ==, before);
    at_end();
}

                                                                                                 
                                                                                                                
                                                                                             
                                                                                              
static void
test_attach_persist_failure_keeps_existing_mounts(void)
{
    at_begin("lxc.uts.name = c1\n"
             "lxc.mount.entry = /srv/q mnt/q none bind 0 0\n"
             "lxc.mount.entry = /srv/gone mnt/old none bind 0 0\n"
             "lxc.mount.entry = /zz/z mnt/z none bind 0 0\n");
    g_hash_table_add(AT.missing, g_strdup("/srv/gone"));
    at_rootfs_mount(299, "0:63", "/data/q", "/mnt/q");
    at_rootfs_mount(300, "0:63", "/data/a", "/mnt/old");
    at_rootfs_mount(301, "0:64", "/z", "/mnt/z");
    at_hardlink_config();
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, TRUE, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_cmpstr(err->message, ==, "Failed to persist container volume config");
    g_clear_error(&err);
    g_autofree gchar *um = at_seq("umount ", NULL);
    g_assert_cmpstr(um, ==, "umount 500");
    g_autofree gchar *pv = at_seq("private ", NULL);
    g_assert_cmpstr(pv, ==, "private 500");
    g_assert_true(at_alive(299));
    g_assert_true(at_alive(300));
    g_assert_true(at_alive(301));
    g_assert_false(at_alive(500));
    at_end();
}

                                                                                                                     
                                                              
static void
test_attach_persist_failure_revert_failure_reported(void)
{
    at_begin("lxc.uts.name = c1\n");
    at_hardlink_config();
    AT.umount_fail_id = 500;
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, TRUE, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_cmpstr(err->message, ==,
                    "Failed to persist container volume config; the running mount could not be removed: umount: Device or resource busy");
    g_clear_error(&err);
    g_assert_cmpuint(at_count("umount 500"), >=, 1);
    g_assert_true(at_alive(500));
    at_end();
}

                                                                                                      
static void
test_attach_stopped_no_mount(void)
{
    at_begin("lxc.uts.name = c1\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", TRUE, FALSE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_assert_cmpuint(at_lines("lxc.mount.entry = "), ==, 1);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0"), ==, 1);
    g_assert_cmpuint(at_lines(AT_HOOK_LINE), ==, 1);
    at_end();
}

                                                                                                           
#define AT_OCCUPIED_MSG(p) \
    "container_path already has a volume attached while the container is running: " p "; detach it first, or attach while the container is stopped"

                                                                                                                      
                                                                     
static void
at_occupied_refused(const gchar *canon, const gchar *cpath, const gchar *msg)
{
    g_autofree gchar *before = at_cfg_text();
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), canon, cpath, FALSE, TRUE, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_assert_cmpstr(err->message, ==, msg);
    g_clear_error(&err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_assert_cmpuint(at_count("private "), ==, 0);
    g_assert_cmpuint(at_count("umount "), ==, 0);
    g_autofree gchar *after = at_cfg_text();
    g_assert_cmpstr(after, ==, before);
}

                                                                                                                
                                                                                                 
                                                                                       
static void
test_attach_running_occupied_by_line_refused(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt/a none bind 0 0\n");
    at_occupied_refused("/srv/b", "/mnt/a", AT_OCCUPIED_MSG("/mnt/a"));
    g_autofree gchar *mnt = g_build_filename(AT.rootfs, "mnt", NULL);
    g_assert_false(g_file_test(mnt, G_FILE_TEST_EXISTS));
    at_end();
}

                                                                                                      
                                      
static void
test_attach_running_occupied_by_mount_refused(void)
{
    at_begin("lxc.uts.name = c1\n");
    at_rootfs_mount(299, "0:65", "/legacy", "/mnt/a");
    at_occupied_refused("/srv/a", "/mnt/a", AT_OCCUPIED_MSG("/mnt/a"));
    g_assert_true(at_alive(299));
    at_end();
}

                                                                                                      
                                                                                                                           
static void
test_attach_running_dest_spelling_refused(void)
{
    static const struct { const gchar *line, *cpath; } cases[] = {
        { "mnt/a", "/mnt//a" }, { "mnt/a", "/mnt/a/" }, { "mnt/a", "/mnt/./a" }, { "/mnt//a/", "/mnt/a" }, { NULL, "/mnt//a/" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *cfg = cases[i].line ? g_strdup_printf("lxc.uts.name = c1\nlxc.mount.entry = /srv/a %s none bind 0 0\n", cases[i].line)
                                              : g_strdup("lxc.uts.name = c1\n");
        at_begin(cfg);
        if (!cases[i].line)
            at_rootfs_mount(299, "0:65", "/legacy", "/mnt/a");
        at_occupied_refused("/srv/b", cases[i].cpath, AT_OCCUPIED_MSG("/mnt/a"));
        at_end();
    }
}

                                                                                                          
                                                                                                
static void
test_attach_stopped_same_path_allowed(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt/a none bind 0 0\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/b", "/mnt/a", FALSE, FALSE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_autofree gchar *text = at_cfg_text();
    const gchar *first = strstr(text, "lxc.mount.entry = /srv/a mnt/a none bind 0 0\n");
    const gchar *second = strstr(text, "lxc.mount.entry = /srv/b mnt/a none bind 0 0\n");
    g_assert_nonnull(first);
    g_assert_nonnull(second);
    g_assert_true(first < second);
    g_assert_cmpuint(at_lines("lxc.mount.entry = "), ==, 2);
    at_end();
}

                                                                                                               
                              
#define AT_INSIDE_MSG(p, o) \
    "container_path is inside another attached volume while the container is running: " p " (volume at " o "); attach while the container is " \
    "stopped (the path must already exist inside that volume's source)"

                                                                                                           
                                                                                                  
                                                                                                           
                 
static void
test_attach_running_inside_line_refused(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt/a none bind 0 0\n");
    at_occupied_refused("/srv/b", "/mnt/a/sub", AT_INSIDE_MSG("/mnt/a/sub", "/mnt/a"));
    g_autofree gchar *mnt = g_build_filename(AT.rootfs, "mnt", NULL);
    g_assert_false(g_file_test(mnt, G_FILE_TEST_EXISTS));
    at_end();
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/a mnt none bind 0 0\nlxc.mount.entry = /zz/x mnt/./a/ none bind 0 0\n");
    at_occupied_refused("/srv/b", "/mnt//a/sub/deeper", AT_INSIDE_MSG("/mnt/a/sub/deeper", "/mnt/a"));
    at_end();
}

                                                                                                       
                                                                          
static void
test_attach_running_inside_mount_refused(void)
{
    at_begin("lxc.uts.name = c1\n");
    at_rootfs_mount(299, "0:65", "/legacy", "/mnt/a");
    at_occupied_refused("/srv/a", "/mnt/a/sub", AT_INSIDE_MSG("/mnt/a/sub", "/mnt/a"));
    g_assert_true(at_alive(299));
    g_autofree gchar *sub = g_build_filename(AT.rootfs, "mnt", "a", "sub", NULL);
    g_assert_false(g_file_test(sub, G_FILE_TEST_EXISTS));
    at_end();
}

                                                                                                                     
                                                                                              
                                                                     
static void
test_attach_running_inside_legacy_cover_names_topmost(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /zz/x mnt/a none bind 0 0\nlxc.mount.entry = /srv/a mnt none bind 0 0\n");
    at_occupied_refused("/srv/b", "/mnt/a/sub", AT_INSIDE_MSG("/mnt/a/sub", "/mnt"));
    g_autofree gchar *mnt = g_build_filename(AT.rootfs, "mnt", NULL);
    g_assert_false(g_file_test(mnt, G_FILE_TEST_EXISTS));
    at_end();
}

                                                                                                                   
                        
static void
test_attach_running_sibling_prefix_not_inside(void)
{
    at_begin("lxc.uts.name = c1\nlxc.mount.entry = /zz/x mnt/a none bind 0 0\n");
    at_rootfs_mount(299, "0:64", "/x", "/mnt/a");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/ab", FALSE, TRUE, &err));
    g_assert_no_error(err);
    g_autofree gchar *seq = at_seq("mount_clone ", NULL);
    g_assert_cmpstr(seq, ==, "mount_clone /mnt/ab 0");
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a mnt/ab none bind 0 0"), ==, 1);
    at_end();
}

                                                                                             
                                                                                                 
                                                                                                          
                                                                                           
                                                                            
#define AT_COVER_HEAD "container_path would cover another attached volume: "
#define AT_NEST_HEAD "container_path is inside the attached volume at "
#define AT_TYPE_TEXT "container_path must be the same type as host_path (both directories or both regular files)"
#define AT_LINK_TEXT "container_path must not traverse symbolic links inside the container"

                                           
static gchar *
at_nest_missing(const gchar *outer, const gchar *rel)
{
    return g_strconcat(AT_NEST_HEAD, outer, ", but ", rel, " does not exist in that volume's source; create it there first", NULL);
}

                                                                         
static gchar *
at_nest_ctx(const gchar *outer, const gchar *rel, const gchar *what)
{
    return g_strconcat(AT_NEST_HEAD, outer, " (", rel, " in that volume's source): ", what, NULL);
}

                                                                     
static void
at_config(const gchar *tmpl)
{
    g_auto(GStrv) parts = g_strsplit(tmpl, "@S@", -1);
    g_autofree gchar *text = g_strjoinv(AT.srcdir, parts);
    g_assert_true(g_file_set_contents(AT.cfg, text, -1, NULL));
}

                                                                                                     
static void
at_mk(const gchar *rel, gchar kind, const gchar *target)
{
    g_autofree gchar *p = g_build_filename(AT.srcdir, rel, NULL), *parent = g_path_get_dirname(p);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0755), ==, 0);
    if (kind == 'd')
        g_assert_cmpint(g_mkdir_with_parents(p, 0755), ==, 0);
    else if (kind == 'f')
        g_assert_true(g_file_set_contents(p, "keep\n", -1, NULL));
    else if (kind == 'p')
        g_assert_cmpint(mkfifo(p, 0600), ==, 0);
    else
        g_assert_cmpint(symlink(target, p), ==, 0);
}

static gboolean
at_exists(const gchar *rel)
{
    g_autofree gchar *p = g_build_filename(AT.srcdir, rel, NULL);
    struct stat st;
    return lstat(p, &st) == 0;
}

                                                                                                                             
                                                                                                                 
static void
at_bad_path_refused(const gchar *src, const gchar *canon, const gchar *cpath, gboolean running, const gchar *msg)
{
    g_autofree gchar *before = at_cfg_text(), *mnt = g_build_filename(AT.rootfs, "mnt", NULL);
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src(src), canon, cpath, FALSE, running, &err));
    g_assert_error(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_assert_cmpstr(err->message, ==, msg);
    g_clear_error(&err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_assert_cmpuint(at_count("private "), ==, 0);
    g_assert_cmpuint(at_count("umount "), ==, 0);
    g_autofree gchar *after = at_cfg_text();
    g_assert_cmpstr(after, ==, before);
    g_assert_false(g_file_test(mnt, G_FILE_TEST_EXISTS));
}

                                                                                                     
                                                                
static const struct { const gchar *lines, *cpath, *inner; } AT_COVER_CASES[] = {
    { "lxc.mount.entry = /srv/a mnt/a/sub none bind 0 0\n", "/mnt/a", "/mnt/a/sub" },
    { "lxc.mount.entry = /srv/a mnt//a/./sub/ none bind 0 0\n", "/mnt/./a/", "/mnt/a/sub" },
    { "lxc.mount.entry = /srv/a mnt/a/x/y none bind 0 0\n", "/mnt", "/mnt/a/x/y" },
    { "lxc.mount.entry = /zz/t mnt none bind 0 0\nlxc.mount.entry = /srv/a mnt/a/sub none bind 0 0\n", "/mnt/a", "/mnt/a/sub" },
};

                                                                               
static void
at_cover_case(guint i, gboolean running)
{
    g_autofree gchar *cfg = g_strconcat("lxc.uts.name = c1\n", AT_COVER_CASES[i].lines, NULL);
    g_autofree gchar *msg = g_strconcat(AT_COVER_HEAD, AT_COVER_CASES[i].inner, "; detach it first", NULL);
    at_begin(cfg);
    at_bad_path_refused("a", "/srv/b", AT_COVER_CASES[i].cpath, running, msg);
    at_end();
}

                                                                                                                     
                                                                                           
static void
test_attach_cover_inner_refused_running(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(AT_COVER_CASES); i++)
        at_cover_case(i, TRUE);
}

                                                                                                     
                                             
static void
test_attach_cover_inner_refused_stopped(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(AT_COVER_CASES); i++)
        at_cover_case(i, FALSE);
}

                                                                                                   
                     
static void
test_attach_cover_sibling_prefix_allowed(void)
{
    for (guint running = 0; running < 2; running++) {
        at_begin("lxc.uts.name = c1\nlxc.mount.entry = /zz/x mnt/ab none bind 0 0\n");
        GError *err = NULL;
        g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, running, &err));
        g_assert_no_error(err);
        g_autofree gchar *seq = at_seq("mount_clone ", NULL);
        g_assert_cmpstr(seq, ==, running ? "mount_clone /mnt/a 0" : "");
        g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a mnt/a none bind 0 0"), ==, 1);
        g_assert_cmpuint(at_lines("lxc.mount.entry = "), ==, 2);
        at_end();
    }
}

                                                                                                                   
                                                                                                            
                                                                     
static void
test_attach_stopped_inside_missing_refused(void)
{
    static const struct { const gchar *outer, *cpath, *rel; gboolean mk_file_x; } cases[] = {
        { "outer", "/mnt/a/sub", "sub", FALSE },
        { "outer", "/mnt/a//x/./y", "x/y", FALSE },
        { "outer", "/mnt/a/x/y", "x/y", TRUE },
        { "gone", "/mnt/a/sub", "sub", FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        at_begin("lxc.uts.name = c1\n");
        at_mk("outer", 'd', NULL);
        if (cases[i].mk_file_x)
            at_mk("outer/x", 'f', NULL);
        g_autofree gchar *cfg = g_strdup_printf("lxc.uts.name = c1\nlxc.mount.entry = @S@/%s mnt/a none bind 0 0\n", cases[i].outer);
        at_config(cfg);
        g_autofree gchar *msg = at_nest_missing("/mnt/a", cases[i].rel);
        at_bad_path_refused("a", "/srv/b", cases[i].cpath, FALSE, msg);
        g_assert_false(at_exists("outer/sub"));
        g_assert_false(at_exists("outer/x/y"));
        g_assert_false(at_exists("gone"));
        at_end();
    }
}

                                                                                                    
                                                                                               
                                                                                                          
                     
static void
test_attach_stopped_inside_present_allowed(void)
{
    static const struct { const gchar *lines, *src, *cpath, *entry; } cases[] = {
        { "lxc.mount.entry = @S@/outer mnt/a none bind 0 0\n", "a", "/mnt/a/sub", "/srv/b mnt/a/sub" },
        { "lxc.mount.entry = @S@/outer mnt/a none bind 0 0\n", "f", "/mnt/a/conf", "/srv/b mnt/a/conf" },
        { "lxc.mount.entry = @S@/top mnt none bind 0 0\nlxc.mount.entry = @S@/outer mnt/a none bind 0 0\n", "a", "/mnt/a/sub",
          "/srv/b mnt/a/sub" },
        { "lxc.mount.entry = @S@/top mnt/a none bind 0 0\nlxc.mount.entry = @S@/outer mnt/a none bind 0 0\n", "a", "/mnt/a/sub",
          "/srv/b mnt/a/sub" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        at_begin("lxc.uts.name = c1\n");
        at_mk("outer/sub", 'd', NULL);
        at_mk("outer/conf", 'f', NULL);
        at_mk("top/a", 'd', NULL);                                                        
        g_autofree gchar *cfg = g_strconcat("lxc.uts.name = c1\n", cases[i].lines, NULL);
        at_config(cfg);
        GError *err = NULL;
        g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src(cases[i].src), "/srv/b", cases[i].cpath, FALSE, FALSE, &err));
        g_assert_no_error(err);
        g_assert_cmpuint(at_count("mount_clone "), ==, 0);
        g_autofree gchar *line = g_strdup_printf("lxc.mount.entry = %s none bind 0 0", cases[i].entry);
        g_assert_cmpuint(at_lines(line), ==, 1);
        g_assert_false(at_exists("top/a/sub"));                              
        g_assert_false(at_exists("top/sub"));
        g_assert_true(at_exists("outer/sub") && at_exists("outer/conf"));
        at_end();
    }
}

                                                                                                                      
                                                                                                     
                                                                                                 
                                                                   
static void
test_attach_stopped_inside_legacy_cover_uses_topmost(void)
{
    at_begin("lxc.uts.name = c1\n");
    at_mk("inner", 'd', NULL);
    at_mk("top/a/sub", 'd', NULL);
    at_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/inner mnt/a none bind 0 0\nlxc.mount.entry = @S@/top mnt none bind 0 0\n");
    GError *err = NULL;
    const gboolean ok = pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/b", "/mnt/a/sub", FALSE, FALSE, &err);
    g_assert_no_error(err);                                                         
    g_assert_true(ok);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/b mnt/a/sub none bind 0 0"), ==, 1);
    g_assert_cmpuint(at_lines("lxc.mount.entry = "), ==, 3);
    g_assert_false(at_exists("inner/sub"));                                       
    g_assert_false(at_exists("top/a/sub/sub"));
    g_assert_false(at_exists("top/sub"));
    g_assert_true(at_exists("top/a/sub"));
    at_end();
}

                                                                                          
                                                
static void
test_attach_stopped_inside_type_mismatch_refused(void)
{
    static const struct { const gchar *src, *cpath, *rel; } cases[] = {
        { "a", "/mnt/a/file", "file" }, { "f", "/mnt/a/dir", "dir" }, { "a", "/mnt/a/fifo", "fifo" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        at_begin("lxc.uts.name = c1\n");
        at_mk("outer/file", 'f', NULL);
        at_mk("outer/dir", 'd', NULL);
        at_mk("outer/fifo", 'p', NULL);
        at_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/outer mnt/a none bind 0 0\n");
        g_autofree gchar *msg = at_nest_ctx("/mnt/a", cases[i].rel, AT_TYPE_TEXT);
        at_bad_path_refused(cases[i].src, "/srv/b", cases[i].cpath, FALSE, msg);
        at_end();
    }
}

                                                                                          
                                                       
static void
test_attach_stopped_inside_symlink_refused(void)
{
    static const struct { const gchar *cpath, *rel; } cases[] = {
        { "/mnt/a/lnk", "lnk" }, { "/mnt/a/lnk/x", "lnk/x" }, { "/mnt/a/abs/x", "abs/x" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        at_begin("lxc.uts.name = c1\n");
        at_mk("a/x", 'd', NULL);
        at_mk("outer", 'd', NULL);
        at_mk("outer/lnk", 'l', "../a");
        g_autofree gchar *abs_target = g_build_filename(AT.srcdir, "a", NULL);
        at_mk("outer/abs", 'l', abs_target);
        at_config("lxc.uts.name = c1\nlxc.mount.entry = @S@/outer mnt/a none bind 0 0\n");
        g_autofree gchar *msg = at_nest_ctx("/mnt/a", cases[i].rel, AT_LINK_TEXT);
        at_bad_path_refused("a", "/srv/b", cases[i].cpath, FALSE, msg);
        at_end();
    }
}

                                                                                                                           
                                                                                                           
static void
test_attach_persists_canonical_dest(void)
{
    at_begin("lxc.uts.name = c1\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/./a/", FALSE, FALSE, &err));
    g_assert_no_error(err);
    close(AT.src_fd);
    AT.src_fd = -1;
    g_assert_true(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/b", "/mnt//b/.", FALSE, TRUE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a mnt/a none bind 0 0"), ==, 1);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/b mnt/b none bind 0 0"), ==, 1);
    g_autofree gchar *seq = at_seq("mount_clone ", NULL);
    g_assert_cmpstr(seq, ==, "mount_clone /mnt/b 0");
    g_assert_true(pcv_lxc_volume_config_drop(AT.lxc, "c1", "/mnt/a", FALSE, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/a "), ==, 0);
    g_assert_cmpuint(at_lines("lxc.mount.entry = /srv/b mnt/b none bind 0 0"), ==, 1);
    at_end();
}

                                                                                                 
                                                                                                    
static void
test_mount_running_file_source_creates_file(void)
{
    at_begin("lxc.uts.name = c1\n");
    AT.src_root = "/data/f";
    guint64 id = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src("f"), "/etc/conf.d/app.conf", FALSE, &id, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(id, ==, 500);
    g_autofree gchar *seq = at_seq("mount_clone ", NULL);
    g_assert_cmpstr(seq, ==, "mount_clone /etc/conf.d/app.conf 0");
    g_autofree gchar *dir = g_build_filename(AT.rootfs, "etc", "conf.d", NULL), *leaf = g_build_filename(dir, "app.conf", NULL);
    struct stat st;
    g_assert_cmpint(lstat(dir, &st), ==, 0);
    g_assert_true(S_ISDIR(st.st_mode));
    g_assert_cmpint(lstat(leaf, &st), ==, 0);
    g_assert_true(S_ISREG(st.st_mode));
    g_assert_cmpint(st.st_size, ==, 0);
    g_assert_cmpuint(st.st_uid, ==, geteuid());
    g_assert_cmpuint(st.st_mode & 0111, ==, 0);                                                
    at_end();
}

                                                                                                      
                                                                                             
                                                                                                                    
static void
test_mount_running_created_modes_ignore_umask(void)
{
    at_begin("lxc.uts.name = c1\n");
    g_autofree gchar *srv = g_build_filename(AT.rootfs, "srv", NULL);
    g_assert_cmpint(g_mkdir(srv, 0700), ==, 0);                           
    mode_t old_mask = umask(077);
    struct { const gchar *src, *cpath; } cases[] = { { "a", "/mnt/new1/new2/data" }, { "a", "/srv/inner" }, { "f", "/etc/conf.d/app.conf" } };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        guint64 id = 0;
        GError *err = NULL;
        gboolean ok = pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src(cases[i].src), cases[i].cpath, FALSE, &id, &err);
        g_assert_no_error(err);
        g_assert_true(ok);
        close(AT.src_fd);
        AT.src_fd = -1;
    }
    umask(old_mask);
    const struct { const gchar *rel; guint mode; } want[] = {
        { "mnt", 0755 }, { "mnt/new1", 0755 }, { "mnt/new1/new2", 0755 }, { "mnt/new1/new2/data", 0755 },
        { "srv", 0700 }, { "srv/inner", 0755 }, { "etc", 0755 }, { "etc/conf.d", 0755 }, { "etc/conf.d/app.conf", 0644 },
    };
    for (guint i = 0; i < G_N_ELEMENTS(want); i++) {
        g_autofree gchar *path = g_build_filename(AT.rootfs, want[i].rel, NULL);
        g_assert_cmpuint(se_mode(path), ==, want[i].mode);
    }
    at_end();
}

                                                              
static void
test_mount_running_file_source_existing_file(void)
{
    at_begin("lxc.uts.name = c1\n");
    g_autofree gchar *etc = g_build_filename(AT.rootfs, "etc", NULL), *hosts = g_build_filename(etc, "hosts", NULL);
    g_assert_cmpint(g_mkdir(etc, 0755), ==, 0);
    g_assert_true(g_file_set_contents(hosts, "127.0.0.1 localhost\n", -1, NULL));
    guint64 id = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src("f"), "/etc/hosts", FALSE, &id, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(id, ==, 500);
    g_assert_cmpuint(at_count("mount_clone /etc/hosts 0"), ==, 1);
    g_autofree gchar *text = NULL;
    g_assert_true(g_file_get_contents(hosts, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "127.0.0.1 localhost\n");
    at_end();
}

                                                                                                 
                                                                                                          
static void
test_mount_running_id_from_clone(void)
{
    at_begin("lxc.uts.name = c1\n");
    AT.leaf_fail_after_clone = TRUE;
    guint64 id = 0;
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src("a"), "/mnt/a", FALSE, &id, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(id, ==, 500);
    g_assert_cmpuint(AT.clones, ==, 1);
    at_end();
}

                                                                                                            
                         
static void
test_attach_mount_clone_failure_keeps_config(void)
{
    at_begin("lxc.uts.name = c1\n");
    AT.clone_fail = TRUE;
    g_autofree gchar *before = at_cfg_text();
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", "/mnt/a", FALSE, TRUE, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_cmpstr(err->message, ==, "statx(STATX_MNT_ID): Operation not supported");
    g_clear_error(&err);
    g_assert_cmpuint(at_count("umount "), ==, 0);
    g_autofree gchar *after = at_cfg_text();
    g_assert_cmpstr(after, ==, before);
    at_end();
}

                                                                                                           
                                                                                                              
                                                                               
static void
test_attach_root_target_refused(void)
{
    static const gchar *paths[] = { "/", "//", "/./", "/.//." };
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        for (guint running = 0; running < 2; running++) {
            at_begin("lxc.uts.name = c1\nlxc.mount.entry = /srv/z mnt/z none bind 0 0\n");
            g_autofree gchar *before = at_cfg_text();
            GError *err = NULL;
            g_assert_false(pcv_lxc_volume_attach(AT.lxc, "c1", at_src("a"), "/srv/a", paths[i], FALSE, running, &err));
            g_assert_error(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
            g_assert_cmpstr(err->message, ==, PCV_LXC_VOLUME_ROOT_TARGET_MSG);
            g_clear_error(&err);
            g_assert_cmpuint(at_count("source_open "), ==, 0);                              
            g_assert_cmpuint(at_count("mount_clone "), ==, 0);
            g_autofree gchar *after = at_cfg_text();
            g_assert_cmpstr(after, ==, before);
            at_end();
        }
    }
}

                                                                                                                  
                                                                               
static void
test_mount_running_special_file_target_refused(void)
{
    at_begin("lxc.uts.name = c1\n");
    g_autofree gchar *run = g_build_filename(AT.rootfs, "run", NULL), *fifo = g_build_filename(run, "pipe", NULL);
    g_assert_cmpint(g_mkdir(run, 0755), ==, 0);
    g_assert_cmpint(mkfifo(fifo, 0644), ==, 0);
    guint64 id = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src("a"), "/run/pipe", FALSE, &id, &err));
    g_assert_error(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_assert_cmpstr(err->message, ==, PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
    g_clear_error(&err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    struct stat st;
    g_assert_cmpint(lstat(fifo, &st), ==, 0);
    g_assert_true(S_ISFIFO(st.st_mode));
    at_end();
}

                                                                                                     
                                                                                              
static void
at_mismatch_case(const gchar *src, const gchar *cpath, gboolean target_is_dir)
{
    at_begin("lxc.uts.name = c1\n");
    g_autofree gchar *target = g_strconcat(AT.rootfs, cpath, NULL), *parent = g_path_get_dirname(target);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0755), ==, 0);
    if (target_is_dir)
        g_assert_cmpint(g_mkdir(target, 0755), ==, 0);
    else
        g_assert_true(g_file_set_contents(target, "x\n", -1, NULL));
    guint64 id = 0;
    GError *err = NULL;
    g_assert_false(pcv_lxc_volume_mount_running(AT.lxc, "c1", at_src(src), cpath, FALSE, &id, &err));
    g_assert_error(err, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_assert_cmpstr(err->message, ==, PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
    g_clear_error(&err);
    g_assert_cmpuint(at_count("mount_clone "), ==, 0);
    g_assert_true(g_file_test(target, target_is_dir ? G_FILE_TEST_IS_DIR : G_FILE_TEST_IS_REGULAR));
    at_end();
}

                                              
static void
test_mount_running_mismatch_dir_onto_file(void)
{
    at_mismatch_case("a", "/etc/hosts", FALSE);
}

                                          
static void
test_mount_running_mismatch_file_onto_dir(void)
{
    at_mismatch_case("f", "/srv/data", TRUE);
}

void test_lxc_volume_mount_register(void);

void
test_lxc_volume_mount_register(void)
{
    g_test_add_func("/lxc_volume_mount/residue_none", test_residue_none);
    g_test_add_func("/lxc_volume_mount/residue_reverse_order_id_match", test_residue_reverse_order_id_match);
    g_test_add_func("/lxc_volume_mount/residue_parent_shared_child_no_propagation", test_residue_parent_shared_child_no_propagation);
    g_test_add_func("/lxc_volume_mount/residue_moved_child_listed_first", test_residue_moved_child_listed_first);
    g_test_add_func("/lxc_volume_mount/detach_running_shared_legacy_copy_no_propagation",
                    test_detach_running_shared_legacy_copy_no_propagation);
    g_test_add_func("/lxc_volume_mount/make_private_fail_parent_skips_subtree", test_make_private_fail_parent_skips_subtree);
    g_test_add_func("/lxc_volume_mount/residue_stack_slave_pair_cleared", test_residue_stack_slave_pair_cleared);
    g_test_add_func("/lxc_volume_mount/residue_stack_shared_covered_refused", test_residue_stack_shared_covered_refused);
    g_test_add_func("/lxc_volume_mount/detach_running_stack_same_dest", test_detach_running_stack_same_dest);
    g_test_add_func("/lxc_volume_mount/detach_running_stack_shared_refused", test_detach_running_stack_shared_refused);
    g_test_add_func("/lxc_volume_mount/residue_stack_bottom_under_shared_parent", test_residue_stack_bottom_under_shared_parent);
    g_test_add_func("/lxc_volume_mount/residue_shadowed_deep_mount", test_residue_shadowed_deep_mount);
    g_test_add_func("/lxc_volume_mount/residue_id_mismatch_skipped", test_residue_id_mismatch_skipped);
    g_test_add_func("/lxc_volume_mount/residue_skip_until_next_success", test_residue_skip_until_next_success);
    g_test_add_func("/lxc_volume_mount/residue_private_failure_no_umount", test_residue_private_failure_no_umount);
    g_test_add_func("/lxc_volume_mount/residue_prefix_boundary", test_residue_prefix_boundary);
    g_test_add_func("/lxc_volume_mount/residue_prefix_from_fd_path", test_residue_prefix_from_fd_path);
    g_test_add_func("/lxc_volume_mount/residue_staging_prefix_and_dir_removal", test_residue_staging_prefix_and_dir_removal);
    g_test_add_func("/lxc_volume_mount/residue_attempt_cap", test_residue_attempt_cap);
    g_test_add_func("/lxc_volume_mount/residue_many_members_one_call", test_residue_many_members_one_call);
    g_test_add_func("/lxc_volume_mount/residue_file_mount_points", test_residue_file_mount_points);
    g_test_add_func("/lxc_volume_mount/residue_symlink_leaf_not_followed", test_residue_symlink_leaf_not_followed);
    g_test_add_func("/lxc_volume_mount/residue_missing_rootfs_ok", test_residue_missing_rootfs_ok);
    g_test_add_func("/lxc_volume_mount/residue_prefix_open_error", test_residue_prefix_open_error);
    g_test_add_func("/lxc_volume_mount/open_leaf_real_types", test_open_leaf_real_types);
    g_test_add_func("/lxc_volume_mount/start_prepare_order", test_start_prepare_order);
    g_test_add_func("/lxc_volume_mount/start_prepare_modes", test_start_prepare_modes);
    g_test_add_func("/lxc_volume_mount/prepare_dir_chown_via_child", test_prepare_dir_chown_via_child);
    g_test_add_func("/lxc_volume_mount/start_prepare_link_source_fails", test_start_prepare_link_source_fails);
    g_test_add_func("/lxc_volume_mount/start_prepare_missing_source_skips", test_start_prepare_missing_source_skips);
    g_test_add_func("/lxc_volume_mount/start_prepare_mount_failure_aborts", test_start_prepare_mount_failure_aborts);
    g_test_add_func("/lxc_volume_mount/start_prepare_refuses_foreign_stage_dir", test_start_prepare_refuses_foreign_stage_dir);
    g_test_add_func("/lxc_volume_mount/start_prepare_concurrent_stage_root", test_start_prepare_concurrent_stage_root);
    g_test_add_func("/lxc_volume_mount/start_prepare_creates_missing_target_dir", test_start_prepare_creates_missing_target_dir);
    g_test_add_func("/lxc_volume_mount/start_prepare_creates_missing_target_file", test_start_prepare_creates_missing_target_file);
    g_test_add_func("/lxc_volume_mount/start_prepare_target_type_mismatch_fails", test_start_prepare_target_type_mismatch_fails);
    g_test_add_func("/lxc_volume_mount/start_prepare_target_symlink_refused", test_start_prepare_target_symlink_refused);
    g_test_add_func("/lxc_volume_mount/start_prepare_missing_rootfs_skips_target", test_start_prepare_missing_rootfs_skips_target);
    g_test_add_func("/lxc_volume_mount/start_prepare_nested_missing_fails_clean", test_start_prepare_nested_missing_fails_clean);
    g_test_add_func("/lxc_volume_mount/start_prepare_nested_present_ok_no_rootfs_ensure",
                    test_start_prepare_nested_present_ok_no_rootfs_ensure);
    g_test_add_func("/lxc_volume_mount/start_prepare_nested_legacy_cover_uses_topmost",
                    test_start_prepare_nested_legacy_cover_uses_topmost);
    g_test_add_func("/lxc_volume_mount/runtime_config_file_modes", test_runtime_config_file_modes);
    g_test_add_func("/lxc_volume_mount/runtime_config_remove_ok", test_runtime_config_remove_ok);
    g_test_add_func("/lxc_volume_mount/volume_set_staging_ident", test_volume_set_staging_ident);
    g_test_add_func("/lxc_volume_mount/volume_set_dot_spelling_shares_slots", test_volume_set_dot_spelling_shares_slots);
    g_test_add_func("/lxc_volume_mount/attach_dup_same_source", test_attach_dup_same_source);
    g_test_add_func("/lxc_volume_mount/attach_dup_subdir_not_dup", test_attach_dup_subdir_not_dup);
    g_test_add_func("/lxc_volume_mount/attach_dup_ignores_unidentifiable", test_attach_dup_ignores_unidentifiable);
    g_test_add_func("/lxc_volume_mount/attach_persist_failure_reverts_mount", test_attach_persist_failure_reverts_mount);
    g_test_add_func("/lxc_volume_mount/attach_persist_failure_keeps_existing_mounts", test_attach_persist_failure_keeps_existing_mounts);
    g_test_add_func("/lxc_volume_mount/attach_persist_failure_revert_failure_reported", test_attach_persist_failure_revert_failure_reported);
    g_test_add_func("/lxc_volume_mount/attach_stopped_no_mount", test_attach_stopped_no_mount);
    g_test_add_func("/lxc_volume_mount/attach_running_occupied_by_line_refused", test_attach_running_occupied_by_line_refused);
    g_test_add_func("/lxc_volume_mount/attach_running_occupied_by_mount_refused", test_attach_running_occupied_by_mount_refused);
    g_test_add_func("/lxc_volume_mount/attach_running_dest_spelling_refused", test_attach_running_dest_spelling_refused);
    g_test_add_func("/lxc_volume_mount/attach_stopped_same_path_allowed", test_attach_stopped_same_path_allowed);
    g_test_add_func("/lxc_volume_mount/attach_running_inside_line_refused", test_attach_running_inside_line_refused);
    g_test_add_func("/lxc_volume_mount/attach_running_inside_mount_refused", test_attach_running_inside_mount_refused);
    g_test_add_func("/lxc_volume_mount/attach_running_inside_legacy_cover_names_topmost",
                    test_attach_running_inside_legacy_cover_names_topmost);
    g_test_add_func("/lxc_volume_mount/attach_running_sibling_prefix_not_inside", test_attach_running_sibling_prefix_not_inside);
    g_test_add_func("/lxc_volume_mount/attach_cover_inner_refused_running", test_attach_cover_inner_refused_running);
    g_test_add_func("/lxc_volume_mount/attach_cover_inner_refused_stopped", test_attach_cover_inner_refused_stopped);
    g_test_add_func("/lxc_volume_mount/attach_cover_sibling_prefix_allowed", test_attach_cover_sibling_prefix_allowed);
    g_test_add_func("/lxc_volume_mount/attach_stopped_inside_missing_refused", test_attach_stopped_inside_missing_refused);
    g_test_add_func("/lxc_volume_mount/attach_stopped_inside_present_allowed", test_attach_stopped_inside_present_allowed);
    g_test_add_func("/lxc_volume_mount/attach_stopped_inside_legacy_cover_uses_topmost",
                    test_attach_stopped_inside_legacy_cover_uses_topmost);
    g_test_add_func("/lxc_volume_mount/attach_stopped_inside_type_mismatch_refused", test_attach_stopped_inside_type_mismatch_refused);
    g_test_add_func("/lxc_volume_mount/attach_stopped_inside_symlink_refused", test_attach_stopped_inside_symlink_refused);
    g_test_add_func("/lxc_volume_mount/attach_persists_canonical_dest", test_attach_persists_canonical_dest);
    g_test_add_func("/lxc_volume_mount/mount_running_file_source_creates_file", test_mount_running_file_source_creates_file);
    g_test_add_func("/lxc_volume_mount/mount_running_file_source_existing_file", test_mount_running_file_source_existing_file);
    g_test_add_func("/lxc_volume_mount/mount_running_created_modes_ignore_umask", test_mount_running_created_modes_ignore_umask);
    g_test_add_func("/lxc_volume_mount/mount_running_mismatch_dir_onto_file", test_mount_running_mismatch_dir_onto_file);
    g_test_add_func("/lxc_volume_mount/mount_running_mismatch_file_onto_dir", test_mount_running_mismatch_file_onto_dir);
    g_test_add_func("/lxc_volume_mount/mount_running_id_from_clone", test_mount_running_id_from_clone);
    g_test_add_func("/lxc_volume_mount/attach_mount_clone_failure_keeps_config", test_attach_mount_clone_failure_keeps_config);
    g_test_add_func("/lxc_volume_mount/attach_root_target_refused", test_attach_root_target_refused);
    g_test_add_func("/lxc_volume_mount/mount_running_special_file_target_refused", test_mount_running_special_file_target_refused);
}
