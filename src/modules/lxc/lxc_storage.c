#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lxc_storage.h"
#include "utils/pcv_config.h"
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>
#include <linux/magic.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define STORAGE_MARKER "purecvisor.storage"
#define STORAGE_JOURNAL ".pcv-restore"
#define STORAGE_STAGE ".pcv-restore-new"
#define STORAGE_SNAPS ".pcv-snapshots"

typedef int StorageFd;

static void storage_fd_clear(StorageFd *fd)
{
    if (*fd >= 0) close(*fd);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(StorageFd, storage_fd_clear)

typedef struct {
    gchar fs[33];
    gchar uuid[33];
    gchar parent_uuid[33];
    guint64 id;
    gboolean readonly;
} StorageIdentity;

static gboolean storage_error(GError **error, const gchar *format, ...)
{
    va_list args;
    va_start(args, format);
    if (error && !*error)
        *error = g_error_new_valist(G_IO_ERROR, G_IO_ERROR_FAILED, format, args);
    va_end(args);
    return FALSE;
}

static gboolean component_valid(const gchar *name)
{
    if (!name || !g_ascii_isalnum(*name) || strlen(name) > 128) return FALSE;
    for (const gchar *p = name; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return FALSE;
    return TRUE;
}

static gboolean trusted_fd(int fd, gboolean directory, gboolean ancestor, GError **error)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return storage_error(error, "Storage stat failed: %s", g_strerror(errno));
    if ((directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) || st.st_uid != 0 ||
        ((st.st_mode & 0022) && !(ancestor && (st.st_mode & S_ISVTX))))
        return storage_error(error, "Storage path must have trusted root ownership and permissions");
    if (!directory && (st.st_nlink != 1 || st.st_size > 65536))
        return storage_error(error, "Invalid storage metadata file");
    return TRUE;
}

static int base_open_full(gboolean create, GError **error)
{
    const gchar *path = pcv_config_get_container_path();
    if (!path || path[0] != '/' || !path[1]) {
        storage_error(error, "Container path must be an absolute non-root directory");
        return -1;
    }
    g_auto(GStrv) parts = g_strsplit(path + 1, "/", -1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    for (guint i = 0; fd >= 0 && parts[i]; i++) {
        if (!*parts[i] && !parts[i + 1]) break;
        if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], "..")) {
            close(fd);
            storage_error(error, "Container path contains an invalid component");
            return -1;
        }
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, parts[i], 0755) == 0) {
                if (fsync(fd) < 0) {
                    int saved = errno;
                    close(fd);
                    storage_error(error, "Cannot persist container path: %s", g_strerror(saved));
                    return -1;
                }
                next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            } else if (errno == EEXIST)
                next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        close(fd);
        fd = next;
        if (fd >= 0 && !trusted_fd(fd, TRUE, parts[i + 1] && *parts[i + 1], error)) {
            close(fd);
            return -1;
        }
    }
    if (fd < 0) storage_error(error, "Cannot open trusted container path: %s", g_strerror(errno));
    return fd;
}

static int base_open(GError **error)
{
    return base_open_full(FALSE, error);
}

static int directory_open(int parent, const gchar *name, GError **error)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        storage_error(error, "Cannot open storage directory %s: %s", name, g_strerror(errno));
        return -1;
    }
    if (!trusted_fd(fd, TRUE, FALSE, error)) { close(fd); return -1; }
    return fd;
}

static gboolean ordinary_directory(int fd, GError **error)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_ino == 256)
        return storage_error(error, "Managed outer directories must not be subvolumes");
    return TRUE;
}

static int ordinary_directory_open(int parent, const gchar *name, GError **error)
{
    int fd = directory_open(parent, name, error);
    if (fd >= 0 && !ordinary_directory(fd, error)) { close(fd); return -1; }
    return fd;
}

static int container_open(const gchar *name, GError **error)
{
    if (!component_valid(name)) { storage_error(error, "Invalid container name"); return -1; }
    g_auto(StorageFd) base = base_open(error);
    return base < 0 ? -1 : directory_open(base, name, error);
}

static gboolean entry_exists(int fd, const gchar *name)
{
    struct stat st;
    return fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT;
}

static gboolean sync_directory(int fd, GError **error)
{
    return fsync(fd) == 0 || storage_error(error, "Storage directory sync failed: %s", g_strerror(errno));
}

static gchar *metadata_temp_target(const gchar *entry)
{
    gsize len = strlen(entry);
    if (!g_str_has_prefix(entry, ".pcv-write-") || len <= 48 || entry[len - 37] != '-' ||
        !g_uuid_string_is_valid(entry + len - 36)) return NULL;
    return g_strndup(entry + 11, len - 48);
}

static gboolean metadata_snapshot_target(const gchar *target)
{
    gsize len = strlen(target);
    if (len <= 13 || !g_str_has_prefix(target, ".pcv-") || !g_str_has_suffix(target, ".storage")) return FALSE;
    g_autofree gchar *snapshot = g_strndup(target + 5, len - 13);
    return component_valid(snapshot);
}

static gboolean metadata_temp_validate(int parent, const gchar *entry, GError **error)
{
    g_auto(StorageFd) fd = openat(parent, entry, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return storage_error(error, "Untrusted metadata temporary file %s; retained", entry);
    struct stat st;
    if (!trusted_fd(fd, FALSE, FALSE, error)) return FALSE;
    if (fstat(fd, &st) < 0 || (st.st_mode & 07777) != 0600)
        return storage_error(error, "Metadata temporary file %s must have mode 0600; retained", entry);
    return TRUE;
}

static gboolean metadata_temps(int parent, const gchar *target, gboolean cleanup, GError **error)
{
    g_autofree gchar *prefix = target ? g_strdup_printf(".pcv-write-%s-", target) : NULL;
    g_autoptr(GPtrArray) leftovers = g_ptr_array_new_with_free_func(g_free);
    int scan = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir = scan < 0 ? NULL : fdopendir(scan);
    if (!dir) {
        if (scan >= 0) close(scan);
        return storage_error(error, "Cannot inspect metadata temporary files");
    }
    gboolean ok = TRUE;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!g_str_has_prefix(entry->d_name, ".pcv-write-")) continue;
        g_autofree gchar *actual = metadata_temp_target(entry->d_name);
        if (!actual) {
            if (!target || g_str_has_prefix(entry->d_name, prefix)) {
                ok = storage_error(error, "Malformed metadata temporary file %s; retained", entry->d_name);
                break;
            }
        } else if (!target || !strcmp(actual, target)) {
            if ((!target && !metadata_snapshot_target(actual)) || !metadata_temp_validate(parent, entry->d_name, error)) {
                ok = storage_error(error, "Unrecognized metadata temporary file %s; retained", entry->d_name);
                break;
            }
            g_ptr_array_add(leftovers, g_strdup(entry->d_name));
        }
        errno = 0;
    }
    if (ok && errno) ok = storage_error(error, "Metadata temporary file traversal failed");
    closedir(dir);
    if (!ok || !cleanup) return ok;
    for (guint i = 0; i < leftovers->len; i++)
        if (unlinkat(parent, g_ptr_array_index(leftovers, i), 0) < 0)
            return storage_error(error, "Cannot remove stale metadata temporary file: %s", g_strerror(errno));
    return !leftovers->len || sync_directory(parent, error);
}

