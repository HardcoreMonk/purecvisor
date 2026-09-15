#define _GNU_SOURCE
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <sys/xattr.h>
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
#include <stdlib.h>

static const char *base_path;
static const char *backend;
static const char *mounts = "";
static gboolean is_btrfs = TRUE;
static int fail_delete;
static int fail_exchange;
static int fail_sync;
static const char *fail_unlink;
static unsigned next_id = 400;
static guint64 root_mount_id = 31;

const gchar *pcv_config_get_container_path(void) { return base_path; }
const gchar *pcv_config_get_container_pool(void) { return "changed/containers"; }
const gchar *pcv_config_get_string(const gchar *group, const gchar *key, const gchar *def)
{ (void)group; (void)key; return backend ? backend : def; }
gchar *pcv_config_dup_raw_value(const gchar *group, const gchar *key)
{ (void)group; (void)key; return g_strdup(backend); }
gboolean pcv_spawn_sync(const gchar * const *argv, gchar **out, gchar **err, GError **error)
{
    (void)argv; (void)err; (void)error;
    if (out) *out = g_strdup("");
    return TRUE;
}

static int mock_fstat(int fd, struct stat *st)
{
    int result = fstat(fd, st);
    if (result == 0) {
        int owner = 0;
        int group = 0;
        unsigned id = 0;
        fgetxattr(fd, "user.pcv_test_owner", &owner, sizeof(owner));
        fgetxattr(fd, "user.pcv_test_group", &group, sizeof(group));
        st->st_uid = owner;
        st->st_gid = group;
        if (fgetxattr(fd, "user.pcv_test_id", &id, sizeof(id)) == sizeof(id))
            st->st_ino = 256;
    }
    return result;
}

static int mock_fstatfs(int fd, struct statfs *fs)
{
    int result = fstatfs(fd, fs);
    if (!result) fs->f_type = is_btrfs ? BTRFS_SUPER_MAGIC : EXT4_SUPER_MAGIC;
    return result;
}

int mock_statx(int fd, const char *path, int flags, unsigned mask, struct statx *st)
{
    int result = statx(fd, path, flags, mask, st);
    if (!result) {
        st->stx_mnt_id = root_mount_id;
        st->stx_mask |= STATX_MNT_ID;
    }
    return result;
}

static FILE *mock_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/self/mountinfo") == 0)
        return fmemopen((void *)mounts, strlen(mounts), "r");
    return fopen(path, mode);
}

static void subvol_fd(int fd, unsigned id, gboolean readonly)
{
    g_assert_cmpint(fsetxattr(fd, "user.pcv_test_id", &id, sizeof(id), 0), ==, 0);
    g_assert_cmpint(fsetxattr(fd, "user.pcv_test_ro", &readonly, sizeof(readonly), 0), ==, 0);
}

