#include "lxc_volume_stage.h"
#include "lxc_volume_guard.h"

#include <glib.h>
#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

#define LXC_LOG_DOM "lxc_driver"

GQuark
pcv_lxc_volume_error_quark(void)
{
    return g_quark_from_static_string("pcv-lxc-volume-error-quark");
}

static GMutex G_vol_roots_mu;
static gchar *G_vol_volstage_test, *G_vol_runtime_test;

void
pcv_lxc_volume_set_roots_for_test(const gchar *volstage_root, const gchar *runtime_root)
{
    g_mutex_lock(&G_vol_roots_mu);
    g_free(G_vol_volstage_test);
    G_vol_volstage_test = g_strdup(volstage_root);
    g_free(G_vol_runtime_test);
    G_vol_runtime_test = g_strdup(runtime_root);
    g_mutex_unlock(&G_vol_roots_mu);
}

static gchar *
_volstage_root(void)
{
    g_mutex_lock(&G_vol_roots_mu);
    gchar *root = g_strdup(G_vol_volstage_test ? G_vol_volstage_test : PCV_LXC_VOLSTAGE_ROOT);
    g_mutex_unlock(&G_vol_roots_mu);
    return root;
}

static gchar *
_runtime_root(void)
{
    g_mutex_lock(&G_vol_roots_mu);
    gchar *root = g_strdup(G_vol_runtime_test ? G_vol_runtime_test : PCV_LXC_RUNTIME_CONFIG_DIR);
    g_mutex_unlock(&G_vol_roots_mu);
    return root;
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
pcv_lxc_volume_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error)
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
                PCV_LOG_WARN(LXC_LOG_DOM, "volume mount: cannot make the failed mount private: %s", g_strerror(errno));
            gchar *path = g_strdup_printf("/proc/self/fd/%d", tree);
            if (umount2(path, MNT_DETACH) < 0)
                PCV_LOG_WARN(LXC_LOG_DOM, "volume mount: cannot remove the mount after the slave switch failed: %s",
                             g_strerror(errno));
            g_free(path);
            ok = FALSE;
        }
    }
    close(tree);
    return ok;
}

static int
_stage_mismatch(GError **error, const gchar *what)
{
    g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH, what);
    return -1;
}

static gboolean
_stage_owner_ok(const struct stat *st, guint mode, gint64 gid)
{
    const gid_t want = gid > 0 ? (gid_t)gid : getegid();
    return st->st_uid == geteuid() && st->st_gid == want && (guint)(st->st_mode & 07777) == mode;
}

static gchar *
_fd_path(int fd)
{
    g_autofree gchar *link = g_strdup_printf("/proc/self/fd/%d", fd);
    return g_file_read_link(link, NULL);
}

static int
_prepare_dir(int parent_fd, const gchar *name, guint mode, gint64 gid, GError **error)
{
    const gboolean made = mkdirat(parent_fd, name, 0700) == 0;
    if (!made && errno != EEXIST) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "create %s: %s", name, g_strerror(e));
        return -1;
    }
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == ELOOP || e == ENOTDIR)
            return _stage_mismatch(error, "volume staging directory has unexpected owner or mode");
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "open %s: %s", name, g_strerror(e));
        return -1;
    }
    if (made && gid > 0) {
        g_autofree gchar *parent = _fd_path(parent_fd);
        g_autofree gchar *path = parent ? g_build_filename(parent, name, NULL) : NULL;
        g_autofree gchar *owner = g_strdup_printf("0:%" G_GINT64_FORMAT, gid);
        const gchar *argv[] = { "chown", owner, "--", path, NULL };
        g_autofree gchar *err_out = NULL;
        GError *local = NULL;
        gboolean ran = path && pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_STORAGE, NULL, &err_out, &local);
        if (!ran) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "chown failed: %s",
                        !path ? "cannot resolve the staging directory"
                              : local ? local->message : (err_out && *err_out ? g_strstrip(err_out) : "unknown"));
            g_clear_error(&local);
            close(fd);
            return -1;
        }
    }
    if (made && fchmod(fd, mode) < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "chmod %s: %s", name, g_strerror(e));
        close(fd);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || !_stage_owner_ok(&st, mode, gid)) {
        close(fd);
        return _stage_mismatch(error, "volume staging directory has unexpected owner or mode");
    }
    return fd;
}

