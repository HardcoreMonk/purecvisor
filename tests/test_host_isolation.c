













#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "test_host_isolation.h"


static gchar *G_root;
static gchar *G_audit_dir;
static gchar *G_audit_path;
static gchar *G_sg_db_path;
static gchar *G_sg_script_dir;
static gchar *G_sg_script_path;


static int
_remove_entry(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;

    if (typeflag == FTW_DP)
        (void)g_rmdir(path);
    else
        (void)g_unlink(path);
    return 0;
}



static void
_host_root_cleanup(void)
{
    if (G_root)
        (void)nftw(G_root, _remove_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}



static const gchar *
_host_root(void)
{
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        GError *error = NULL;
        gchar *root = g_dir_make_tmp("pcv-test-host-XXXXXX", &error);
        if (!root) {
            fprintf(stderr, "FATAL: test host-path seam 임시 루트 생성 실패: %s\n",
                    error ? error->message : "unknown");
            abort();
        }
        G_root = root;
        G_audit_dir = g_build_filename(root, "log", NULL);
        G_audit_path = g_build_filename(G_audit_dir, "audit.log", NULL);
        G_sg_db_path = g_build_filename(root, "lib", "security_groups.db", NULL);
        G_sg_script_dir = g_build_filename(root, "run", NULL);
        G_sg_script_path = g_build_filename(G_sg_script_dir, "sg-ruleset.nft", NULL);



        gchar *lib_dir = g_path_get_dirname(G_sg_db_path);
        if (g_mkdir_with_parents(lib_dir, 0700) != 0) {
            fprintf(stderr, "FATAL: test host-path seam DB 디렉터리 생성 실패: %s\n", lib_dir);
            abort();
        }
        g_free(lib_dir);
        atexit(_host_root_cleanup);
        g_once_init_leave(&once, 1);
    }
    return G_root;
}

const char *
pcv_test_host_root_dir(void)
{
    return _host_root();
}

const char *
pcv_test_audit_log_dir(void)
{
    (void)_host_root();
    return G_audit_dir;
}

const char *
pcv_test_audit_log_path(void)
{
    (void)_host_root();
    return G_audit_path;
}

const char *
pcv_test_sg_db_path(void)
{
    (void)_host_root();
    return G_sg_db_path;
}

const char *
pcv_test_sg_nft_script_dir(void)
{
    (void)_host_root();
    return G_sg_script_dir;
}

const char *
pcv_test_sg_nft_script_path(void)
{
    (void)_host_root();
    return G_sg_script_path;
}