static void remove_contents(int fd)
{
    DIR *dir = fdopendir(dup(fd));
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        int child = openat(fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (child >= 0) {
            remove_contents(child);
            close(child);
            g_assert_cmpint(unlinkat(fd, ent->d_name, AT_REMOVEDIR), ==, 0);
        } else g_assert_cmpint(unlinkat(fd, ent->d_name, 0), ==, 0);
    }
    closedir(dir);
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == BTRFS_IOC_SYNC) {
        if (fail_sync > 0 && --fail_sync == 0) { errno = EIO; return -1; }
        return 0;
    }
    if (request == BTRFS_IOC_FS_INFO) {
        struct btrfs_ioctl_fs_info_args *info = arg;
        memset(info, 0, sizeof(*info));
        memset(info->fsid, 0x11, sizeof(info->fsid));
        return 0;
    }
    if (request == BTRFS_IOC_GET_SUBVOL_INFO) {
        struct btrfs_ioctl_get_subvol_info_args *info = arg;
        unsigned id = 0;
        gboolean readonly = FALSE;
        if (fgetxattr(fd, "user.pcv_test_id", &id, sizeof(id)) != sizeof(id)) {
            errno = EINVAL;
            return -1;
        }
        fgetxattr(fd, "user.pcv_test_ro", &readonly, sizeof(readonly));
        memset(info, 0, sizeof(*info));
        info->treeid = id;
        memcpy(info->uuid, &id, sizeof(id));
        unsigned parent = 0;
        fgetxattr(fd, "user.pcv_test_parent", &parent, sizeof(parent));
        memcpy(info->parent_uuid, &parent, sizeof(parent));
        info->flags = readonly ? BTRFS_ROOT_SUBVOL_RDONLY : 0;
        return 0;
    }
    if (request == BTRFS_IOC_SNAP_CREATE_V2) {
        struct btrfs_ioctl_vol_args_v2 *args = arg;
        if (mkdirat(fd, args->name, 0755) < 0) return -1;
        int child = openat(fd, args->name, O_RDONLY | O_DIRECTORY);
        subvol_fd(child, next_id++, (args->flags & BTRFS_SUBVOL_RDONLY) != 0);
        unsigned parent = 0;
        g_assert_cmpint(fgetxattr(args->fd, "user.pcv_test_id", &parent, sizeof(parent)), ==, sizeof(parent));
        g_assert_cmpint(fsetxattr(child, "user.pcv_test_parent", &parent, sizeof(parent), 0), ==, 0);
        close(child);
        return 0;
    }
    if (request == BTRFS_IOC_SNAP_DESTROY_V2) {
        struct btrfs_ioctl_vol_args_v2 *args = arg;
        if (fail_delete > 0 && --fail_delete == 0) { errno = EIO; return -1; }
        int child = openat(fd, args->name, O_RDONLY | O_DIRECTORY);
        if (child < 0) return -1;
        remove_contents(child);
        close(child);
        return unlinkat(fd, args->name, AT_REMOVEDIR);
    }
    errno = EINVAL;
    return -1;
}

static int mock_renameat2(int oldfd, const char *oldname, int newfd, const char *newname, unsigned flags)
{
    if (fail_exchange) { errno = EIO; return -1; }
    if (flags == RENAME_EXCHANGE) {
        if (renameat(oldfd, oldname, oldfd, ".test-exchange") < 0) return -1;
        if (renameat(newfd, newname, oldfd, oldname) < 0) return -1;
        return renameat(oldfd, ".test-exchange", newfd, newname);
    }
    return renameat2(oldfd, oldname, newfd, newname, flags);
}

static int mock_unlinkat(int fd, const char *name, int flags)
{
    if (fail_unlink && !strcmp(fail_unlink, name)) {
        fail_unlink = NULL;
        errno = EIO;
        return -1;
    }
    return unlinkat(fd, name, flags);
}

#define fstat mock_fstat
#define fstatfs mock_fstatfs
#define statx(...) mock_statx(__VA_ARGS__)
#define ioctl mock_ioctl
#define fopen mock_fopen
#define renameat2 mock_renameat2
#define unlinkat mock_unlinkat
#include "modules/lxc/lxc_storage.c"
#undef fstat
#undef fstatfs
#undef statx
#undef ioctl
#undef fopen
#undef renameat2
#undef unlinkat

static gchar *fixture(void)
{
    GError *error = NULL;
    gchar *path = g_dir_make_tmp("pcv-lxc-storage-XXXXXX", &error);
    g_assert_no_error(error);
    base_path = path;
    backend = NULL;
    mounts = "";
    fail_delete = fail_exchange = fail_sync = 0;
    fail_unlink = NULL;
    is_btrfs = TRUE;
    root_mount_id = 31;
    return path;
}

static void fixture_free(gchar *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    remove_contents(fd);
    close(fd);
    g_assert_cmpint(rmdir(path), ==, 0);
    g_free(path);
}