static int
_prepare_slot(int dir_fd, const gchar *name, gboolean is_dir, GError **error)
{
    int rc = 0;
    if (is_dir) {
        rc = mkdirat(dir_fd, name, 0700);
    } else {
        int w = openat(dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (w < 0)
            rc = -1;
        else
            close(w);
    }
    int fd = rc == 0 || errno == EEXIST ? openat(dir_fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (fd < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "create %s: %s", name, g_strerror(e));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || (st.st_mode & S_IFMT) != (is_dir ? S_IFDIR : S_IFREG) ||
        !_stage_owner_ok(&st, is_dir ? 0700 : 0600, 0)) {
        close(fd);
        return _stage_mismatch(error, "volume staging slot has unexpected type, owner or mode");
    }
    return fd;
}

static int
_open_dir(const gchar *path, GError **error)
{
    int fd = open(path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(e), g_strerror(e));
    }
    return fd;
}

static void
_stage_fail(GError **error, GError *e, const gchar *path, const gchar *container_path)
{
    if (g_error_matches(e, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "staging path %s has unexpected type, owner or mode; refusing to start", path);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot prepare the volume staging for %s: %s", container_path,
                    e ? e->message : "unknown error");
    g_clear_error(&e);
}

static gboolean
_stage_dir_open(const gchar *stage_root, const gchar *name, guint mode, gint64 gid,
                const gchar *container_path, int *n_fd, GError **error)
{
    g_autofree gchar *parent = g_path_get_dirname(stage_root), *base = g_path_get_basename(stage_root);
    g_autofree gchar *n_path = g_build_filename(stage_root, name, NULL);
    const gchar *at = stage_root;
    GError *e = NULL;
    g_mkdir_with_parents(parent, 0755);
    int pfd = _open_dir(parent, &e), sfd = -1;
    if (pfd >= 0) {
        static GMutex stage_root_mu;
        g_mutex_lock(&stage_root_mu);
        sfd = _prepare_dir(pfd, base, 0711, 0, &e);
        g_mutex_unlock(&stage_root_mu);
        close(pfd);
    }
    if (sfd >= 0) {
        at = n_path;
        *n_fd = _prepare_dir(sfd, name, mode, gid, &e);
        close(sfd);
    }
    if (*n_fd >= 0)
        return TRUE;
    _stage_fail(error, e, at, container_path);
    return FALSE;
}

static gboolean
_write_all(int fd, const gchar *text)
{
    gsize len = strlen(text), off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return FALSE;
        off += (gsize)w;
    }
    return TRUE;
}

static int
_runtime_dir_at(int at, const gchar *name)
{
    if (mkdirat(at, name, 0700) < 0 && errno != EEXIST)
        return -1;
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    int e = 0;
    if (fstat(fd, &st) < 0)
        e = errno;
    else if (st.st_uid != geteuid())
        e = EPERM;
    else if (fchmod(fd, 0700) < 0)
        e = errno;
    if (e != 0) {
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static gboolean
_runtime_config_write(const gchar *name, const gchar *text, GError **error)
{
    g_autofree gchar *root = _runtime_root();
    g_autofree gchar *parent = g_path_get_dirname(root);
    g_autofree gchar *dir = g_build_filename(root, name, NULL);
    g_autofree gchar *tmp = NULL;
    const gchar *at = parent;
    int rfd = -1, nfd = -1, fd = -1;
    gboolean ok = g_mkdir_with_parents(parent, 0755) == 0;
    if (ok) {
        at = root;
        ok = (rfd = _runtime_dir_at(AT_FDCWD, root)) >= 0;
    }
    if (ok) {
        at = dir;
        ok = (nfd = _runtime_dir_at(rfd, name)) >= 0;
    }
    for (guint i = 0; ok && fd < 0 && i < 8; i++) {
        g_free(tmp);
        tmp = g_strdup_printf(".config.%08x", g_random_int());
        fd = openat(nfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            ok = FALSE;
    }
    ok = ok && fd >= 0 && _write_all(fd, text) && fchmod(fd, 0600) == 0;
    int e = errno;
    if (fd >= 0 && close(fd) < 0 && ok) {
        ok = FALSE;
        e = errno;
    }
    if (ok && renameat(nfd, tmp, nfd, "config") < 0) {
        ok = FALSE;
        e = errno;
    }
    if (!ok && fd >= 0)
        (void)unlinkat(nfd, tmp, 0);
    if (!ok)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", at, g_strerror(e));
    if (nfd >= 0)
        close(nfd);
    if (rfd >= 0)
        close(rfd);
    return ok;
}

gchar *
pcv_lxc_runtime_config_path(const gchar *name)
{
    g_autofree gchar *root = _runtime_root();
    return g_build_filename(root, name, "config", NULL);
}

static gboolean
_name_ok(const gchar *name)
{
    return name && *name && !strchr(name, '/') && !g_str_equal(name, ".") && !g_str_equal(name, "..");
}

static GPtrArray *
_list_at(int dir_fd, GError **error)
{
    int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "list directory: %s", g_strerror(e));
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    struct dirent *de;
    int e = 0;
    for (;;) {
        errno = 0;
        if (!(de = readdir(d))) {
            e = errno;
            break;
        }
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            g_ptr_array_add(out, g_strdup(de->d_name));
    }
    closedir(d);
    if (e != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "list directory: %s", g_strerror(e));
        g_ptr_array_unref(out);
        return NULL;
    }
    return out;
}

static gboolean
_remove_at(int dir_fd, const gchar *name, GError **error)
{
    int rc = unlinkat(dir_fd, name, 0);
    if (rc < 0 && errno == EISDIR)
        rc = unlinkat(dir_fd, name, AT_REMOVEDIR);
    if (rc == 0 || errno == ENOENT)
        return TRUE;
    int e = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "remove %s: %s", name, g_strerror(e));
    return FALSE;
}