static GKeyFile *metadata_read(int parent, const gchar *name, GError **error)
{
    g_auto(StorageFd) fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { storage_error(error, "Cannot read storage metadata %s: %s", name, g_strerror(errno)); return NULL; }
    if (!trusted_fd(fd, FALSE, FALSE, error)) return NULL;
    gchar data[65537];
    gsize len = 0;
    while (len < sizeof(data)) {
        ssize_t n = read(fd, data + len, sizeof(data) - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { storage_error(error, "Storage metadata read failed: %s", g_strerror(errno)); return NULL; }
        if (!n) break;
        len += (gsize)n;
    }
    if (len == sizeof(data) || memchr(data, '\0', len)) {
        storage_error(error, "Invalid storage metadata length");
        return NULL;
    }
    GKeyFile *key = g_key_file_new();
    if (!g_key_file_load_from_data(key, data, len, G_KEY_FILE_NONE, error)) {
        g_key_file_unref(key);
        return NULL;
    }
    if (g_key_file_get_integer(key, "storage", "version", NULL) != 1) {
        storage_error(error, "Unsupported storage metadata version");
        g_key_file_unref(key);
        return NULL;
    }
    return key;
}

static gboolean metadata_write(int parent, const gchar *name, GKeyFile *key, GError **error)
{
    if (!metadata_temps(parent, name, TRUE, error)) return FALSE;
    gsize len;
    g_autofree gchar *data = g_key_file_to_data(key, &len, error);
    if (!data) return FALSE;
    g_autofree gchar *random = g_uuid_string_random();
    g_autofree gchar *tmp = g_strdup_printf(".pcv-write-%s-%s", name, random);
    g_auto(StorageFd) fd = openat(parent, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return storage_error(error, "Cannot create storage metadata: %s", g_strerror(errno));
    gsize written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            unlinkat(parent, tmp, 0);
            return storage_error(error, "Storage metadata write failed: %s", g_strerror(errno));
        }
        written += (gsize)n;
    }
    if (fsync(fd) < 0 || renameat(parent, tmp, parent, name) < 0) {
        int saved = errno;
        unlinkat(parent, tmp, 0);
        return storage_error(error, "Storage metadata commit failed: %s", g_strerror(saved));
    }
    return sync_directory(parent, error);
}

static void uuid_hex(const guint8 bytes[16], gchar hex[33])
{
    static const gchar digits[] = "0123456789abcdef";
    for (guint i = 0; i < 16; i++) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 15];
    }
    hex[32] = '\0';
}

static gboolean btrfs_filesystem(int fd, gchar fs[33], GError **error)
{
    struct statfs st;
    struct btrfs_ioctl_fs_info_args info = {0};
    if (fstatfs(fd, &st) < 0 || st.f_type != BTRFS_SUPER_MAGIC)
        return storage_error(error, "Explicit Btrfs storage requires a mounted Btrfs filesystem");
    if (ioctl(fd, BTRFS_IOC_FS_INFO, &info) < 0)
        return storage_error(error, "Cannot obtain Btrfs filesystem identity: %s", g_strerror(errno));
    uuid_hex(info.fsid, fs);
    return TRUE;
}

static gboolean identity_read(int parent, const gchar *name, StorageIdentity *identity, GError **error)
{
    g_auto(StorageFd) fd = directory_open(parent, name, error);
    struct stat st;
    struct btrfs_ioctl_get_subvol_info_args info = {0};
    if (fd < 0 || !btrfs_filesystem(fd, identity->fs, error)) return FALSE;
    if (fstat(fd, &st) < 0 || st.st_ino != 256 || ioctl(fd, BTRFS_IOC_GET_SUBVOL_INFO, &info) < 0)
        return storage_error(error, "%s is not an identifiable Btrfs subvolume", name);
    uuid_hex(info.uuid, identity->uuid);
    uuid_hex(info.parent_uuid, identity->parent_uuid);
    identity->id = info.treeid;
    identity->readonly = (info.flags & BTRFS_ROOT_SUBVOL_RDONLY) != 0;
    return TRUE;
}

static void identity_store(GKeyFile *key, const gchar *group, const StorageIdentity *identity)
{
    g_key_file_set_string(key, group, "fs_uuid", identity->fs);
    g_key_file_set_string(key, group, "subvol_uuid", identity->uuid);
    g_key_file_set_uint64(key, group, "subvol_id", identity->id);
}

static gboolean identity_matches(GKeyFile *key, const gchar *group, const StorageIdentity *identity, GError **error)
{
    g_autofree gchar *fs = g_key_file_get_string(key, group, "fs_uuid", NULL);
    g_autofree gchar *uuid = g_key_file_get_string(key, group, "subvol_uuid", NULL);
    guint64 id = g_key_file_get_uint64(key, group, "subvol_id", NULL);
    return (!g_strcmp0(fs, identity->fs) && !g_strcmp0(uuid, identity->uuid) && id == identity->id && id != 0) ||
        storage_error(error, "Btrfs storage identity mismatch; data retained");
}

static GKeyFile *marker_new(PcvLxcStorageKind kind)
{
    GKeyFile *key = g_key_file_new();
    g_key_file_set_integer(key, "storage", "version", 1);
    g_key_file_set_string(key, "storage", "kind", kind == PCV_LXC_STORAGE_BTRFS ? "btrfs" : "zfs");
    g_key_file_set_string(key, "storage", "state", "active");
    return key;
}

static gboolean path_within(const gchar *path, const gchar *root)
{
    gsize len = strlen(root);
    return !strncmp(path, root, len) && (!path[len] || path[len] == '/');
}