static void create_container(const char *name)
{
    gchar *dir = g_build_filename(base_path, name, NULL);
    gchar *root = g_build_filename(dir, "rootfs", NULL);
    g_assert_cmpint(mkdir(dir, 0755), ==, 0);
    g_assert_cmpint(mkdir(root, 0755), ==, 0);
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_record(name, PCV_LXC_STORAGE_BTRFS, NULL, &error));
    g_assert_no_error(error);
    g_free(root);
    g_free(dir);
}

static void test_default_validation(void)
{
    gchar *path = fixture();
    PcvLxcStorageKind kind;
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_default(&kind, &error));
    g_assert_cmpint(kind, ==, PCV_LXC_STORAGE_ZFS);
    backend = "btrfs";
    g_assert_true(pcv_lxc_storage_default(&kind, &error));
    g_assert_cmpint(kind, ==, PCV_LXC_STORAGE_BTRFS);
    backend = "typo";
    g_assert_false(pcv_lxc_storage_default(&kind, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    backend = "";
    g_assert_false(pcv_lxc_storage_default(&kind, &error));
    g_clear_error(&error);
    fixture_free(path);
}

static void test_create_path_guard(void)
{
    gchar *path = fixture();
    GError *error = NULL;
    g_assert_false(pcv_lxc_storage_prepare_create("../escape", PCV_LXC_STORAGE_BTRFS, &error));
    g_clear_error(&error);
    gchar *link = g_build_filename(path, "escape", NULL);
    g_assert_cmpint(symlink("/", link), ==, 0);
    g_assert_false(pcv_lxc_storage_prepare_create("escape", PCV_LXC_STORAGE_BTRFS, &error));
    g_clear_error(&error);
    is_btrfs = FALSE;
    g_assert_false(pcv_lxc_storage_prepare_create("fresh", PCV_LXC_STORAGE_BTRFS, &error));
    g_clear_error(&error);
    is_btrfs = TRUE;
    g_assert_true(pcv_lxc_storage_prepare_create("fresh", PCV_LXC_STORAGE_BTRFS, &error));
    g_assert_no_error(error);
    g_free(link);
    fixture_free(path);
}

static void test_record_and_tamper(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    backend = "zfs";
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_cmpint(storage.kind, ==, PCV_LXC_STORAGE_BTRFS);
    pcv_lxc_storage_clear(&storage);
    gchar *root = g_build_filename(path, "alpha", "rootfs", NULL);
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    g_assert_true(g_file_test(root, G_FILE_TEST_IS_DIR));
    g_free(root);
    fixture_free(path);
}

static void test_snapshot_and_guards(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    g_assert_no_error(error);
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    g_clear_error(&error);
    GPtrArray *list = pcv_lxc_storage_btrfs_snapshot_list("alpha", &error);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(list, 0), ==, "first");
    g_ptr_array_unref(list);
    gchar *nested = g_build_filename(path, "alpha", "rootfs", "nested", NULL);
    g_assert_cmpint(mkdir(nested, 0755), ==, 0);
    int fd = open(nested, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    g_assert_false(pcv_lxc_storage_btrfs_validate_copy("alpha", &error));
    g_clear_error(&error);
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    g_assert_cmpint(rmdir(nested), ==, 0);
    gchar *line = g_strdup_printf("77 1 8:1 / %s/alpha/rootfs/bind rw - ext4 /dev/sda rw\n", path);
    mounts = line;
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_create("alpha", "mounted", &error));
    g_clear_error(&error);
    mounts = "";
    g_free(line);
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_delete("alpha", "first", &error));
    g_assert_no_error(error);
    g_free(nested);
    fixture_free(path);
}

static void test_restore_recovery(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    gchar *meta = g_build_filename(path, "alpha", "purecvisor.meta", NULL);
    g_assert_true(g_file_set_contents(meta, "preserved", -1, &error));
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    fail_exchange = 1;
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_rollback("alpha", "first", &error));
    g_clear_error(&error);
    PcvLxcStorage storage = {0};
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    fail_exchange = 0;
    fail_delete = 1;
    g_assert_false(pcv_lxc_storage_recover("alpha", &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_recover("alpha", &error));
    g_assert_no_error(error);
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    pcv_lxc_storage_clear(&storage);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(meta, &contents, NULL, &error));
    g_assert_cmpstr(contents, ==, "preserved");
    g_free(contents);
    g_free(meta);
    fixture_free(path);
}