void
pcv_lxc_volstage_clear(const gchar *name)
{
    if (!_name_ok(name))
        return;
    g_autofree gchar *stage_root = _volstage_root();
    g_autofree gchar *n_dir = g_build_filename(stage_root, name, NULL);
    int root_fd = open(stage_root, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int n_fd = open(n_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (n_fd < 0) {
        if (root_fd >= 0)
            close(root_fd);
        return;
    }
    GError *e = NULL;
    GPtrArray *ents = _list_at(n_fd, &e);
    gboolean ok = ents != NULL;
    for (guint i = 0; ents && i < ents->len; i++) {
        const gchar *slot = g_ptr_array_index(ents, i);
        int sfd = openat(n_fd, slot, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (sfd >= 0) {
            g_autofree gchar *p = g_strdup_printf("/proc/self/fd/%d", sfd);
            umount2(p, MNT_DETACH);
            close(sfd);
        }
        ok = _remove_at(n_fd, slot, &e) && ok;
    }
    if (ok && root_fd >= 0)
        ok = _remove_at(root_fd, name, &e);
    if (!ok)
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': cannot clear the volume staging directory: %s", name,
                     e ? e->message : "unknown error");
    g_clear_error(&e);
    if (ents)
        g_ptr_array_unref(ents);
    close(n_fd);
    if (root_fd >= 0)
        close(root_fd);
}

void
pcv_lxc_runtime_config_remove(const gchar *name)
{
    if (!_name_ok(name))
        return;
    g_autofree gchar *root = _runtime_root();
    GError *e = NULL;
    int rfd = open(root, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int nfd = rfd >= 0 ? openat(rfd, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    gboolean ok = nfd >= 0 || errno == ENOENT;
    if (!ok)
        g_set_error(&e, G_IO_ERROR, g_io_error_from_errno(errno), "open: %s", g_strerror(errno));
    GPtrArray *ents = nfd >= 0 ? _list_at(nfd, &e) : NULL;
    if (nfd >= 0)
        ok = ents != NULL;
    for (guint i = 0; ok && ents && i < ents->len; i++)
        ok = _remove_at(nfd, g_ptr_array_index(ents, i), &e);
    if (ok && nfd >= 0)
        ok = _remove_at(rfd, name, &e);
    if (!ok)
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': cannot remove the runtime config directory %s/%s: %s", name, root, name,
                     e ? e->message : "unknown error");
    g_clear_error(&e);
    if (ents)
        g_ptr_array_unref(ents);
    if (nfd >= 0)
        close(nfd);
    if (rfd >= 0)
        close(rfd);
}

void
pcv_lxc_volume_start_abort(const gchar *lxc_path, const gchar *name)
{
    (void)lxc_path;
    pcv_lxc_volstage_clear(name);
    pcv_lxc_runtime_config_remove(name);
}

static gboolean
_config_write_atomic(const gchar *lxc_path, const gchar *name, const gchar *text, GError **error)
{
    g_autofree gchar *dir = g_build_filename(lxc_path, name, NULL);
    int dfd = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "open %s: %s", dir, g_strerror(errno));
        return FALSE;
    }
    g_autofree gchar *tmp = NULL;
    int fd = -1;
    gboolean ok = TRUE;
    for (guint i = 0; fd < 0 && i < 8; i++) {
        g_free(tmp);
        tmp = g_strdup_printf(".config.%08x", g_random_int());
        fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) {
            ok = FALSE;
            break;
        }
    }
    ok = ok && fd >= 0 && _write_all(fd, text);
    int e = errno;
    if (fd >= 0 && close(fd) < 0 && ok) {
        ok = FALSE;
        e = errno;
    }
    if (ok && renameat(dfd, tmp, dfd, "config") < 0) {
        ok = FALSE;
        e = errno;
    }
    if (!ok && fd >= 0)
        (void)unlinkat(dfd, tmp, 0);
    if (!ok)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "write %s/config: %s", dir, g_strerror(e));
    close(dfd);
    return ok;
}