static gboolean mount_check_with_id(const gchar *path, gchar **dataset, guint64 *mount_id, GError **error)
{
    FILE *file = fopen("/proc/self/mountinfo", "re");
    if (!file) return storage_error(error, "Cannot inspect storage mount boundaries: %s", g_strerror(errno));
    gchar *line = NULL;
    size_t size = 0;
    gboolean ok = TRUE;
    while (getline(&line, &size, file) >= 0) {
        g_auto(GStrv) fields = g_strsplit(line, " ", -1);
        guint n = g_strv_length(fields);
        if (n < 10) { ok = storage_error(error, "Invalid mountinfo record"); break; }
        g_autofree gchar *mountpoint = g_strcompress(fields[4]);
        if (dataset) {
            guint separator = 6;
            while (separator < n && strcmp(fields[separator], "-")) separator++;
            if (!strcmp(path, mountpoint)) {
                if (*dataset || separator + 3 >= n) { ok = storage_error(error, "Ambiguous or invalid container mount boundary"); break; }
                gchar *end = NULL;
                guint64 id = g_ascii_strtoull(fields[0], &end, 10);
                if (!id || !end || *end) { ok = storage_error(error, "Invalid container mount identity"); break; }
                *dataset = !strcmp(fields[separator + 1], "zfs") ? g_strcompress(fields[separator + 2]) : g_strdup("");
                if (mount_id) *mount_id = id;
            }
        } else if (path_within(mountpoint, path)) {
            ok = storage_error(error, "Mounted filesystem below managed rootfs: %s", mountpoint);
            break;
        }
    }
    if (ferror(file)) ok = storage_error(error, "Cannot read mountinfo");
    free(line);
    fclose(file);
    return ok;
}

static gboolean mount_check(const gchar *path, gchar **dataset, GError **error)
{
    return mount_check_with_id(path, dataset, NULL, error);
}

static gchar *container_path(const gchar *name)
{
    return g_build_filename(pcv_config_get_container_path(), name, NULL);
}

static gchar *deletion_name(const gchar *name)
{
    return g_strdup_printf(".pcv-delete-%s.storage", name);
}

static GKeyFile *deletion_read(int base, const gchar *name, GError **error)
{
    g_autofree gchar *filename = deletion_name(name);
    g_autoptr(GKeyFile) key = metadata_read(base, filename, error);
    if (!key) return NULL;
    g_autofree gchar *state = g_key_file_get_string(key, "storage", "state", NULL);
    g_autofree gchar *kind = g_key_file_get_string(key, "storage", "kind", NULL);
    g_autofree gchar *owner = g_key_file_get_string(key, "storage", "container", NULL);
    g_autofree gchar *expected_fs = g_key_file_get_string(key, "storage", "fs_uuid", NULL);
    gchar fs[33];
    if (g_strcmp0(state, "deleting") || g_strcmp0(kind, "btrfs") || g_strcmp0(owner, name) ||
        !btrfs_filesystem(base, fs, error) || g_strcmp0(expected_fs, fs)) {
        storage_error(error, "Invalid container deletion tombstone");
        return NULL;
    }
    return g_steal_pointer(&key);
}

static gboolean zfs_dataset(const gchar *name, gchar **dataset, GError **error)
{
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    g_auto(StorageFd) root = openat(container, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) return storage_error(error, "ZFS rootfs must be a direct directory without symlink escapes");
    g_autofree gchar *path = container_path(name);
    guint64 outer_mount = 0, root_mount = 0;
    if (!mount_check_with_id(path, dataset, &outer_mount, error)) return FALSE;
    g_autofree gchar *root_path = g_build_filename(path, "rootfs", NULL);
    g_autofree gchar *root_dataset = NULL;
    if (!mount_check_with_id(root_path, &root_dataset, &root_mount, error)) return FALSE;
    if (root_dataset && !*root_dataset) return storage_error(error, "Foreign filesystem covers container rootfs");
    if (*dataset && **dataset && root_dataset) return storage_error(error, "Ambiguous ZFS container and rootfs mounts");
    if (root_dataset) {
        g_free(*dataset);
        *dataset = g_steal_pointer(&root_dataset);
    }
    if (!*dataset || !**dataset || strchr(*dataset, '@') || strchr(*dataset, '\n'))
        return storage_error(error, "No exact mounted ZFS dataset for container %s", name);
    struct statx st = {0};
    if (statx(root, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &st) < 0 ||
        !(st.stx_mask & STATX_MNT_ID) || st.stx_mnt_id != (root_mount ? root_mount : outer_mount))
        return storage_error(error, "Live rootfs mount does not match the selected ZFS dataset");
    return TRUE;
}

gboolean pcv_lxc_storage_default(PcvLxcStorageKind *kind, GError **error)
{
    g_autofree gchar *value = pcv_config_dup_raw_value("container", "storage_backend");
    if (!value || !strcmp(value, "zfs")) { *kind = PCV_LXC_STORAGE_ZFS; return TRUE; }
    if (!strcmp(value, "btrfs")) { *kind = PCV_LXC_STORAGE_BTRFS; return TRUE; }
    return storage_error(error, "Invalid [container] storage_backend; expected zfs or btrfs");
}

gboolean pcv_lxc_storage_prepare_create(const gchar *name, PcvLxcStorageKind kind, GError **error)
{
    if (!component_valid(name)) return storage_error(error, "Invalid container name");
    if (kind != PCV_LXC_STORAGE_ZFS && kind != PCV_LXC_STORAGE_BTRFS)
        return storage_error(error, "Invalid container storage backend");
    g_auto(StorageFd) base = base_open_full(TRUE, error);
    if (base < 0) return FALSE;
    g_autofree gchar *tombstone = deletion_name(name);
    if (entry_exists(base, name) || entry_exists(base, tombstone))
        return storage_error(error, "Container destination already exists or deletion is pending");
    gchar fs[33];
    return kind != PCV_LXC_STORAGE_BTRFS || btrfs_filesystem(base, fs, error);
}