static void test_delete_retry(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "second", &error));
    fail_delete = 2;
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    PcvLxcStorage storage = {0};
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_resolve_for_destroy("alpha", &storage, &error));
    pcv_lxc_storage_clear(&storage);
    g_assert_true(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_assert_no_error(error);
    gchar *dir = g_build_filename(path, "alpha", NULL);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
    g_free(dir);
    fixture_free(path);
}

static void test_outer_subvolume_refused_before_deletion(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    gchar *snapshots = g_build_filename(path, "alpha", ".pcv-snapshots", NULL);
    int fd = open(snapshots, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    gchar *root = g_build_filename(path, "alpha", "rootfs", NULL);
    g_assert_true(g_file_test(root, G_FILE_TEST_IS_DIR));
    g_free(root);
    g_free(snapshots);
    fixture_free(path);
}

static void test_metadata_ownership_and_symlinks(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    gchar *marker = g_build_filename(path, "alpha", "purecvisor.storage", NULL);
    int owner = 1000;
    g_assert_cmpint(setxattr(marker, "user.pcv_test_owner", &owner, sizeof(owner), 0), ==, 0);
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    g_assert_cmpint(unlink(marker), ==, 0);
    g_assert_cmpint(symlink("/etc/passwd", marker), ==, 0);
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    g_free(marker);
    fixture_free(path);
}

static void test_restore_ambiguous_identity(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    fail_exchange = 1;
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_rollback("alpha", "first", &error));
    g_clear_error(&error);
    fail_exchange = 0;
    gchar *stage = g_build_filename(path, "alpha", ".pcv-restore-new", NULL);
    int fd = open(stage, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    g_assert_false(pcv_lxc_storage_recover("alpha", &error));
    g_clear_error(&error);
    g_assert_true(g_file_test(stage, G_FILE_TEST_IS_DIR));
    gchar *journal = g_build_filename(path, "alpha", ".pcv-restore", NULL);
    g_assert_true(g_file_test(journal, G_FILE_TEST_EXISTS));
    g_free(journal);
    g_free(stage);
    fixture_free(path);
}

static void test_restore_missing_old_after_exchange(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    fail_unlink = ".pcv-restore";
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_rollback("alpha", "first", &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_recover("alpha", &error));
    g_assert_no_error(error);
    PcvLxcStorage storage = {0};
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    pcv_lxc_storage_clear(&storage);
    fixture_free(path);
}

static void test_delete_tombstone_retry(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    fail_unlink = "alpha";
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    PcvLxcStorage storage = {0};
    g_assert_true(pcv_lxc_storage_resolve_for_destroy("alpha", &storage, &error));
    pcv_lxc_storage_clear(&storage);
    fail_unlink = ".pcv-delete-alpha.storage";
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_resolve_for_destroy("alpha", &storage, &error));
    pcv_lxc_storage_clear(&storage);
    g_assert_false(pcv_lxc_storage_prepare_create("alpha", PCV_LXC_STORAGE_BTRFS, &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_assert_no_error(error);
    g_assert_true(pcv_lxc_storage_prepare_create("alpha", PCV_LXC_STORAGE_BTRFS, &error));
    fixture_free(path);
}

static void test_zfs_actual_mount_resolution(void)
{
    gchar *path = fixture();
    gchar *root = g_build_filename(path, "alpha", "rootfs", NULL);
    g_assert_cmpint(g_mkdir_with_parents(root, 0755), ==, 0);
    gchar *line = g_strdup_printf("31 1 0:51 / %s rw - zfs oldpool/containers/alpha rw\n", root);
    mounts = line;
    is_btrfs = FALSE;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_cmpstr(storage.dataset, ==, "oldpool/containers/alpha");
    pcv_lxc_storage_clear(&storage);
    g_assert_true(pcv_lxc_storage_record("alpha", PCV_LXC_STORAGE_ZFS, NULL, &error));
    backend = "btrfs";
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_cmpstr(storage.dataset, ==, "oldpool/containers/alpha");
    pcv_lxc_storage_clear(&storage);
    mounts = "";
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    g_assert_true(pcv_lxc_storage_recover("alpha", &error));
    g_free(line);
    g_free(root);
    fixture_free(path);
}

static void test_snapshot_delete_commit_failure_retains_identity(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    fail_sync = 1;
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_delete("alpha", "first", &error));
    g_clear_error(&error);
    gchar *marker = g_build_filename(path, "alpha", ".pcv-snapshots", ".pcv-first.storage", NULL);
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_delete("alpha", "first", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_free(marker);
    fixture_free(path);
}

static void test_create_missing_base_and_reject_symlink(void)
{
    gchar *path = fixture();
    gchar *nested = g_build_filename(path, "new", "lxc", NULL);
    base_path = nested;
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_prepare_create("alpha", PCV_LXC_STORAGE_ZFS, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(nested, G_FILE_TEST_IS_DIR));
    gchar *link = g_build_filename(path, "link", NULL);
    g_assert_cmpint(symlink(nested, link), ==, 0);
    base_path = link;
    g_assert_false(pcv_lxc_storage_prepare_create("alpha", PCV_LXC_STORAGE_BTRFS, &error));
    g_clear_error(&error);
    g_free(link);
    g_free(nested);
    fixture_free(path);
}

static void test_unmarked_btrfs_and_snapshot_identity_refused(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    gchar *root = g_build_filename(path, "alpha", ".pcv-snapshots", "first", "rootfs", NULL);
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, TRUE);
    close(fd);
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    g_assert_true(g_file_test(root, G_FILE_TEST_IS_DIR));
    gchar *marker = g_build_filename(path, "alpha", "purecvisor.storage", NULL);
    g_assert_cmpint(unlink(marker), ==, 0);
    PcvLxcStorage storage = {0};
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    g_free(marker);
    g_free(root);
    fixture_free(path);
}

static void test_zfs_rootfs_symlink_refused(void)
{
    gchar *path = fixture();
    gchar *dir = g_build_filename(path, "alpha", NULL);
    g_assert_cmpint(mkdir(dir, 0755), ==, 0);
    gchar *root = g_build_filename(dir, "rootfs", NULL);
    g_assert_cmpint(symlink("/", root), ==, 0);
    gchar *line = g_strdup_printf("31 1 0:51 / %s rw - zfs pool/containers/alpha rw\n", dir);
    mounts = line;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_clear_error(&error);
    mounts = "";
    g_free(line);
    g_free(root);
    g_free(dir);
    fixture_free(path);
}

static void test_zfs_mount_ownership_guard(void)
{
    gchar *path = fixture();
    create_container("alpha");
    g_assert_false(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", FALSE));
    g_assert_false(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", TRUE));
    fixture_free(path);
    path = fixture();
    gchar *root = g_build_filename(path, "alpha", "rootfs", NULL);
    g_assert_cmpint(g_mkdir_with_parents(root, 0755), ==, 0);
    g_assert_true(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", TRUE));
    g_assert_false(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", FALSE));
    gchar *line = g_strdup_printf("31 1 0:51 / %s rw - zfs oldpool/alpha rw\n", root);
    mounts = line;
    is_btrfs = FALSE;
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_record("alpha", PCV_LXC_STORAGE_ZFS, NULL, &error));
    mounts = "";
    g_assert_true(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", TRUE));
    g_assert_false(pcv_lxc_storage_zfs_mount_allowed("alpha", "otherpool/alpha", TRUE));
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    subvol_fd(fd, next_id++, FALSE);
    close(fd);
    is_btrfs = TRUE;
    g_assert_false(pcv_lxc_storage_zfs_mount_allowed("alpha", "oldpool/alpha", TRUE));
    g_free(root);
    g_free(line);
    fixture_free(path);
}

static gchar *crashed_metadata(const gchar *directory, const gchar *target)
{
    gchar *filename = g_strdup_printf("%s/.pcv-write-%s-12345678-1234-4234-8234-123456789abc", directory, target);
    int fd = open(filename, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "partial", 7), ==, 7);
    close(fd);
    return filename;
}

static void test_crashed_metadata_delete_retry_scoped(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    gchar *container = g_build_filename(path, "alpha", NULL);
    gchar *snapshots = g_build_filename(container, ".pcv-snapshots", NULL);
    gchar *snap_tmp = crashed_metadata(snapshots, ".pcv-first.storage");
    GPtrArray *list = pcv_lxc_storage_btrfs_snapshot_list("alpha", &error);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 1);
    g_ptr_array_unref(list);
    g_assert_true(g_file_test(snap_tmp, G_FILE_TEST_EXISTS));
    fail_delete = 1;
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    gchar *marker_tmp = crashed_metadata(container, "purecvisor.storage");
    gchar *journal_tmp = crashed_metadata(container, ".pcv-restore");
    gchar *base_tmp = crashed_metadata(path, ".pcv-delete-alpha.storage");
    gchar *other_tmp = crashed_metadata(path, ".pcv-delete-alpha.storage-other.storage");
    g_assert_true(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(container, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(base_tmp, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(other_tmp, G_FILE_TEST_EXISTS));
    g_free(other_tmp);
    g_free(base_tmp);
    g_free(journal_tmp);
    g_free(marker_tmp);
    g_free(snap_tmp);
    g_free(snapshots);
    g_free(container);
    fixture_free(path);
}

static void test_crashed_metadata_restore_retry(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    g_assert_true(pcv_lxc_storage_btrfs_snapshot_create("alpha", "first", &error));
    fail_exchange = 1;
    g_assert_false(pcv_lxc_storage_btrfs_snapshot_rollback("alpha", "first", &error));
    g_clear_error(&error);
    fail_exchange = 0;
    gchar *container = g_build_filename(path, "alpha", NULL);
    gchar *marker_tmp = crashed_metadata(container, "purecvisor.storage");
    gchar *journal_tmp = crashed_metadata(container, ".pcv-restore");
    g_assert_true(pcv_lxc_storage_recover("alpha", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(marker_tmp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(journal_tmp, G_FILE_TEST_EXISTS));
    g_assert_true(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_free(journal_tmp);
    g_free(marker_tmp);
    g_free(container);
    fixture_free(path);
}

static void test_crashed_metadata_tombstone_only_retry(void)
{
    gchar *path = fixture();
    create_container("alpha");
    GError *error = NULL;
    fail_unlink = ".pcv-delete-alpha.storage";
    g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_clear_error(&error);
    gchar *tmp = crashed_metadata(path, ".pcv-delete-alpha.storage");
    gchar *other = crashed_metadata(path, ".pcv-delete-beta.storage");
    g_assert_true(pcv_lxc_storage_btrfs_destroy("alpha", &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(tmp, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(other, G_FILE_TEST_EXISTS));
    g_free(other);
    g_free(tmp);
    fixture_free(path);
}

static void test_crashed_metadata_untrusted_preserved(void)
{
    for (guint variant = 0; variant < 5; variant++) {
        gchar *path = fixture();
        create_container("alpha");
        gchar *container = g_build_filename(path, "alpha", NULL);
        gchar *tmp = crashed_metadata(container, "purecvisor.storage");
        if (variant == 0) {
            g_assert_cmpint(unlink(tmp), ==, 0);
            g_assert_cmpint(symlink("/etc/passwd", tmp), ==, 0);
        } else if (variant == 1) {
            g_assert_cmpint(chmod(tmp, 0644), ==, 0);
        } else if (variant == 2) {
            int owner = 1000;
            g_assert_cmpint(setxattr(tmp, "user.pcv_test_owner", &owner, sizeof(owner), 0), ==, 0);
        } else if (variant == 3) {
            gchar *malformed = g_strdup_printf("%s/.pcv-write-purecvisor.storage-invalid", container);
            g_assert_cmpint(rename(tmp, malformed), ==, 0);
            g_free(tmp);
            tmp = malformed;
        } else {
            gchar *second = g_build_filename(path, "linked", NULL);
            g_assert_cmpint(link(tmp, second), ==, 0);
            g_free(second);
        }
        GError *error = NULL;
        g_assert_false(pcv_lxc_storage_btrfs_destroy("alpha", &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        struct stat st;
        g_assert_cmpint(lstat(tmp, &st), ==, 0);
        gchar *root = g_build_filename(container, "rootfs", NULL);
        g_assert_true(g_file_test(root, G_FILE_TEST_IS_DIR));
        g_free(root);
        g_free(tmp);
        g_free(container);
        fixture_free(path);
    }
}

static void test_zfs_foreign_rootfs_and_stacked_mount_refusal(void)
{
    for (guint variant = 0; variant < 4; variant++) {
        gchar *path = fixture();
        gchar *container = g_build_filename(path, "alpha", NULL);
        gchar *root = g_build_filename(container, "rootfs", NULL);
        g_assert_cmpint(g_mkdir_with_parents(root, 0755), ==, 0);
        const gchar *first = variant == 1 ? root : container;
        const gchar *second = variant == 2 || variant == 3 ? container : root;
        const gchar *other_type = variant == 3 ? "zfs" : "ext4";
        gchar *line = g_strdup_printf("31 1 0:51 / %s rw - zfs pool/alpha rw\n"
                                     "32 1 0:52 / %s rw - %s other rw\n", first, second, other_type);
        mounts = line;
        is_btrfs = FALSE;
        GError *error = NULL;
        PcvLxcStorage storage = {0};
        g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        mounts = "";
        g_free(line);
        g_free(root);
        g_free(container);
        fixture_free(path);
    }
}

static void test_zfs_open_root_mount_identity(void)
{
    gchar *path = fixture();
    gchar *container = g_build_filename(path, "alpha", NULL);
    gchar *root = g_build_filename(container, "rootfs", NULL);
    g_assert_cmpint(g_mkdir_with_parents(root, 0755), ==, 0);
    gchar *line = g_strdup_printf("30 1 0:50 / %s rw - ext4 /dev/sda rw\n"
                                 "31 30 0:51 / %s rw - zfs pool/alpha rw\n", container, root);
    mounts = line;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_cmpstr(storage.dataset, ==, "pool/alpha");
    pcv_lxc_storage_clear(&storage);
    root_mount_id = 99;
    g_assert_false(pcv_lxc_storage_resolve("alpha", &storage, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    mounts = "";
    g_free(line);
    g_free(root);
    g_free(container);
    fixture_free(path);
}

static void test_record_native_directory_mode(void)
{
    for (guint variant = 0; variant < 10; variant++) {
        gchar *path = fixture();
        gchar *container = g_build_filename(path, "alpha", NULL);
        gchar *root = g_build_filename(container, "rootfs", NULL);
        g_assert_cmpint(g_mkdir_with_parents(root, 0755), ==, 0);
        g_assert_cmpint(chmod(container, variant == 3 ? 0772 : 0770), ==, 0);
        int root_fd = open(root, O_RDONLY | O_DIRECTORY);
        subvol_fd(root_fd, next_id++, FALSE);
        close(root_fd);
        if (variant == 1 || variant == 2) {
            int foreign = 1000;
            g_assert_cmpint(setxattr(container, variant == 1 ? "user.pcv_test_group" : "user.pcv_test_owner",
                                    &foreign, sizeof(foreign), 0), ==, 0);
        } else if (variant >= 4 && variant <= 7) {
            const gchar *entries[] = {"purecvisor.storage", ".pcv-restore", ".pcv-restore-new", ".pcv-delete-alpha.storage"};
            gchar *entry = g_build_filename(variant == 7 ? path : container, entries[variant - 4], NULL);
            g_assert_true(g_file_set_contents(entry, "existing", -1, NULL));
            g_free(entry);
        } else if (variant == 8) {
            gchar *link = g_build_filename(path, "alias", NULL);
            g_assert_cmpint(symlink(container, link), ==, 0);
            g_free(link);
        } else if (variant == 9) {
            int fd = open(container, O_RDONLY | O_DIRECTORY);
            subvol_fd(fd, next_id++, FALSE);
            close(fd);
        }
        GError *error = NULL;
        gboolean result = pcv_lxc_storage_record(variant == 8 ? "alias" : "alpha", PCV_LXC_STORAGE_BTRFS, NULL, &error);
        struct stat st;
        g_assert_cmpint(stat(container, &st), ==, 0);
        if (variant == 0) {
            g_assert_true(result);
            g_assert_no_error(error);
            g_assert_cmpuint(st.st_mode & 0777, ==, 0750);
            PcvLxcStorage storage = {0};
            g_assert_true(pcv_lxc_storage_resolve("alpha", &storage, &error));
            pcv_lxc_storage_clear(&storage);
        } else {
            g_assert_false(result);
            g_assert_nonnull(error);
            g_clear_error(&error);
            g_assert_cmpuint(st.st_mode & 0777, ==, variant == 3 ? 0772 : 0770);
        }
        g_free(root);
        g_free(container);
        fixture_free(path);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/storage/default-validation", test_default_validation);
    g_test_add_func("/storage/create-path-guard", test_create_path_guard);
    g_test_add_func("/storage/record-and-tamper", test_record_and_tamper);
    g_test_add_func("/storage/snapshot-and-guards", test_snapshot_and_guards);
    g_test_add_func("/storage/restore-recovery", test_restore_recovery);
    g_test_add_func("/storage/delete-retry", test_delete_retry);
    g_test_add_func("/storage/outer-subvolume-before-delete", test_outer_subvolume_refused_before_deletion);
    g_test_add_func("/storage/metadata-ownership-symlink", test_metadata_ownership_and_symlinks);
    g_test_add_func("/storage/restore-ambiguous", test_restore_ambiguous_identity);
    g_test_add_func("/storage/restore-missing-old", test_restore_missing_old_after_exchange);
    g_test_add_func("/storage/delete-tombstone-retry", test_delete_tombstone_retry);
    g_test_add_func("/storage/zfs-actual-mount", test_zfs_actual_mount_resolution);
    g_test_add_func("/storage/snapshot-delete-commit-failure", test_snapshot_delete_commit_failure_retains_identity);
    g_test_add_func("/storage/create-missing-base", test_create_missing_base_and_reject_symlink);
    g_test_add_func("/storage/unmarked-and-snapshot-identity", test_unmarked_btrfs_and_snapshot_identity_refused);
    g_test_add_func("/storage/zfs-rootfs-symlink", test_zfs_rootfs_symlink_refused);
    g_test_add_func("/lxc_storage/zfs_mount_ownership_guard", test_zfs_mount_ownership_guard);
    g_test_add_func("/storage/crashed-metadata-delete-scoped", test_crashed_metadata_delete_retry_scoped);
    g_test_add_func("/storage/crashed-metadata-restore", test_crashed_metadata_restore_retry);
    g_test_add_func("/storage/crashed-metadata-tombstone", test_crashed_metadata_tombstone_only_retry);
    g_test_add_func("/storage/crashed-metadata-untrusted", test_crashed_metadata_untrusted_preserved);
    g_test_add_func("/storage/zfs-foreign-stacked-mounts", test_zfs_foreign_rootfs_and_stacked_mount_refusal);
    g_test_add_func("/storage/zfs-open-root-mount-identity", test_zfs_open_root_mount_identity);
    g_test_add_func("/storage/record-native-directory-mode", test_record_native_directory_mode);
    return g_test_run();
}