gboolean
pcv_lxc_volume_start_prepare(const gchar *lxc_path, const gchar *name, GError **error)
{
    g_autofree gchar *cfg_path = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(cfg_path, &text, NULL, &e)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e->message);
        g_clear_error(&e);
        return FALSE;
    }
    g_autofree gchar *hooked = pcv_lxc_config_with_guard_hook(text);
    if (hooked) {
        if (!_config_write_atomic(lxc_path, name, hooked, error))
            return FALSE;
        g_free(text);
        text = g_steal_pointer(&hooked);
    }
    pcv_lxc_volstage_clear(name);
    gint64 gid = 0;
    const gboolean userns = pcv_lxc_config_root_gid(text, &gid);
    const guint n_mode = userns ? 0710 : 0700;
    if (!userns)
        gid = 0;
    g_autofree gchar *stage_root = _volstage_root();
    g_autofree gchar *rt_text = pcv_lxc_runtime_config_text(text, stage_root, name, &e);
    GPtrArray *lines = rt_text ? pcv_lxc_volstage_lines(text, stage_root, name, &e) : NULL;
    gboolean ok = lines != NULL;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e ? e->message : "unknown error");
    g_clear_error(&e);
    int n_fd = -1;
    for (guint i = 0; ok && i < lines->len; i++) {
        const PcvLxcVolstageLine *l = lines->pdata[i];
        int src = pcv_lxc_volume_source_open(l->source, NULL, &e);
        if (src < 0) {
            if (g_error_matches(e, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH)) {
                g_autofree gchar *msg = pcv_lxc_volume_guard_message(l->container_path);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, msg);
                ok = FALSE;
            } else if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
                PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' volume %s: source not opened, no staging for it: %s", name,
                             l->container_path, e ? e->message : "unknown error");
            }
            g_clear_error(&e);
            continue;
        }
        struct stat sst;
        int slot = -1;
        const guint flags = (l->readonly ? PCV_LXC_MOUNT_RO : 0) | (l->recursive ? PCV_LXC_MOUNT_RECURSIVE : 0);
        if (fstat(src, &sst) < 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot prepare the volume staging for %s: %s",
                        l->container_path, g_strerror(errno));
            ok = FALSE;
        } else if (n_fd < 0 && !_stage_dir_open(stage_root, name, n_mode, gid, l->container_path, &n_fd, error)) {
            ok = FALSE;
        } else if ((slot = _prepare_slot(n_fd, strrchr(l->slot, '/') + 1, S_ISDIR(sst.st_mode), &e)) < 0) {
            _stage_fail(error, g_steal_pointer(&e), l->slot, l->container_path);
            ok = FALSE;
        } else if (!pcv_lxc_volume_mount_clone(src, slot, flags, NULL, &e)) {
            _stage_fail(error, g_steal_pointer(&e), l->slot, l->container_path);
            ok = FALSE;
        }
        if (slot >= 0)
            close(slot);
        close(src);
    }
    if (n_fd >= 0)
        close(n_fd);
    if (ok && !_runtime_config_write(name, rt_text, &e)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e->message);
        ok = FALSE;
    }
    g_clear_error(&e);
    if (lines)
        g_ptr_array_unref(lines);
    if (!ok)
        pcv_lxc_volume_start_abort(lxc_path, name);
    return ok;
}