static int container_open_for_record(const gchar *name, PcvLxcStorageKind kind, GError **error)
{
    if (!component_valid(name)) { storage_error(error, "Invalid container name"); return -1; }
    g_auto(StorageFd) base = base_open(error);
    if (base < 0) return -1;
    g_autofree gchar *tombstone = deletion_name(name);
    if (entry_exists(base, tombstone)) {
        storage_error(error, "Container deletion is pending; permissions unchanged");
        return -1;
    }
    int fd = openat(base, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { storage_error(error, "Cannot open new container directory: %s", g_strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != 0 || st.st_gid != 0 || (st.st_mode & 0002) ||
        entry_exists(fd, STORAGE_MARKER) || entry_exists(fd, STORAGE_JOURNAL) || entry_exists(fd, STORAGE_STAGE)) {
        close(fd);
        storage_error(error, "New container directory ownership or existing storage state prevents finalization");
        return -1;
    }
    if (kind == PCV_LXC_STORAGE_BTRFS && !ordinary_directory(fd, error)) { close(fd); return -1; }
    if ((st.st_mode & 0020) && (fchmod(fd, (st.st_mode & 07777) & ~0020) < 0 || !sync_directory(fd, error))) {
        close(fd);
        storage_error(error, "Cannot harden new container directory: %s", g_strerror(errno));
        return -1;
    }
    if (!trusted_fd(fd, TRUE, FALSE, error)) { close(fd); return -1; }
    return fd;
}

gboolean pcv_lxc_storage_record(const gchar *name, PcvLxcStorageKind kind, const gchar *dataset, GError **error)
{
    if (kind != PCV_LXC_STORAGE_BTRFS && kind != PCV_LXC_STORAGE_ZFS)
        return storage_error(error, "Invalid container storage backend");
    g_auto(StorageFd) container = container_open_for_record(name, kind, error);
    if (container < 0) return FALSE;
    g_autoptr(GKeyFile) key = marker_new(kind);
    if (kind == PCV_LXC_STORAGE_BTRFS) {
        if (!ordinary_directory(container, error)) return FALSE;
        StorageIdentity identity;
        if (!identity_read(container, "rootfs", &identity, error)) return FALSE;
        if (identity.readonly) return storage_error(error, "Container rootfs must be writable");
        gchar fs[33];
        if (!btrfs_filesystem(container, fs, error) || strcmp(fs, identity.fs))
            return storage_error(error, "Container and rootfs filesystem differ");
        identity_store(key, "storage", &identity);
    } else if (kind == PCV_LXC_STORAGE_ZFS) {
        g_autofree gchar *actual = NULL;
        if (!zfs_dataset(name, &actual, error)) return FALSE;
        if (dataset && strcmp(dataset, actual)) return storage_error(error, "ZFS dataset identity mismatch");
        g_key_file_set_string(key, "storage", "dataset", actual);
    } else return storage_error(error, "Invalid container storage backend");
    return metadata_write(container, STORAGE_MARKER, key, error);
}

static gboolean resolve_internal(const gchar *name, PcvLxcStorage *storage, gboolean deleting_allowed, GError **error)
{
    if (!component_valid(name)) return storage_error(error, "Invalid container name");
    g_auto(StorageFd) base = base_open(error);
    if (base < 0) return FALSE;
    g_autofree gchar *tombstone = deletion_name(name);
    g_autoptr(GKeyFile) deletion = NULL;
    if (entry_exists(base, tombstone)) {
        if (!deleting_allowed) return storage_error(error, "Container deletion is pending");
        deletion = deletion_read(base, name, error);
        if (!deletion) return FALSE;
        if (!entry_exists(base, name)) {
            storage->kind = PCV_LXC_STORAGE_BTRFS;
            storage->dataset = NULL;
            return TRUE;
        }
    }
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    if (entry_exists(container, STORAGE_JOURNAL) || entry_exists(container, STORAGE_STAGE))
        return storage_error(error, "Container restore recovery required");
    if (!entry_exists(container, STORAGE_MARKER) && !deletion) {
        g_autofree gchar *actual = NULL;
        if (!zfs_dataset(name, &actual, error)) return FALSE;
        storage->kind = PCV_LXC_STORAGE_ZFS;
        storage->dataset = g_steal_pointer(&actual);
        return TRUE;
    }
    g_autoptr(GKeyFile) key = deletion ? g_steal_pointer(&deletion) : metadata_read(container, STORAGE_MARKER, error);
    if (!key) return FALSE;
    g_autofree gchar *kind = g_key_file_get_string(key, "storage", "kind", NULL);
    g_autofree gchar *state = g_key_file_get_string(key, "storage", "state", NULL);
    gboolean deleting = !g_strcmp0(state, "deleting");
    if (g_strcmp0(state, "active") && !(deleting && deleting_allowed))
        return storage_error(error, "Container deletion pending or marker state invalid");
    if (!g_strcmp0(kind, "btrfs")) {
        gchar fs[33];
        g_autofree gchar *expected_fs = g_key_file_get_string(key, "storage", "fs_uuid", NULL);
        if (!btrfs_filesystem(container, fs, error) || g_strcmp0(fs, expected_fs))
            return storage_error(error, "Container filesystem identity mismatch");
        if (!deleting || entry_exists(container, "rootfs")) {
            StorageIdentity identity;
            if (!identity_read(container, "rootfs", &identity, error) || !identity_matches(key, "storage", &identity, error)) return FALSE;
            if (identity.readonly) return storage_error(error, "Container rootfs is unexpectedly read-only");
        }
        storage->kind = PCV_LXC_STORAGE_BTRFS;
        storage->dataset = NULL;
        return TRUE;
    }
    if (!g_strcmp0(kind, "zfs") && !deleting) {
        g_autofree gchar *actual = NULL;
        g_autofree gchar *expected = g_key_file_get_string(key, "storage", "dataset", NULL);
        if (!zfs_dataset(name, &actual, error)) return FALSE;
        if (g_strcmp0(actual, expected)) return storage_error(error, "ZFS dataset identity mismatch");
        storage->kind = PCV_LXC_STORAGE_ZFS;
        storage->dataset = g_steal_pointer(&actual);
        return TRUE;
    }
    return storage_error(error, "Invalid storage backend marker");
}

gboolean pcv_lxc_storage_resolve(const gchar *name, PcvLxcStorage *storage, GError **error)
{
    return resolve_internal(name, storage, FALSE, error);
}

gboolean pcv_lxc_storage_resolve_for_destroy(const gchar *name, PcvLxcStorage *storage, GError **error)
{
    return resolve_internal(name, storage, TRUE, error);
}

void pcv_lxc_storage_clear(PcvLxcStorage *storage)
{
    g_clear_pointer(&storage->dataset, g_free);
}

static gboolean tree_scan(int fd, dev_t device, guint depth, GError **error)
{
    if (depth > 512) return storage_error(error, "Rootfs directory nesting exceeds safe traversal limit");
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return storage_error(error, "Cannot scan rootfs: %s", g_strerror(errno));
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return storage_error(error, "Cannot scan rootfs: %s", g_strerror(errno)); }
    gboolean ok = TRUE;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            ok = storage_error(error, "Rootfs changed during traversal: %s", g_strerror(errno));
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            g_auto(StorageFd) child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0 || fstat(child, &st) < 0) {
                ok = storage_error(error, "Cannot inspect rootfs child %s", entry->d_name);
                break;
            }
            if (st.st_dev != device || st.st_ino == 256) {
                ok = storage_error(error, "Nested subvolume or filesystem below rootfs: %s", entry->d_name);
                break;
            }
            if (!tree_scan(child, device, depth + 1, error)) { ok = FALSE; break; }
        }
        errno = 0;
    }
    if (ok && errno) ok = storage_error(error, "Rootfs traversal failed: %s", g_strerror(errno));
    closedir(dir);
    return ok;
}

static gboolean root_validate(int parent, const gchar *entry, const gchar *path, GError **error)
{
    if (!mount_check(path, NULL, error)) return FALSE;
    g_auto(StorageFd) fd = directory_open(parent, entry, error);
    struct stat st;
    if (fd < 0) return FALSE;
    if (fstat(fd, &st) < 0) return storage_error(error, "Cannot stat rootfs: %s", g_strerror(errno));
    return tree_scan(fd, st.st_dev, 0, error);
}

static gboolean require_btrfs(const gchar *name, gboolean deleting, GError **error)
{
    PcvLxcStorage storage = {0};
    if (!resolve_internal(name, &storage, deleting, error)) return FALSE;
    gboolean ok = storage.kind == PCV_LXC_STORAGE_BTRFS;
    pcv_lxc_storage_clear(&storage);
    return ok || storage_error(error, "Operation requires managed Btrfs storage");
}

gboolean pcv_lxc_storage_btrfs_validate_copy(const gchar *name, GError **error)
{
    if (!require_btrfs(name, FALSE, error)) return FALSE;
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    g_autofree gchar *path = g_build_filename(pcv_config_get_container_path(), name, "rootfs", NULL);
    return root_validate(container, "rootfs", path, error);
}

static gboolean entries_allowed_with_temps(int fd, const gchar * const *allowed,
                                          const gchar * const *temp_targets, GError **error)
{
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return storage_error(error, "Cannot inspect managed directory");
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return storage_error(error, "Cannot inspect managed directory"); }
    gboolean ok = TRUE;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!g_strv_contains(allowed, entry->d_name)) {
            g_autofree gchar *target = metadata_temp_target(entry->d_name);
            if (!target || !temp_targets || !g_strv_contains(temp_targets, target) ||
                !metadata_temp_validate(fd, entry->d_name, error)) {
                ok = storage_error(error, "Unrecognized managed directory entry %s; data retained", entry->d_name);
                break;
            }
        }
        errno = 0;
    }
    if (ok && errno) ok = storage_error(error, "Managed directory traversal failed");
    closedir(dir);
    return ok;
}

static gboolean entries_allowed(int fd, const gchar * const *allowed, GError **error)
{
    return entries_allowed_with_temps(fd, allowed, NULL, error);
}

static gboolean subvolume_snapshot(int source, int destination, const gchar *name, gboolean readonly, GError **error)
{
    if (entry_exists(destination, name)) return storage_error(error, "Snapshot destination already exists");
    struct btrfs_ioctl_vol_args_v2 args = {0};
    args.fd = source;
    args.flags = readonly ? BTRFS_SUBVOL_RDONLY : 0;
    g_strlcpy(args.name, name, sizeof(args.name));
    if (ioctl(destination, BTRFS_IOC_SNAP_CREATE_V2, &args) < 0)
        return storage_error(error, "Btrfs snapshot creation failed: %s", g_strerror(errno));
    return sync_directory(destination, error);
}

static gboolean btrfs_commit(int fd, GError **error)
{
    return ioctl(fd, BTRFS_IOC_SYNC, NULL) == 0 ||
        storage_error(error, "Btrfs transaction commit failed; recovery metadata retained: %s", g_strerror(errno));
}

static gboolean subvolume_delete(int parent, const gchar *name, GError **error)
{
    struct btrfs_ioctl_vol_args_v2 args = {0};
    g_strlcpy(args.name, name, sizeof(args.name));
    if (ioctl(parent, BTRFS_IOC_SNAP_DESTROY_V2, &args) < 0)
        return storage_error(error, "Btrfs subvolume deletion failed: %s", g_strerror(errno));
    return sync_directory(parent, error);
}

static gboolean unlink_sync(int parent, const gchar *name, int flags, GError **error)
{
    if (unlinkat(parent, name, flags) < 0)
        return storage_error(error, "Cannot remove managed entry %s: %s", name, g_strerror(errno));
    return sync_directory(parent, error);
}

static GKeyFile *snapshot_validate(int container, const gchar *name, const gchar *snapshot,
                                 gboolean deleting_allowed, GError **error)
{
    if (!component_valid(snapshot)) { storage_error(error, "Invalid snapshot name"); return NULL; }
    g_autofree gchar *snapshots_path = g_build_filename(pcv_config_get_container_path(), name, STORAGE_SNAPS, NULL);
    if (!mount_check(snapshots_path, NULL, error)) return NULL;
    g_auto(StorageFd) snapshots = ordinary_directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) return NULL;
    g_autofree gchar *marker = g_strdup_printf(".pcv-%s.storage", snapshot);
    g_autoptr(GKeyFile) key = metadata_read(snapshots, marker, error);
    if (!key) return NULL;
    g_autofree gchar *kind = g_key_file_get_string(key, "storage", "kind", NULL);
    g_autofree gchar *owner = g_key_file_get_string(key, "storage", "container", NULL);
    g_autofree gchar *snap = g_key_file_get_string(key, "storage", "snapshot", NULL);
    g_autofree gchar *state = g_key_file_get_string(key, "storage", "state", NULL);
    gboolean deleting = !g_strcmp0(state, "deleting");
    if (g_strcmp0(kind, "btrfs") || g_strcmp0(owner, name) || g_strcmp0(snap, snapshot) ||
        (g_strcmp0(state, "active") && !(deleting_allowed && deleting))) {
        storage_error(error, "Invalid managed snapshot metadata");
        return NULL;
    }
    gchar fs[33];
    g_autofree gchar *expected_fs = g_key_file_get_string(key, "storage", "fs_uuid", NULL);
    if (!btrfs_filesystem(container, fs, error) || g_strcmp0(fs, expected_fs)) {
        storage_error(error, "Snapshot filesystem identity mismatch");
        return NULL;
    }
    if (deleting && !entry_exists(snapshots, snapshot)) return g_steal_pointer(&key);
    g_auto(StorageFd) dir = ordinary_directory_open(snapshots, snapshot, error);
    if (dir < 0) return NULL;
    const gchar *allowed[] = {"rootfs", NULL};
    if (!entries_allowed(dir, allowed, error)) return NULL;
    if (!deleting || entry_exists(dir, "rootfs")) {
        StorageIdentity identity;
        if (!identity_read(dir, "rootfs", &identity, error) || !identity_matches(key, "storage", &identity, error)) return NULL;
        if (!identity.readonly) { storage_error(error, "Managed snapshot must remain read-only"); return NULL; }
        g_autofree gchar *path = g_build_filename(pcv_config_get_container_path(), name, STORAGE_SNAPS, snapshot, "rootfs", NULL);
        if (!root_validate(dir, "rootfs", path, error)) return NULL;
    }
    return g_steal_pointer(&key);
}

gboolean pcv_lxc_storage_btrfs_snapshot_create(const gchar *name, const gchar *snapshot, GError **error)
{
    if (!component_valid(snapshot)) return storage_error(error, "Invalid snapshot name");
    if (!pcv_lxc_storage_btrfs_validate_copy(name, error)) return FALSE;
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    if (mkdirat(container, STORAGE_SNAPS, 0700) < 0 && errno != EEXIST)
        return storage_error(error, "Cannot create snapshot directory: %s", g_strerror(errno));
    if (!sync_directory(container, error)) return FALSE;
    g_auto(StorageFd) snapshots = ordinary_directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) return FALSE;
    g_autofree gchar *marker = g_strdup_printf(".pcv-%s.storage", snapshot);
    if (entry_exists(snapshots, marker)) return storage_error(error, "Snapshot metadata already exists");
    g_autofree gchar *snapshots_path = g_build_filename(pcv_config_get_container_path(), name, STORAGE_SNAPS, NULL);
    if (!mount_check(snapshots_path, NULL, error)) return FALSE;
    if (mkdirat(snapshots, snapshot, 0700) < 0)
        return storage_error(error, "Snapshot already exists or cannot be created: %s", g_strerror(errno));
    if (!sync_directory(snapshots, error)) return FALSE;
    g_auto(StorageFd) dir = ordinary_directory_open(snapshots, snapshot, error);
    g_auto(StorageFd) root = directory_open(container, "rootfs", error);
    if (dir < 0 || root < 0) return FALSE;
    if (!subvolume_snapshot(root, dir, "rootfs", TRUE, error)) {
        if (!entry_exists(dir, "rootfs")) unlinkat(snapshots, snapshot, AT_REMOVEDIR);
        return FALSE;
    }
    StorageIdentity identity;
    if (!identity_read(dir, "rootfs", &identity, error)) return FALSE;
    g_autoptr(GKeyFile) key = marker_new(PCV_LXC_STORAGE_BTRFS);
    identity_store(key, "storage", &identity);
    g_key_file_set_string(key, "storage", "container", name);
    g_key_file_set_string(key, "storage", "snapshot", snapshot);
    return metadata_write(snapshots, marker, key, error);
}

static GPtrArray *snapshot_names(int container, GError **error)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    if (!entry_exists(container, STORAGE_SNAPS)) return names;
    g_auto(StorageFd) snapshots = ordinary_directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) { g_ptr_array_unref(names); return NULL; }
    if (!metadata_temps(snapshots, NULL, FALSE, error)) { g_ptr_array_unref(names); return NULL; }
    int scan = openat(snapshots, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir = scan < 0 ? NULL : fdopendir(scan);
    if (!dir) {
        if (scan >= 0) close(scan);
        storage_error(error, "Cannot enumerate snapshots");
        g_ptr_array_unref(names);
        return NULL;
    }
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (g_str_has_prefix(entry->d_name, ".pcv-write-")) continue;
        g_autofree gchar *candidate = NULL;
        if (g_str_has_prefix(entry->d_name, ".pcv-") && g_str_has_suffix(entry->d_name, ".storage"))
            candidate = g_strndup(entry->d_name + 5, strlen(entry->d_name) - 5 - 8);
        else candidate = g_strdup(entry->d_name);
        if (!component_valid(candidate)) {
            storage_error(error, "Unrecognized snapshot entry; data retained");
            closedir(dir);
            g_ptr_array_unref(names);
            return NULL;
        }
        gboolean present = FALSE;
        for (guint i = 0; i < names->len; i++)
            if (!strcmp(candidate, g_ptr_array_index(names, i))) { present = TRUE; break; }
        if (!present) g_ptr_array_add(names, g_steal_pointer(&candidate));
        errno = 0;
    }
    int saved = errno;
    closedir(dir);
    if (saved) {
        storage_error(error, "Snapshot enumeration failed");
        g_ptr_array_unref(names);
        return NULL;
    }
    return names;
}

GPtrArray *pcv_lxc_storage_btrfs_snapshot_list(const gchar *name, GError **error)
{
    if (!require_btrfs(name, FALSE, error)) return NULL;
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return NULL;
    g_autoptr(GPtrArray) names = snapshot_names(container, error);
    if (!names) return NULL;
    for (guint i = 0; i < names->len; i++) {
        g_autoptr(GKeyFile) key = snapshot_validate(container, name, g_ptr_array_index(names, i), TRUE, error);
        if (!key) return NULL;
    }
    return g_steal_pointer(&names);
}

static gboolean snapshot_delete_internal(int container, const gchar *name, const gchar *snapshot, GError **error)
{
    g_autoptr(GKeyFile) key = snapshot_validate(container, name, snapshot, TRUE, error);
    if (!key) return FALSE;
    g_auto(StorageFd) snapshots = directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) return FALSE;
    g_autofree gchar *marker = g_strdup_printf(".pcv-%s.storage", snapshot);
    g_key_file_set_string(key, "storage", "state", "deleting");
    if (!metadata_write(snapshots, marker, key, error)) return FALSE;
    gboolean has_directory = entry_exists(snapshots, snapshot);
    if (has_directory) {
        g_auto(StorageFd) dir = directory_open(snapshots, snapshot, error);
        if (dir < 0) return FALSE;
        if (entry_exists(dir, "rootfs") && !subvolume_delete(dir, "rootfs", error)) return FALSE;
    }
    if (!btrfs_commit(snapshots, error)) return FALSE;
    if (has_directory && !unlink_sync(snapshots, snapshot, AT_REMOVEDIR, error)) return FALSE;
    return unlink_sync(snapshots, marker, 0, error);
}

gboolean pcv_lxc_storage_btrfs_snapshot_delete(const gchar *name, const gchar *snapshot, GError **error)
{
    if (!require_btrfs(name, FALSE, error)) return FALSE;
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    g_autofree gchar *path = g_build_filename(pcv_config_get_container_path(), name, STORAGE_SNAPS, NULL);
    if (!mount_check(path, NULL, error)) return FALSE;
    return snapshot_delete_internal(container, name, snapshot, error);
}

static gboolean restore_candidate(int container, const gchar *name, GKeyFile *journal, GError **error)
{
    StorageIdentity root;
    if (!identity_read(container, "rootfs", &root, error) || !identity_matches(journal, "old", &root, error)) return FALSE;
    g_autofree gchar *snapshot = g_key_file_get_string(journal, "storage", "snapshot", NULL);
    g_autoptr(GKeyFile) snapshot_key = snapshot_validate(container, name, snapshot, FALSE, error);
    if (!snapshot_key) return FALSE;
    g_auto(StorageFd) snapshots = directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) return FALSE;
    g_auto(StorageFd) snapdir = directory_open(snapshots, snapshot, error);
    if (snapdir < 0) return FALSE;
    StorageIdentity source;
    if (!identity_read(snapdir, "rootfs", &source, error) || !identity_matches(journal, "source", &source, error)) return FALSE;
    if (!entry_exists(container, STORAGE_STAGE)) {
        g_auto(StorageFd) source_fd = directory_open(snapdir, "rootfs", error);
        if (source_fd < 0 || !subvolume_snapshot(source_fd, container, STORAGE_STAGE, FALSE, error)) return FALSE;
    }
    StorageIdentity candidate;
    if (!identity_read(container, STORAGE_STAGE, &candidate, error)) return FALSE;
    if (strcmp(candidate.parent_uuid, source.uuid) || strcmp(candidate.fs, root.fs) || candidate.readonly)
        return storage_error(error, "Unrecognized restore candidate; data retained");
    identity_store(journal, "new", &candidate);
    g_key_file_set_string(journal, "storage", "phase", "ready");
    return metadata_write(container, STORAGE_JOURNAL, journal, error);
}

gboolean pcv_lxc_storage_recover(const gchar *name, GError **error)
{
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    if (!entry_exists(container, STORAGE_JOURNAL)) {
        if (entry_exists(container, STORAGE_STAGE)) return storage_error(error, "Unjournaled restore candidate; data retained");
        if (entry_exists(container, STORAGE_MARKER)) {
            g_autoptr(GKeyFile) marker = metadata_read(container, STORAGE_MARKER, error);
            if (!marker || !metadata_temps(container, STORAGE_MARKER, TRUE, error) ||
                !metadata_temps(container, STORAGE_JOURNAL, TRUE, error)) return FALSE;
        }
        return TRUE;
    }
    g_autoptr(GKeyFile) journal = metadata_read(container, STORAGE_JOURNAL, error);
    g_autoptr(GKeyFile) marker = metadata_read(container, STORAGE_MARKER, error);
    if (!journal || !marker) return FALSE;
    g_autofree gchar *kind = g_key_file_get_string(journal, "storage", "kind", NULL);
    g_autofree gchar *state = g_key_file_get_string(journal, "storage", "state", NULL);
    g_autofree gchar *marker_kind = g_key_file_get_string(marker, "storage", "kind", NULL);
    g_autofree gchar *marker_state = g_key_file_get_string(marker, "storage", "state", NULL);
    if (g_strcmp0(kind, "btrfs") || g_strcmp0(state, "restore") ||
        g_strcmp0(marker_kind, "btrfs") || g_strcmp0(marker_state, "active"))
        return storage_error(error, "Invalid restore journal or container marker");
    if (!metadata_temps(container, STORAGE_MARKER, FALSE, error) ||
        !metadata_temps(container, STORAGE_JOURNAL, FALSE, error)) return FALSE;
    g_autofree gchar *phase = g_key_file_get_string(journal, "storage", "phase", NULL);
    if (!g_strcmp0(phase, "preparing")) {
        StorageIdentity old;
        if (!identity_read(container, "rootfs", &old, error) || !identity_matches(marker, "storage", &old, error)) return FALSE;
        if (!restore_candidate(container, name, journal, error)) return FALSE;
    } else if (g_strcmp0(phase, "ready")) return storage_error(error, "Invalid restore journal phase");

    StorageIdentity root, stage;
    if (!identity_read(container, "rootfs", &root, error)) return FALSE;
    gboolean has_stage = entry_exists(container, STORAGE_STAGE);
    if (has_stage && !identity_read(container, STORAGE_STAGE, &stage, error)) return FALSE;
    gboolean root_old = identity_matches(journal, "old", &root, NULL);
    gboolean root_new = identity_matches(journal, "new", &root, NULL);
    gboolean stage_old = has_stage && identity_matches(journal, "old", &stage, NULL);
    gboolean stage_new = has_stage && identity_matches(journal, "new", &stage, NULL);
    if (root_old == root_new || (has_stage && stage_old == stage_new) ||
        (!((root_old && stage_new) || (root_new && (stage_old || !has_stage)))) || root.readonly ||
        (has_stage && stage.readonly))
        return storage_error(error, "Ambiguous restore identity pair; data retained");
    gboolean marker_ok = identity_matches(marker, "storage", &root, NULL) ||
        (root_new && has_stage && identity_matches(marker, "storage", &stage, NULL));
    if (!marker_ok && root_new && !has_stage) {
        g_autofree gchar *marker_fs = g_key_file_get_string(marker, "storage", "fs_uuid", NULL);
        g_autofree gchar *marker_uuid = g_key_file_get_string(marker, "storage", "subvol_uuid", NULL);
        g_autofree gchar *old_fs = g_key_file_get_string(journal, "old", "fs_uuid", NULL);
        g_autofree gchar *old_uuid = g_key_file_get_string(journal, "old", "subvol_uuid", NULL);
        marker_ok = marker_fs && marker_uuid && !g_strcmp0(marker_fs, old_fs) && !g_strcmp0(marker_uuid, old_uuid) &&
            g_key_file_get_uint64(marker, "storage", "subvol_id", NULL) == g_key_file_get_uint64(journal, "old", "subvol_id", NULL);
    }
    if (!marker_ok) return storage_error(error, "Restore marker identity mismatch; data retained");
    g_autofree gchar *root_path = g_build_filename(pcv_config_get_container_path(), name, "rootfs", NULL);
    g_autofree gchar *stage_path = g_build_filename(pcv_config_get_container_path(), name, STORAGE_STAGE, NULL);
    if (!root_validate(container, "rootfs", root_path, error) ||
        (has_stage && !root_validate(container, STORAGE_STAGE, stage_path, error))) return FALSE;
    if (!metadata_temps(container, STORAGE_JOURNAL, TRUE, error)) return FALSE;
    if (root_old) {
        if (renameat2(container, "rootfs", container, STORAGE_STAGE, RENAME_EXCHANGE) < 0)
            return storage_error(error, "Atomic rootfs exchange failed: %s", g_strerror(errno));
        if (!sync_directory(container, error) || !btrfs_commit(container, error)) return FALSE;
        root = stage;
    }
    identity_store(marker, "storage", &root);
    if (!metadata_write(container, STORAGE_MARKER, marker, error)) return FALSE;
    if (has_stage && !subvolume_delete(container, STORAGE_STAGE, error)) return FALSE;
    if (!btrfs_commit(container, error)) return FALSE;
    return unlink_sync(container, STORAGE_JOURNAL, 0, error);
}

gboolean pcv_lxc_storage_btrfs_snapshot_rollback(const gchar *name, const gchar *snapshot, GError **error)
{
    if (!pcv_lxc_storage_btrfs_validate_copy(name, error)) return FALSE;
    g_auto(StorageFd) container = container_open(name, error);
    if (container < 0) return FALSE;
    g_autoptr(GKeyFile) snap = snapshot_validate(container, name, snapshot, FALSE, error);
    if (!snap) return FALSE;
    StorageIdentity old, source;
    if (!identity_read(container, "rootfs", &old, error)) return FALSE;
    g_auto(StorageFd) snapshots = directory_open(container, STORAGE_SNAPS, error);
    if (snapshots < 0) return FALSE;
    g_auto(StorageFd) snapdir = directory_open(snapshots, snapshot, error);
    if (snapdir < 0 || !identity_read(snapdir, "rootfs", &source, error)) return FALSE;
    g_autoptr(GKeyFile) journal = marker_new(PCV_LXC_STORAGE_BTRFS);
    g_key_file_set_string(journal, "storage", "state", "restore");
    g_key_file_set_string(journal, "storage", "phase", "preparing");
    g_key_file_set_string(journal, "storage", "snapshot", snapshot);
    identity_store(journal, "old", &old);
    identity_store(journal, "source", &source);
    if (!metadata_write(container, STORAGE_JOURNAL, journal, error)) return FALSE;
    return pcv_lxc_storage_recover(name, error);
}

static gboolean sidecars_validate(int container, GError **error)
{
    const gchar *allowed[] = {"rootfs", STORAGE_MARKER, STORAGE_SNAPS,
        "config", "purecvisor.owner", "purecvisor.meta", NULL};
    const gchar *temp_targets[] = {STORAGE_MARKER, STORAGE_JOURNAL, NULL};
    if (!entries_allowed_with_temps(container, allowed, temp_targets, error)) return FALSE;
    const gchar *sidecars[] = {"config", "purecvisor.owner", "purecvisor.meta", NULL};
    for (guint i = 0; sidecars[i]; i++) {
        if (!entry_exists(container, sidecars[i])) continue;
        g_auto(StorageFd) fd = openat(container, sidecars[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0 || !trusted_fd(fd, FALSE, FALSE, error))
            return storage_error(error, "Untrusted container sidecar %s; data retained", sidecars[i]);
    }
    return TRUE;
}

gboolean pcv_lxc_storage_btrfs_destroy(const gchar *name, GError **error)
{
    if (!require_btrfs(name, TRUE, error)) return FALSE;
    g_auto(StorageFd) base = base_open(error);
    if (base < 0) return FALSE;
    g_autofree gchar *tombstone = deletion_name(name);
    if (!metadata_temps(base, tombstone, FALSE, error)) return FALSE;
    if (!entry_exists(base, name)) {
        g_autoptr(GKeyFile) deletion = deletion_read(base, name, error);
        return deletion && metadata_temps(base, tombstone, TRUE, error) && unlink_sync(base, tombstone, 0, error);
    }
    g_auto(StorageFd) container = directory_open(base, name, error);
    if (container < 0) return FALSE;
    if (!ordinary_directory(container, error)) return FALSE;
    g_autofree gchar *path = container_path(name);
    if (!mount_check(path, NULL, error) || !sidecars_validate(container, error)) return FALSE;
    g_autoptr(GKeyFile) marker = entry_exists(base, tombstone) ? deletion_read(base, name, error) :
        metadata_read(container, STORAGE_MARKER, error);
    if (!marker) return FALSE;
    if (entry_exists(container, "rootfs")) {
        g_autofree gchar *root_path = g_build_filename(path, "rootfs", NULL);
        if (!root_validate(container, "rootfs", root_path, error)) return FALSE;
    }
    g_autoptr(GPtrArray) snapshots = snapshot_names(container, error);
    if (!snapshots) return FALSE;
    for (guint i = 0; i < snapshots->len; i++) {
        g_autoptr(GKeyFile) snap = snapshot_validate(container, name, g_ptr_array_index(snapshots, i), TRUE, error);
        if (!snap) return FALSE;
    }
    g_key_file_set_string(marker, "storage", "state", "deleting");
    g_key_file_set_string(marker, "storage", "container", name);
    if (!metadata_write(container, STORAGE_MARKER, marker, error)) return FALSE;
    if (!metadata_temps(container, STORAGE_JOURNAL, TRUE, error)) return FALSE;
    for (guint i = 0; i < snapshots->len; i++)
        if (!snapshot_delete_internal(container, name, g_ptr_array_index(snapshots, i), error)) return FALSE;
    if (entry_exists(container, STORAGE_SNAPS)) {
        g_auto(StorageFd) snapshot_dir = ordinary_directory_open(container, STORAGE_SNAPS, error);
        if (snapshot_dir < 0 || !metadata_temps(snapshot_dir, NULL, TRUE, error) ||
            !unlink_sync(container, STORAGE_SNAPS, AT_REMOVEDIR, error)) return FALSE;
    }
    if (entry_exists(container, "rootfs") && !subvolume_delete(container, "rootfs", error)) return FALSE;
    if (!btrfs_commit(container, error)) return FALSE;
    if (!metadata_write(base, tombstone, marker, error)) return FALSE;
    const gchar *sidecars[] = {"config", "purecvisor.owner", "purecvisor.meta", STORAGE_MARKER, NULL};
    for (guint i = 0; sidecars[i]; i++)
        if (entry_exists(container, sidecars[i]) && !unlink_sync(container, sidecars[i], 0, error)) return FALSE;
    if (!unlink_sync(base, name, AT_REMOVEDIR, error)) return FALSE;
    return unlink_sync(base, tombstone, 0, error);
}


gboolean pcv_lxc_storage_zfs_mount_allowed(const gchar *name, const gchar *dataset, gboolean rootfs_mount)
{
    if (!component_valid(name) || !dataset || !*dataset) return FALSE;
    g_autoptr(GError) error = NULL;
    g_auto(StorageFd) base = base_open(&error);
    if (base < 0) return FALSE;
    g_autofree gchar *tombstone = deletion_name(name);
    if (entry_exists(base, tombstone)) return FALSE;
    g_auto(StorageFd) container = openat(base, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (container < 0) return errno == ENOENT;
    if (!trusted_fd(container, TRUE, FALSE, &error) ||
        entry_exists(container, STORAGE_JOURNAL) || entry_exists(container, STORAGE_STAGE)) return FALSE;
    if (entry_exists(container, STORAGE_MARKER)) {
        g_autoptr(GKeyFile) key = metadata_read(container, STORAGE_MARKER, &error);
        if (!key) return FALSE;
        g_autofree gchar *kind = g_key_file_get_string(key, "storage", "kind", NULL);
        g_autofree gchar *state = g_key_file_get_string(key, "storage", "state", NULL);
        g_autofree gchar *recorded = g_key_file_get_string(key, "storage", "dataset", NULL);
        if (g_strcmp0(kind, "zfs") || g_strcmp0(state, "active") || g_strcmp0(recorded, dataset))
            return FALSE;
    }
    g_auto(StorageFd) destination = rootfs_mount
        ? openat(container, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
        : dup(container);
    if (destination < 0) return errno == ENOENT;
    const gchar *empty[] = {NULL};
    return trusted_fd(destination, TRUE, FALSE, &error) && ordinary_directory(destination, &error) &&
           entries_allowed(destination, empty, &error);
}
