   
                                
                                                                                            
  
                           
                                                                                 
                                                                  
                                                                                       
                                                                                           
                                                                        
                                                                               
                                                                                                      
                                            
  
                       
                                                                      
                                              
   
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/lxc/lxc_volume_guard.h"
#include "modules/lxc/lxc_volume_mount.h"                                                  

#define LINK_MSG "host_path must not traverse symbolic links"

                                                                                   
                                                       
static gchar *
fixture_new(void)
{
    GError *error = NULL;
    gchar *made = g_dir_make_tmp("pcv-a02-vg-XXXXXX", &error);
    g_assert_no_error(error);
    char real[PATH_MAX];
    g_assert_nonnull(realpath(made, real));                                       
    g_free(made);
    gchar *t = g_strdup(real);
    gchar *ab = g_build_filename(t, "src", "a", "b", NULL);
    g_assert_cmpint(g_mkdir_with_parents(ab, 0755), ==, 0);
    gchar *f = g_build_filename(t, "src", "f", NULL);
    g_assert_true(g_file_set_contents(f, "x", -1, NULL));
    gchar *other = g_build_filename(t, "other", NULL);
    g_assert_cmpint(g_mkdir(other, 0755), ==, 0);
    gchar *esc = g_build_filename(t, "src", "esc", NULL);
    g_assert_cmpint(symlink(other, esc), ==, 0);
    gchar *src = g_build_filename(t, "src", NULL);
    gchar *lnk = g_build_filename(t, "lnk", NULL);
    g_assert_cmpint(symlink(src, lnk), ==, 0);
    g_free(ab); g_free(f); g_free(other); g_free(esc); g_free(src); g_free(lnk);
    return t;
}

                                                        
static void
fixture_free(gchar *t)
{
    static const gchar *const rel[] = { "lnk", "src/esc", "src/f", "src/a/b", "src/a", "src", "other", NULL };
    for (const gchar *const *r = rel; *r; r++) {
        gchar *p = g_build_filename(t, *r, NULL);
        g_assert_cmpint(g_remove(p), ==, 0);
        g_free(p);
    }
    g_assert_cmpint(g_rmdir(t), ==, 0);
    g_free(t);
}

                                                                      
static void
expect_bad_path(const gchar *path, const gchar *msg)
{
    GError *error = NULL;
    gchar *canon = NULL;
    g_assert_cmpint(pcv_lxc_volume_source_open(path, &canon, &error), ==, -1);
    g_assert_true(g_error_matches(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH));
    g_assert_cmpstr(error->message, ==, msg);
    g_assert_null(canon);
    g_clear_error(&error);
}

                                                                           
static void
test_source_open_walks_without_links(void)
{
    gchar *t = fixture_new();
    GError *error = NULL;
    gchar *canon = NULL;

                                                                
    gchar *messy = g_strdup_printf("%s/src//a/./b", t);
    gchar *want = g_strdup_printf("%s/src/a/b", t);
    int fd = pcv_lxc_volume_source_open(messy, &canon, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpstr(canon, ==, want);
    struct stat fs, ps;
    g_assert_cmpint(fstat(fd, &fs), ==, 0);
    g_assert_cmpint(stat(want, &ps), ==, 0);
    g_assert_cmpuint(fs.st_dev, ==, ps.st_dev);
    g_assert_cmpuint(fs.st_ino, ==, ps.st_ino);
    close(fd);
    g_clear_pointer(&canon, g_free);

                                                              
    gchar *file = g_strdup_printf("%s/src/f", t);
    fd = pcv_lxc_volume_source_open(file, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

                                    
    gchar *dir_slash = g_strdup_printf("%s/src/a/", t);
    gchar *dir_want = g_strdup_printf("%s/src/a", t);
    fd = pcv_lxc_volume_source_open(dir_slash, &canon, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpstr(canon, ==, dir_want);
    close(fd);
    g_clear_pointer(&canon, g_free);
    g_free(dir_slash);
    g_free(dir_want);

                    
    fd = pcv_lxc_volume_source_open("//", &canon, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpstr(canon, ==, "/");
    close(fd);
    g_clear_pointer(&canon, g_free);

                                                               
    static const gchar *const bad[] = { "src/esc", "src/esc/x", "lnk/a", "src/a/../f", NULL };
    for (const gchar *const *b = bad; *b; b++) {
        gchar *p = g_strdup_printf("%s/%s", t, *b);
        expect_bad_path(p, LINK_MSG);
        g_free(p);
    }

               
    expect_bad_path("src/a", "host_path must be an absolute path");
    expect_bad_path("", "host_path must be an absolute path");

                                                                                              
    gchar *nope = g_strdup_printf("%s/nope", t);
    gchar *under_file = g_strdup_printf("%s/src/f/x", t);
    gchar *file_slash = g_strdup_printf("%s/src/f/.", t);                                             
    for (const gchar *const *p = (const gchar *const[]){ nope, under_file, file_slash, NULL }; *p; p++) {
        g_assert_cmpint(pcv_lxc_volume_source_open(*p, &canon, &error), ==, -1);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        g_assert_null(canon);
        g_clear_error(&error);
    }

    g_free(messy); g_free(want); g_free(file); g_free(nope); g_free(under_file); g_free(file_slash);
    fixture_free(t);
}

                                                              
static void
test_source_open_rejects_magic_links(void)
{
    expect_bad_path("/proc/self/cwd", LINK_MSG);
    expect_bad_path("/proc/self/root/etc", LINK_MSG);
    gchar *cwd = g_strdup_printf("/proc/%d/cwd", (int)getpid());                             
    gchar *root = g_strdup_printf("/proc/%d/root/etc", (int)getpid());                      
    expect_bad_path(cwd, LINK_MSG);
    expect_bad_path(root, LINK_MSG);
    g_free(cwd);
    g_free(root);
}

                                                                       
                                                                               
                                                                         
                                                                                  

                                                                             
                                           
static gchar *
config_put(const gchar *t, const gchar *text)
{
    gchar *dir = g_build_filename(t, "lxc", "vg", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
    gchar *cfg = g_build_filename(dir, "config", NULL);
    g_assert_true(g_file_set_contents(cfg, text, -1, NULL));
    g_free(cfg);
    g_free(dir);
    return g_build_filename(t, "lxc", NULL);
}

                                                                                 
static void
config_drop(gchar *lxc)
{
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    gchar *dir = g_build_filename(lxc, "vg", NULL);
    g_assert_cmpint(g_remove(cfg), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpint(g_rmdir(lxc), ==, 0);
    g_free(cfg);
    g_free(dir);
    g_free(lxc);
}

                                                
static void
expect_entry(GPtrArray *entries, guint i, const gchar *source, const gchar *dest, const gchar *options, gboolean is_bind)
{
    const PcvLxcMountEntry *e = entries->pdata[i];
    g_assert_cmpstr(e->source, ==, source);
    g_assert_cmpstr(e->dest, ==, dest);
    g_assert_cmpstr(e->options, ==, options);
    g_assert_cmpint(e->is_bind, ==, is_bind);
}

                                                                                                      
                                                                                                
static void
test_parse_mount_entries(void)
{
    static const gchar text[] =
        "# lxc.mount.entry = /srv/c mnt/c none bind 0 0\n"
        "lxc.include = /usr/share/lxc/config/common.conf\n"
        "lxc.mount.entry = /srv/a mnt/a none bind 0 0\n"
        "  \tlxc.mount.entry\t=\t/srv/b\t\tmnt/b   none ro,bind 0 0\n"
        "lxc.mount.entry = /srv/r mnt/r none rbind 0 0\n"
        "lxc.mount.entryX = /srv/x mnt/x none bind 0 0\n"
        "lxc.mount.entry=/srv/e mnt/e none bind,errors=remount-ro 0 0\n"
        "lxc.mount.entry = proc proc proc nodev,noexec,nosuid,unbindable 0 0";
    GPtrArray *entries = pcv_lxc_config_mount_entries(text);
    g_assert_cmpuint(entries->len, ==, 5);
    expect_entry(entries, 0, "/srv/a", "mnt/a", "bind", TRUE);
    expect_entry(entries, 1, "/srv/b", "mnt/b", "ro,bind", TRUE);
    expect_entry(entries, 2, "/srv/r", "mnt/r", "rbind", TRUE);
    expect_entry(entries, 3, "/srv/e", "mnt/e", "bind,errors=remount-ro", TRUE);
    expect_entry(entries, 4, "proc", "proc", "nodev,noexec,nosuid,unbindable", FALSE);
    g_ptr_array_unref(entries);

                                                                                                    
    entries = pcv_lxc_config_mount_entries("lxc.mount.entry = \"/srv/q mnt/q none bind 0 0\"  \n"
                                           "lxc.mount.entry = /srv/a\\040b\\011c\\134d mnt/s none bind=x 0 0\n"
                                           "lxc.mount.entry =   \n");
    g_assert_cmpuint(entries->len, ==, 2);
    expect_entry(entries, 0, "/srv/q", "mnt/q", "bind", TRUE);
    expect_entry(entries, 1, "/srv/a b\tc\\d", "mnt/s", "bind=x", FALSE);                                     
    g_ptr_array_unref(entries);

                                                                                           
    entries = pcv_lxc_config_mount_entries("lxc.mount.entry = /srv/c mnt/c none bind\r\n"
                                           "lxc.mount.entry = /srv/d mnt/d none ro,bind 0 0\r\n");
    g_assert_cmpuint(entries->len, ==, 2);
    expect_entry(entries, 0, "/srv/c", "mnt/c", "bind", TRUE);
    expect_entry(entries, 1, "/srv/d", "mnt/d", "ro,bind", TRUE);
    g_ptr_array_unref(entries);

    entries = pcv_lxc_config_mount_entries(NULL);
    g_assert_cmpuint(entries->len, ==, 0);
    g_ptr_array_unref(entries);
}

                                                                            
static void
test_check_passes_real_sources(void)
{
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.uts.name = vg\n"
                                  "lxc.mount.entry = %s/src/a mnt/a none bind,ro 0 0\n"
                                  "lxc.mount.entry = %s/src/f mnt/f none bind,create=file 0 0\n"
                                  "lxc.mount.entry = %s/src/esc mnt/n none ro 0 0\n"                                 
                                  "lxc.mount.entry = proc proc proc nodev 0 0\n"
                                  "lxc.mount.entry = rel/x mnt/r none bind 0 0\n", t, t, t);
    gchar *lxc = config_put(t, text);
    gchar *bad = NULL;
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));
    g_assert_null(bad);
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", NULL));                           
    config_drop(lxc);
    g_free(text);
    fixture_free(t);
}

                                                                                  
                                                  
static void
test_check_rejects_link_swap(void)
{
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.mount.entry = %s/other mnt/o none bind 0 0\n"
                                  "lxc.mount.entry = %s/src/a mnt/a none bind,ro 0 0\n", t, t);
    gchar *lxc = config_put(t, text);
    gchar *a = g_build_filename(t, "src", "a", NULL);
    gchar *ab = g_build_filename(t, "src", "a", "b", NULL);
    gchar *other = g_build_filename(t, "other", NULL);
    gchar *bad = NULL;
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));                            
    g_assert_null(bad);

                                                      
    g_assert_cmpint(g_rmdir(ab), ==, 0);
    g_assert_cmpint(g_rmdir(a), ==, 0);
    g_assert_cmpint(symlink(other, a), ==, 0);
    g_assert_false(pcv_lxc_volume_guard_check(lxc, "vg", &bad));
    g_assert_cmpstr(bad, ==, "/mnt/a");
    gchar *msg = pcv_lxc_volume_guard_message(bad);
    g_assert_cmpstr(msg, ==, "volume source changed since attach: /mnt/a");
    g_assert_null(strstr(msg, t));
    g_free(msg);
    g_clear_pointer(&bad, g_free);
    g_assert_cmpint(g_remove(a), ==, 0);                   
    g_assert_cmpint(g_mkdir_with_parents(ab, 0755), ==, 0);
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));                        

                                                                                  
    gchar *src = g_build_filename(t, "src", NULL);
    gchar *real = g_build_filename(t, "src.real", NULL);
    g_free(text);
    text = g_strdup_printf("lxc.mount.entry = %s/src/a //mnt/a none bind,ro 0 0\n", t);
    config_drop(lxc);
    lxc = config_put(t, text);
    g_assert_cmpint(g_rename(src, real), ==, 0);
    g_assert_cmpint(symlink(real, src), ==, 0);
    g_assert_false(pcv_lxc_volume_guard_check(lxc, "vg", &bad));
    g_assert_cmpstr(bad, ==, "/mnt/a");
    g_clear_pointer(&bad, g_free);
    g_assert_cmpint(g_remove(src), ==, 0);
    g_assert_cmpint(g_rename(real, src), ==, 0);

                                             
    g_free(text);
    text = g_strdup_printf("lxc.mount.entry = %s/src/a/../f mnt/f none bind 0 0\n", t);
    config_drop(lxc);
    lxc = config_put(t, text);
    g_assert_false(pcv_lxc_volume_guard_check(lxc, "vg", &bad));
    g_assert_cmpstr(bad, ==, "/mnt/f");
    g_clear_pointer(&bad, g_free);

    config_drop(lxc);
    g_free(text); g_free(a); g_free(ab); g_free(other); g_free(src); g_free(real);
    fixture_free(t);
}

                                                                                 
static void
test_check_leaves_missing_source_to_liblxc(void)
{
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.mount.entry = %s/gone mnt/g none bind,optional 0 0\n"
                                  "lxc.mount.entry = %s/src/f/x mnt/x none bind,optional 0 0\n"
                                  "lxc.mount.entry = %s/src/a/b mnt/b none bind 0 0\n", t, t, t);
    gchar *lxc = config_put(t, text);
    gchar *a = g_build_filename(t, "src", "a", NULL);
    g_assert_cmpint(g_chmod(a, 0), ==, 0);                                                                        
    gchar *bad = NULL;
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));
    g_assert_null(bad);
    g_assert_cmpint(g_chmod(a, 0755), ==, 0);
    config_drop(lxc);
    g_free(text);
    g_free(a);
    fixture_free(t);
}

                                                                    
static void
test_check_missing_config_ok(void)
{
    gchar *t = fixture_new();
    gchar *lxc = g_build_filename(t, "lxc", NULL);
    gchar *bad = NULL;
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));                     
    g_assert_null(bad);
    g_assert_cmpint(g_mkdir(lxc, 0755), ==, 0);
    g_assert_true(pcv_lxc_volume_guard_check(lxc, "vg", &bad));                         
    g_assert_null(bad);
    g_assert_cmpint(g_rmdir(lxc), ==, 0);
    g_free(lxc);
    fixture_free(t);
}

                                                                          
                                                                                 
                                                                                              
                                                                 

#define HOOK_LINE PCV_LXC_VOLUME_GUARD_HOOK_KEY " = " PCV_LXC_VOLUME_GUARD_HOOK_VALUE

                                                                                       
static guint
count_lines(const gchar *text, const gchar *line)
{
    gchar **lines = g_strsplit_set(text, "\r\n", -1);
    guint n = 0;
    for (gchar **l = lines; *l; l++)
        n += g_strcmp0(*l, line) == 0;
    g_strfreev(lines);
    return n;
}

                                                                                                                            
                                                                                                            
static void
test_hook_main_exit_codes(void)
{
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.uts.name = vg\n"
                                  "lxc.mount.entry = %s/src/a mnt/a none bind,ro 0 0\n"
                                  "lxc.mount.entry = %s/gone mnt/g none bind,optional 0 0\n"
                                  "lxc.mount.entry = proc proc proc nodev 0 0\n", t, t);
    gchar *lxc = config_put(t, text);
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    gchar *volgen = g_build_filename(lxc, "vg", PCV_LXC_VOLUME_GUARD_VOLGEN, NULL);
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(cfg, "vg"), ==, 1);                                     
    g_assert_true(g_file_set_contents(volgen, "0000\n", -1, NULL));                                         
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(cfg, "vg"), ==, 1);
    g_assert_cmpint(g_remove(volgen), ==, 0);

    gchar *none = g_strdup("lxc.uts.name = vg\n"
                           "lxc.mount.entry = proc proc proc nodev 0 0\n"
                           "lxc.mount.entry = /srv/x mnt/x none ro 0 0\n");               
    g_assert_true(g_file_set_contents(cfg, none, -1, NULL));
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(cfg, "vg"), ==, 0);                
    g_assert_true(g_file_set_contents(cfg, "lxc.mount.entry = rel/x mnt/r none bind 0 0\n", -1, NULL));
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(cfg, "vg"), ==, 1);                                               
    g_assert_true(g_file_set_contents(cfg, none, -1, NULL));
    static const gchar *const bad_names[] = { NULL, "", ".", "..", "../x", "a/b", "/", NULL };
    for (guint i = 0; i < 7; i++)
        g_assert_cmpint(pcv_lxc_volume_guard_hook_main(cfg, bad_names[i]), ==, 1);                            

    gchar *gone = g_build_filename(t, "lxc", "nope", "config", NULL);
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(gone, "vg"), ==, 1);                 
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main("", "vg"), ==, 1);                             
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(NULL, "vg"), ==, 1);                          
    g_assert_cmpint(pcv_lxc_volume_guard_hook_main(lxc, "vg"), ==, 1);                         
    g_free(gone);
    g_free(cfg);
    g_free(volgen);
    config_drop(lxc);
    g_free(text); g_free(none);
    fixture_free(t);
}

                                                                                      
                                                                                   
static void
test_hook_main_stderr(void)
{
    if (g_test_subprocess()) {
        const gchar *root = g_getenv("PCV_TEST_HOOK_ROOT");
        const gchar *cfg = g_getenv("PCV_TEST_HOOK_CONFIG"), *name = g_getenv("PCV_TEST_HOOK_NAME");
        exit(root ? pcv_lxc_volume_guard_hook_run_with_root(root, cfg, name) : pcv_lxc_volume_guard_hook_main(cfg, name));
    }
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.mount.entry = %s/src/a mnt/a none bind 0 0\n", t);                         
    gchar *lxc = config_put(t, text);
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    gchar *pattern = g_strdup_printf("*%s*", t);
    g_setenv("PCV_TEST_HOOK_CONFIG", cfg, TRUE);
    g_setenv("PCV_TEST_HOOK_NAME", "vg", TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: volume source is outside the purecvisor staging area — start it through purecvisor\n");
    g_test_trap_assert_stderr_unmatched(pattern);
    g_test_trap_assert_stdout("");

    g_setenv("PCV_TEST_HOOK_NAME", "../x", TRUE);                                    
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: cannot read the container name (LXC_NAME)\n");
    g_unsetenv("PCV_TEST_HOOK_NAME");
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: cannot read the container name (LXC_NAME)\n");

    g_assert_true(g_file_set_contents(cfg, "lxc.mount.entry = srv/data mnt/data none bind 0 0\n", -1, NULL));                   
    g_setenv("PCV_TEST_HOOK_NAME", "vg", TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: bind mount with a relative source is not allowed: mnt/data\n");
    g_test_trap_assert_stderr_unmatched(pattern);

                                                         
    gchar *vs = g_build_filename(t, "vs", NULL), *c1 = g_build_filename(t, "vs", "vg", NULL), *lnk = g_build_filename(t, "vs", "vg", "k.0", NULL);
    gchar *other = g_build_filename(t, "other", NULL);
    g_assert_cmpint(g_mkdir_with_parents(c1, 0755), ==, 0);
    g_assert_cmpint(symlink(other, lnk), ==, 0);
    gchar *rt = g_strdup_printf("lxc.mount.entry = %s mnt/e none bind 0 0\n", lnk);
    g_assert_true(g_file_set_contents(cfg, rt, -1, NULL));
    g_setenv("PCV_TEST_HOOK_NAME", "vg", TRUE);
    g_setenv("PCV_TEST_HOOK_ROOT", vs, TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: volume source changed since attach: /mnt/e\n");
    g_test_trap_assert_stderr_unmatched(pattern);
    g_unsetenv("PCV_TEST_HOOK_ROOT");
    g_assert_cmpint(g_remove(lnk), ==, 0);
    g_assert_cmpint(g_rmdir(c1), ==, 0);
    g_assert_cmpint(g_rmdir(vs), ==, 0);

    gchar *gone = g_build_filename(t, "lxc", "nope", "config", NULL);
    g_setenv("PCV_TEST_HOOK_CONFIG", gone, TRUE);
    g_setenv("PCV_TEST_HOOK_NAME", "vg", TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("pcv volume guard: cannot read the container config (LXC_CONFIG_FILE)\n");
    g_unsetenv("PCV_TEST_HOOK_CONFIG");
    g_unsetenv("PCV_TEST_HOOK_NAME");
    g_free(gone); g_free(pattern); g_free(cfg); g_free(vs); g_free(c1); g_free(lnk); g_free(other); g_free(rt);
    config_drop(lxc);
    g_free(text);
    fixture_free(t);
}

                                                                                   
                                                                   
static void
test_with_guard_hook(void)
{
                                                    
    g_assert_null(pcv_lxc_config_with_guard_hook(NULL));
    g_assert_null(pcv_lxc_config_with_guard_hook("lxc.uts.name = x\n"
                                                 "lxc.mount.entry = proc proc proc nodev 0 0\n"
                                                 "lxc.mount.entry = rel/x mnt/r none bind 0 0\n"
                                                 "lxc.mount.entry = /srv/n mnt/n none ro 0 0\n"));
                                     
    static const gchar base[] = "lxc.uts.name = x\n"
                                "lxc.hook.pre-start = /usr/share/lxcfs/lxc.mount.hook\n"
                                "lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n";
    gchar *once = pcv_lxc_config_with_guard_hook(base);
    g_assert_cmpstr(once, ==, "lxc.uts.name = x\n"
                              "lxc.hook.pre-start = /usr/share/lxcfs/lxc.mount.hook\n"
                              "lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n"
                              HOOK_LINE "\n");
    g_assert_cmpstr(HOOK_LINE, ==, "lxc.hook.pre-start = /usr/local/bin/pcvctl container volume-guard-hook");
    g_assert_null(pcv_lxc_config_with_guard_hook(once));                         
    g_free(once);
                                                     
    gchar *nl = pcv_lxc_config_with_guard_hook("lxc.mount.entry = /srv/a mnt/a none bind 0 0");
    g_assert_cmpstr(nl, ==, "lxc.mount.entry = /srv/a mnt/a none bind 0 0\n" HOOK_LINE "\n");
    g_free(nl);
    gchar *crlf = pcv_lxc_config_with_guard_hook("lxc.mount.entry = /srv/a mnt/a none bind 0 0\r\n");
    g_assert_cmpstr(crlf, ==, "lxc.mount.entry = /srv/a mnt/a none bind 0 0\r\n" HOOK_LINE "\n");
    g_free(crlf);
                                                                  
    g_assert_null(pcv_lxc_config_with_guard_hook("lxc.mount.entry = /srv/a mnt/a none bind 0 0\n"
                                                 "  lxc.hook.pre-start=\t" PCV_LXC_VOLUME_GUARD_HOOK_VALUE "  \n"));
    g_assert_null(pcv_lxc_config_with_guard_hook("lxc.mount.entry = /srv/a mnt/a none bind 0 0\r\n"
                                                 "lxc.hook.pre-start = \"" PCV_LXC_VOLUME_GUARD_HOOK_VALUE "\"\r\n"));
                                  
    static const gchar *const absent[] = {
        "# " HOOK_LINE "\n",
        "lxc.hook.pre-startX = " PCV_LXC_VOLUME_GUARD_HOOK_VALUE "\n",
        "lxc.hook.mount = " PCV_LXC_VOLUME_GUARD_HOOK_VALUE "\n",
        "lxc.hook.pre-start = " PCV_LXC_VOLUME_GUARD_HOOK " container volume-guard-hookX\n",
                                                                       
        HOOK_LINE "\nlxc.hook.pre-start =\n",
        HOOK_LINE "\nlxc.hook =   \n",
        NULL,
    };
    for (const gchar *const *p = absent; *p; p++) {
        gchar *in = g_strconcat("lxc.mount.entry = /srv/a mnt/a none bind 0 0\n", *p, NULL);
        gchar *out = pcv_lxc_config_with_guard_hook(in);
        g_assert_nonnull(out);
        g_assert_true(g_str_has_prefix(out, in));
        g_assert_cmpstr(out + strlen(in), ==, HOOK_LINE "\n");
        g_free(out);
        g_free(in);
    }
                                   
    g_assert_null(pcv_lxc_config_with_guard_hook("lxc.mount.entry = /srv/a mnt/a none bind 0 0\n"
                                                 "lxc.hook.pre-start =\n" HOOK_LINE "\n"));
}

                                                                                       
                                                                                    
static void
test_config_save_adds_hook_once(void)
{
    gchar *t = fixture_new();
    gchar *lxc = config_put(t, "lxc.uts.name = vg\nlxc.hook.pre-start = /bin/true\n");
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    g_assert_cmpint(g_chmod(cfg, 0640), ==, 0);
    GError *error = NULL;

                                                              
    GStatBuf st0, st1;
    g_assert_cmpint(g_stat(cfg, &st0), ==, 0);
    g_assert_true(pcv_lxc_volume_config_save(lxc, "vg", NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(cfg, &st1), ==, 0);
    g_assert_cmpuint(st0.st_ino, ==, st1.st_ino);

                                             
    g_assert_true(pcv_lxc_volume_config_save(lxc, "vg", "/srv/a mnt/a none bind,ro 0 0", &error));
    g_assert_no_error(error);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "lxc.uts.name = vg\nlxc.hook.pre-start = /bin/true\n"
                              "lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n" HOOK_LINE "\n");
    g_clear_pointer(&text, g_free);
    g_assert_cmpint(g_stat(cfg, &st1), ==, 0);
    g_assert_cmpuint(st1.st_mode & 0777, ==, 0640);                 
    g_assert_cmpuint(st1.st_uid, ==, st0.st_uid);

                                         
    g_assert_true(pcv_lxc_volume_config_save(lxc, "vg", "/srv/b mnt/b none bind 0 0", &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    g_assert_cmpuint(count_lines(text, HOOK_LINE), ==, 1);
    g_assert_cmpuint(count_lines(text, "lxc.hook.pre-start = /bin/true"), ==, 1);
    g_assert_cmpuint(count_lines(text, "lxc.mount.entry = /srv/b mnt/b none bind 0 0"), ==, 1);
    g_clear_pointer(&text, g_free);

                                            
    g_assert_cmpint(g_stat(cfg, &st0), ==, 0);
    g_assert_true(pcv_lxc_volume_config_save(lxc, "vg", NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(cfg, &st1), ==, 0);
    g_assert_cmpuint(st0.st_ino, ==, st1.st_ino);

                                                                 
    g_assert_true(g_file_set_contents(cfg, "lxc.mount.entry = /srv/a mnt/a none bind 0 0\n", -1, NULL));
    g_assert_true(pcv_lxc_volume_config_save(lxc, "vg", NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "lxc.mount.entry = /srv/a mnt/a none bind 0 0\n" HOOK_LINE "\n");
    g_clear_pointer(&text, g_free);

                                            
    gchar *real = g_build_filename(t, "real-config", NULL);
    g_assert_cmpint(g_rename(cfg, real), ==, 0);
    g_assert_cmpint(symlink(real, cfg), ==, 0);
    g_assert_false(pcv_lxc_volume_config_save(lxc, "vg", "/srv/c mnt/c none bind 0 0", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_true(g_file_get_contents(real, &text, NULL, NULL));
    g_assert_null(strstr(text, "/srv/c"));
    g_clear_pointer(&text, g_free);
    g_assert_cmpint(g_remove(cfg), ==, 0);
    g_assert_cmpint(g_rename(real, cfg), ==, 0);

    g_free(real);
    g_free(cfg);
    config_drop(lxc);
    fixture_free(t);
}

                                                                                                  
static void
test_hook_installed(void)
{
    g_assert_false(pcv_lxc_volume_guard_hook_installed(NULL));
    g_assert_false(pcv_lxc_volume_guard_hook_installed("/nonexistent/pcv-a02-vg/pcvctl"));
    g_assert_false(pcv_lxc_volume_guard_hook_installed("/"));                          
    g_assert_false(pcv_lxc_volume_guard_hook_installed("/etc/passwd"));                          
    g_assert_true(pcv_lxc_volume_guard_hook_installed("/bin/sh"));                                  
    if (geteuid() != 0) {
        gchar *t = fixture_new();
        gchar *mine = g_build_filename(t, "src", "f", NULL);
        g_assert_cmpint(g_chmod(mine, 0755), ==, 0);
        g_assert_false(pcv_lxc_volume_guard_hook_installed(mine));                       
        g_free(mine);
        fixture_free(t);
    }
}

                                                                                            
                                                                                 

                                                                                                        
static gchar *
expect_slot_path(const gchar *root, const gchar *name, const gchar *dest_trimmed, guint ordinal)
{
    gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, dest_trimmed, -1);
    gchar *p = g_strdup_printf("%s/%s/%.16s.%u", root, name, hex, ordinal);
    g_free(hex);
    return p;
}

                                                                         
static void
test_volstage_slot_deterministic(void)
{
    gchar *a = pcv_lxc_volstage_slot("/s r", "c1", "mnt/data", 0);
    gchar *want = expect_slot_path("/s r", "c1", "mnt/data", 0);
    g_assert_cmpstr(a, ==, want);
    gchar *b = pcv_lxc_volstage_slot("/s r", "c1", "/mnt/data/", 0);
    gchar *c = pcv_lxc_volstage_slot("/s r", "c1", "mnt/data", 0);
    g_assert_cmpstr(b, ==, a);
    g_assert_cmpstr(c, ==, a);
                                                                                                                                  
    static const gchar *const spell[] = { "mnt//data", "mnt/./data", "./mnt/data/.", "//mnt/.//data//" };
    for (guint i = 0; i < G_N_ELEMENTS(spell); i++) {
        g_autofree gchar *v = pcv_lxc_volstage_slot("/s r", "c1", spell[i], 0);
        g_assert_cmpstr(v, ==, a);
    }
    gchar *one = pcv_lxc_volstage_slot("/s r", "c1", "mnt/data", 1);
    gchar *want1 = expect_slot_path("/s r", "c1", "mnt/data", 1);
    g_assert_cmpstr(one, ==, want1);
    g_assert_true(g_str_has_suffix(one, ".1") && g_str_has_suffix(a, ".0"));
    gchar *x = pcv_lxc_volstage_slot("/s r", "c", "mnt/data", 0), *y = pcv_lxc_volstage_slot("/s r", "c1", "mnt/data", 0);
    g_assert_cmpstr(x, !=, y);
    gchar *dx = g_path_get_dirname(x), *dy = g_path_get_dirname(y);
    g_assert_cmpstr(dx, ==, "/s r/c");
    g_assert_cmpstr(dy, ==, "/s r/c1");                          
    gchar *other = pcv_lxc_volstage_slot("/s r", "c1", "mnt/other", 0);
    g_assert_cmpstr(other, !=, a);
    g_free(a); g_free(want); g_free(b); g_free(c); g_free(one); g_free(want1); g_free(x); g_free(y); g_free(dx); g_free(dy); g_free(other);
}

                                                                                                             
static void
test_dest_canon(void)
{
    static const struct { const gchar *in, *out; } rows[] = {
        { "/mnt//./a/", "mnt/a" }, { "mnt/a", "mnt/a" }, { "/", "" }, { ".", "" }, { "./.", "" }, { "", "" }, { "mnt/..", "mnt/.." },
        { "mnt/.a/a.", "mnt/.a/a." }, { "//a//b//", "a/b" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_autofree gchar *v = pcv_lxc_volume_dest_canon(rows[i].in);
        g_assert_cmpstr(v, ==, rows[i].out);
    }
    g_autofree gchar *n = pcv_lxc_volume_dest_canon(NULL);
    g_assert_cmpstr(n, ==, "");
}

                                                                                                    
static void
slot_entries_check(const gchar *orig, const gchar *result, const gchar *const *want_sources)
{
    GPtrArray *eo = pcv_lxc_config_mount_entries(orig), *er = pcv_lxc_config_mount_entries(result);
    g_assert_cmpuint(eo->len, ==, er->len);
    guint v = 0;
    for (guint i = 0; i < eo->len; i++) {
        const PcvLxcMountEntry *o = eo->pdata[i], *r = er->pdata[i];
        g_assert_cmpstr(r->dest, ==, o->dest);
        g_assert_cmpstr(r->options, ==, o->options);
        g_assert_cmpint(r->is_bind, ==, o->is_bind);
        if (pcv_lxc_mount_entry_is_volume(o)) {
            g_assert_cmpstr(r->source, ==, want_sources[v]);
            v++;
        } else {
            g_assert_cmpstr(r->source, ==, o->source);
        }
    }
    g_assert_cmpuint(v, >, 0);
    g_assert_null(want_sources[v]);
    g_ptr_array_unref(eo);
    g_ptr_array_unref(er);
}

                                                                                 
                                              
static void
test_runtime_config_replaces_sources(void)
{
    static const gchar root[] = "/tmp/pcv vs root";                        
    static const gchar body[] =
        "# lxc.mount.entry = /srv/commented mnt/c none bind 0 0\n"
        "lxc.rootfs.path = zfs:tank/c1\n"
        "lxc.mount.entry = proc mnt/a proc defaults 0 0\n"                                            
        "lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\n"
        "lxc.mount.entry = /srv/b mnt/b none rbind 0 0\r\n"
        "  lxc.mount.entry\t=\t/srv/gone\tmnt/g\tnone\tbind,optional\t0\t0\n"
        "lxc.mount.entry = /srv/my\\040dir mnt/sp none bind 0 0\n"
        "lxc.mount.entry = \"/srv/q mnt/q none bind,ro 0 0\"\n"
        "lxc.mount.entry = proc proc proc nodev 0 0\n"
        "lxc.mount.entry = rel/x mnt/r none bind 0 0\n"
        "lxc.mount.entry = /srv/two /mnt/a/ none bind 0 0\n"                                           
        "lxc.hook.pre-start = /usr/local/bin/pcvctl container volume-guard-hook\n";
    GError *error = NULL;
    gchar *res = pcv_lxc_runtime_config_text(body, root, "c1", &error);
    g_assert_no_error(error);
    g_assert_nonnull(res);
    gchar *s0 = expect_slot_path(root, "c1", "mnt/a", 0), *s1 = expect_slot_path(root, "c1", "mnt/b", 0);
    gchar *s2 = expect_slot_path(root, "c1", "mnt/g", 0), *s3 = expect_slot_path(root, "c1", "mnt/sp", 0);
    gchar *s4 = expect_slot_path(root, "c1", "mnt/q", 0), *s5 = expect_slot_path(root, "c1", "mnt/a", 1);
    const gchar *want[] = { s0, s1, s2, s3, s4, s5, NULL };
    slot_entries_check(body, res, want);

                                                                          
    gchar **lo = g_strsplit_set(body, "\r\n", -1), **lr = g_strsplit_set(res, "\r\n", -1);
    g_assert_cmpuint(g_strv_length(lo), ==, g_strv_length(lr));
    for (guint i = 0; lo[i]; i++) {
        gboolean vol = lo[i][0] != '#' && strstr(lo[i], "/srv/") != NULL;
        if (vol)
            g_assert_cmpstr(lo[i], !=, lr[i]);
        else
            g_assert_cmpstr(lo[i], ==, lr[i]);
    }
    g_assert_nonnull(strstr(res, "\r\n  lxc.mount.entry\t=\t/tmp/pcv\\040vs\\040root/c1/"));
    g_assert_nonnull(strstr(res, "\tmnt/g\tnone\tbind,optional\t0\t0\n"));
    g_assert_nonnull(strstr(res, "lxc.mount.entry = \"/tmp/pcv\\040vs\\040root/c1/"));
    g_assert_true(g_str_has_suffix(strstr(res, "\\040root/c1/") ? res : "", "container volume-guard-hook\n"));
    g_assert_nonnull(strstr(res, " mnt/q none bind,ro 0 0\"\n"));
    g_assert_null(strstr(res, "/srv/a mnt/a"));
    g_assert_null(strstr(res, "/srv/my"));
    g_strfreev(lo); g_strfreev(lr);
    g_free(res); g_free(s0); g_free(s1); g_free(s2); g_free(s3); g_free(s4); g_free(s5);
}

                                                                                            
static gchar *
escape_for_config(const gchar *src)
{
    GString *g = g_string_new(NULL);
    for (const gchar *p = src; *p; p++) {
        if (*p == ' ') g_string_append(g, "\\040");
        else if (*p == '\t') g_string_append(g, "\\011");
        else if (*p == '\\') g_string_append(g, "\\134");
        else g_string_append_c(g, *p);
    }
    return g_string_free(g, FALSE);
}

                                                                                       
                                                                   
static void
test_runtime_config_no_host_source(void)
{
    static const gchar *const cands[] = {
        "/srv/data", "/srv/with space/x", "/srv/tab\there/y", "/srv/back\\slash/z", "/srv/한글/볼륨",
        "/srv/l1/l2/l3/l4/l5/l6/l7/l8/deep", "/srv/dots/../up/x", "/srv/trailing/", "/srv//double//slash", "/srv/a-b_c.d/e", NULL,
    };
    static const gchar *const opts[] = { "bind", "bind,ro", "rbind", "rbind,ro,optional", "bind,create=dir" };
    static const gchar root[] = "/run/pcv-hostless-root";
    GRand *rnd = g_rand_new_with_seed(20261103);
    for (guint n = 0; n < 16; n++) {
        GString *body = g_string_new("lxc.uts.name = c9\n# lxc.mount.entry = /srv/never mnt/zz none bind 0 0\n");
        GPtrArray *used = g_ptr_array_new_with_free_func(g_free);
        guint lines = 1 + g_rand_int_range(rnd, 0, 4);
        for (guint j = 0; j < lines; j++) {
            const gchar *src = cands[g_rand_int_range(rnd, 0, 10)];
            gchar *esc = escape_for_config(src);
            g_string_append_printf(body, "lxc.mount.entry = %s mnt/d%u none %s 0 0\n", esc, j, opts[g_rand_int_range(rnd, 0, 5)]);
            g_ptr_array_add(used, g_strdup(src));
            g_ptr_array_add(used, esc);
        }
        GError *error = NULL;
        gchar *res = pcv_lxc_runtime_config_text(body->str, root, "c9", &error);
        g_assert_no_error(error);
        g_assert_nonnull(res);
        GPtrArray *er = pcv_lxc_config_mount_entries(res);
        g_assert_cmpuint(er->len, ==, lines);
        for (guint i = 0; i < er->len; i++)
            g_assert_true(g_str_has_prefix(((const PcvLxcMountEntry *)er->pdata[i])->source, "/run/pcv-hostless-root/c9/"));
        gchar **ls = g_strsplit_set(res, "\r\n", -1);
        for (gchar **l = ls; *l; l++) {
            if ((*l)[strspn(*l, " \t")] == '#')
                continue;
            for (guint u = 0; u < used->len; u++)
                g_assert_null(strstr(*l, used->pdata[u]));                                   
        }
        g_strfreev(ls);
        g_ptr_array_unref(er); g_ptr_array_unref(used); g_free(res); g_string_free(body, TRUE);
    }
    g_rand_free(rnd);
}

                                                                                 
static void
test_runtime_config_rejects_unparsable(void)
{
    GError *error = NULL;
    g_assert_null(pcv_lxc_runtime_config_text("lxc.uts.name = c1\nlxc.mount.entry = \"/srv/a mnt/a none bind 0 0\n", "/run/x", "c1", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_null(pcv_lxc_runtime_config_text("lxc.mount.entry = '/srv/a mnt/a none bind 0 0\n", "/run/x", "c1", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    static const gchar *const bad_names[] = { NULL, "", ".", "..", "a/b", NULL };
    for (guint i = 0; i < 5; i++) {
        g_assert_null(pcv_lxc_runtime_config_text("lxc.mount.entry = /srv/a mnt/a none bind 0 0\n", "/run/x", bad_names[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
    }
    gchar *ok = pcv_lxc_runtime_config_text("lxc.uts.name = c1\n", "/run/x", "c1", &error);                      
    g_assert_no_error(error);
    g_assert_cmpstr(ok, ==, "lxc.uts.name = c1\n");
    g_free(ok);
}

                                                                                                                      
static void
test_root_gid(void)
{
    gint64 gid = -1;
    g_assert_true(pcv_lxc_config_root_gid("lxc.idmap = u 0 100000 65536\nlxc.idmap = g 0 100000 65536\n", &gid));
    g_assert_cmpint(gid, ==, 100000);
    g_assert_true(pcv_lxc_config_root_gid("  lxc.idmap\t=\tg\t0\t231072\t65536\r\n", &gid));
    g_assert_cmpint(gid, ==, 231072);
    g_assert_false(pcv_lxc_config_root_gid("", &gid));
    g_assert_false(pcv_lxc_config_root_gid(NULL, &gid));
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmap = u 0 100000 65536\n", &gid));                
    g_assert_false(pcv_lxc_config_root_gid("# lxc.idmap = g 0 100000 65536\n", &gid));              
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmap = g 0 100000 0\n", &gid));                         
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmap = g 1000 100000 10\n", &gid));                                
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmap = g 0 abc 10\n", &gid));
    g_assert_false(pcv_lxc_config_root_gid("lxc.idmapx = g 0 100000 65536\n", &gid));
}

                                                                                                  
static int
hook_case(const gchar *t, const gchar *root, const gchar *name, const gchar *body)
{
    gchar *cfg = g_build_filename(t, "hook.cfg", NULL);
    g_assert_true(g_file_set_contents(cfg, body, -1, NULL));
    int rc = pcv_lxc_volume_guard_hook_run_with_root(root, cfg, name);
    g_assert_cmpint(g_remove(cfg), ==, 0);
    g_free(cfg);
    return rc;
}

                                                                                        
static void
test_hook_staging_prefix_table(void)
{
    gchar *t = fixture_new();
    gchar *vs = g_build_filename(t, "vs", NULL);
    gchar *c1 = g_build_filename(vs, "c1", NULL), *c2 = g_build_filename(vs, "c2", NULL);
    gchar *k0 = g_build_filename(c1, "k.0", NULL), *k1 = g_build_filename(c1, "k.1", NULL), *lnk = g_build_filename(c1, "lnk", NULL);
    gchar *o0 = g_build_filename(c2, "k.0", NULL), *other = g_build_filename(t, "other", NULL), *real = g_build_filename(t, "src", "a", NULL);
    g_assert_cmpint(g_mkdir_with_parents(k0, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(k1, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(o0, 0755), ==, 0);
    g_assert_cmpint(symlink(other, lnk), ==, 0);

    gchar *persistent = g_strdup_printf("lxc.mount.entry = %s mnt/a none bind,ro 0 0\n", real);
    gchar *own = g_strdup_printf("lxc.mount.entry = %s mnt/a none bind,ro 0 0\nlxc.mount.entry = %s mnt/b none rbind 0 0\n", k0, k1);
    gchar *foreign = g_strdup_printf("lxc.mount.entry = %s mnt/a none bind 0 0\n", o0);
    gchar *mixed = g_strdup_printf("lxc.mount.entry = %s mnt/a none bind 0 0\nlxc.mount.entry = %s mnt/b none bind 0 0\n", k0, real);
    gchar *linked = g_strdup_printf("lxc.mount.entry = %s mnt/a none bind 0 0\n", lnk);
    gchar *sibling = g_strdup_printf("lxc.mount.entry = %s/c1x/k.0 mnt/a none bind 0 0\n", vs);                                
    const gchar *novol = "lxc.uts.name = c1\nlxc.mount.entry = proc proc proc nodev 0 0\n";
    gchar *dotdot = g_strdup_printf("lxc.mount.entry = %s/../c2/k.0 mnt/a none bind 0 0\n", c1);                          
    gchar *bare = g_strdup_printf("lxc.mount.entry = %s/ mnt/a none bind 0 0\n", c1);                                       
    const gchar *relbind = "lxc.mount.entry = srv/data mnt/data none bind 0 0\n";                                            
    const gchar *relrbind = "lxc.mount.entry = srv/data mnt/data none rbind,ro 0 0\n";
    const gchar *relnon = "lxc.mount.entry = proc proc proc nodev 0 0\nlxc.mount.entry = rel mnt/r none ro 0 0\n";                          

    g_assert_cmpint(hook_case(t, vs, "c1", persistent), ==, 1);                        
    g_assert_cmpint(hook_case(t, vs, "c1", own), ==, 0);                                
    g_assert_cmpint(hook_case(t, vs, "c1", foreign), ==, 1);                      
    g_assert_cmpint(hook_case(t, vs, "c1", mixed), ==, 1);                           
    g_assert_cmpint(hook_case(t, vs, "c1", novol), ==, 0);                     
    g_assert_cmpint(hook_case(t, vs, "c1", linked), ==, 1);                            
    g_assert_cmpint(hook_case(t, vs, "c1", sibling), ==, 1);
    g_assert_cmpint(hook_case(t, vs, "c1", dotdot), ==, 1);
    g_assert_cmpint(hook_case(t, vs, "c1", bare), ==, 0);
    g_assert_cmpint(hook_case(t, vs, "c1", relbind), ==, 1);
    g_assert_cmpint(hook_case(t, vs, "c1", relrbind), ==, 1);
    g_assert_cmpint(hook_case(t, vs, "c1", relnon), ==, 0);
    g_assert_cmpint(hook_case(t, vs, "../x", own), ==, 1);                            
    g_assert_cmpint(hook_case(t, vs, "", own), ==, 1);
    g_assert_cmpint(hook_case(t, vs, NULL, own), ==, 1);
    g_assert_cmpint(hook_case(t, vs, "c2", own), ==, 1);                                     

    g_assert_cmpint(g_remove(lnk), ==, 0);
    g_assert_cmpint(g_rmdir(k0), ==, 0);
    g_assert_cmpint(g_rmdir(k1), ==, 0);
    g_assert_cmpint(g_rmdir(o0), ==, 0);
    g_assert_cmpint(g_rmdir(c1), ==, 0);
    g_assert_cmpint(g_rmdir(c2), ==, 0);
    g_assert_cmpint(g_rmdir(vs), ==, 0);
    g_free(vs); g_free(c1); g_free(c2); g_free(k0); g_free(k1); g_free(lnk); g_free(o0); g_free(other); g_free(real);
    g_free(persistent); g_free(own); g_free(foreign); g_free(mixed); g_free(linked); g_free(sibling); g_free(dotdot); g_free(bare);
    fixture_free(t);
}

                                                                                         
                                                                         
#define VOLGEN_RESET_FAILED "(volume guard fingerprint could not be reset)"
static void
test_check_start_clears_old_volgen_only(void)
{
    gchar *t = fixture_new();
    gchar *text = g_strdup_printf("lxc.mount.entry = %s/src/a mnt/a none bind,ro 0 0\n", t);
    gchar *lxc = config_put(t, text);
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    gchar *volgen = g_build_filename(lxc, "vg", PCV_LXC_VOLUME_GUARD_VOLGEN, NULL);
    gchar *bad = NULL;
    g_assert_true(g_file_set_contents(volgen, "stale\n", -1, NULL));
    g_assert_true(pcv_lxc_volume_guard_check_start(lxc, "vg", &bad));
    g_assert_null(bad);
    g_assert_false(g_file_test(volgen, G_FILE_TEST_EXISTS));                              
    g_assert_true(pcv_lxc_volume_guard_check_start(lxc, "vg", NULL));                     
    g_assert_false(g_file_test(volgen, G_FILE_TEST_EXISTS));

                                          
    gchar *linked = g_strdup_printf("%slxc.mount.entry = %s/src/esc mnt/e none bind 0 0\n", text, t);
    g_assert_true(g_file_set_contents(cfg, linked, -1, NULL));
    g_assert_true(g_file_set_contents(volgen, "stale\n", -1, NULL));
    g_assert_false(pcv_lxc_volume_guard_check_start(lxc, "vg", &bad));
    g_assert_cmpstr(bad, ==, "/mnt/e");
    g_clear_pointer(&bad, g_free);
    g_assert_false(g_file_test(volgen, G_FILE_TEST_EXISTS));

                                         
    gchar *lxc2 = g_build_filename(t, "lxc2", NULL);
    g_assert_true(pcv_lxc_volume_guard_check_start(lxc2, "vg", NULL));
    g_assert_false(g_file_test(lxc2, G_FILE_TEST_EXISTS));

                                               
    g_assert_true(g_file_set_contents(cfg, text, -1, NULL));
    gchar *alias = g_build_filename(lxc, "alias", NULL);
    gchar *vgdir = g_build_filename(lxc, "vg", NULL);
    gchar *got = NULL;
    g_assert_cmpint(symlink(vgdir, alias), ==, 0);
    g_assert_true(g_file_set_contents(volgen, "stale\n", -1, NULL));
    g_assert_false(pcv_lxc_volume_guard_check_start(lxc, "alias", &bad));
    g_assert_cmpstr(bad, ==, VOLGEN_RESET_FAILED);                  
    g_clear_pointer(&bad, g_free);
    g_assert_true(g_file_get_contents(volgen, &got, NULL, NULL));
    g_assert_cmpstr(got, ==, "stale\n");                                    
    g_clear_pointer(&got, g_free);
    g_assert_cmpint(g_remove(alias), ==, 0);

                                              
    g_assert_cmpint(g_remove(volgen), ==, 0);
    g_assert_cmpint(g_mkdir(volgen, 0700), ==, 0);
    g_assert_false(pcv_lxc_volume_guard_check_start(lxc, "vg", &bad));
    g_assert_cmpstr(bad, ==, VOLGEN_RESET_FAILED);
    g_clear_pointer(&bad, g_free);
    g_assert_cmpint(g_rmdir(volgen), ==, 0);

                                                                             
    if (geteuid() != 0) {
        g_assert_true(g_file_set_contents(volgen, "stale\n", -1, NULL));
        g_assert_cmpint(g_chmod(vgdir, 0555), ==, 0);
        g_assert_false(pcv_lxc_volume_guard_check_start(lxc, "vg", &bad));
        g_assert_cmpstr(bad, ==, VOLGEN_RESET_FAILED);
        g_clear_pointer(&bad, g_free);
        g_assert_cmpint(g_chmod(vgdir, 0755), ==, 0);
        g_assert_cmpint(g_remove(volgen), ==, 0);
    }

    config_drop(lxc);
    g_free(alias); g_free(vgdir); g_free(lxc2); g_free(linked); g_free(volgen); g_free(cfg); g_free(text);
    fixture_free(t);
}

                                                                                                             
                                                                                           
                                                                                            
                                                                               
static void
test_config_save_write_failure_keeps_config(void)
{
    if (g_test_subprocess()) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl = { 0, 0 };
        g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &rl), ==, 0);
        GError *error = NULL;
        gboolean ok = pcv_lxc_volume_config_save(g_getenv("PCV_TEST_VOLSAVE_LXC"), "vg", "/srv/a mnt/a none bind,ro 0 0", &error);
        exit(!ok && error != NULL ? 0 : 1);
    }
    gchar *t = fixture_new();
    const gchar *orig = "lxc.uts.name = vg\nlxc.net.0.type = veth\n";
    gchar *lxc = config_put(t, orig);
    gchar *cfg = g_build_filename(lxc, "vg", "config", NULL);
    g_setenv("PCV_TEST_VOLSAVE_LXC", lxc, TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();                                             
    g_unsetenv("PCV_TEST_VOLSAVE_LXC");
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, orig);                              
    gchar *dir = g_build_filename(lxc, "vg", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(d);
    for (const gchar *n; (n = g_dir_read_name(d)) != NULL;)
        g_assert_cmpstr(n, ==, "config");                                     
    g_dir_close(d);
    g_free(dir); g_free(text); g_free(cfg);
    config_drop(lxc);
    fixture_free(t);
}

                                                                                  
                                                                  
static void
test_forget_clears_volgen(void)
{
    gchar *t = fixture_new();
    gchar *lxc = config_put(t, "");
    gchar *volgen = g_build_filename(lxc, "vg", PCV_LXC_VOLUME_GUARD_VOLGEN, NULL);
    g_assert_true(g_file_set_contents(volgen, "fp\n", -1, NULL));
    g_assert_true(pcv_lxc_volume_guard_forget(lxc, "vg"));
    g_assert_false(g_file_test(volgen, G_FILE_TEST_EXISTS));
    g_assert_true(pcv_lxc_volume_guard_forget(lxc, "vg"));                      
    g_assert_true(pcv_lxc_volume_guard_forget(lxc, "missing"));                      

    gchar *alias = g_build_filename(lxc, "alias", NULL);
    gchar *vgdir = g_build_filename(lxc, "vg", NULL);
    g_assert_cmpint(symlink(vgdir, alias), ==, 0);
    g_assert_true(g_file_set_contents(volgen, "fp\n", -1, NULL));
    g_assert_false(pcv_lxc_volume_guard_forget(lxc, "alias"));                        
    g_assert_true(g_file_test(volgen, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_remove(alias), ==, 0);
    g_assert_cmpint(g_remove(volgen), ==, 0);

    config_drop(lxc);
    g_free(alias); g_free(vgdir); g_free(volgen);
    fixture_free(t);
}

void
test_lxc_volume_guard_register(void)
{
    g_test_add_func("/lxc_volume_guard/source_open_walks_without_links", test_source_open_walks_without_links);
    g_test_add_func("/lxc_volume_guard/source_open_rejects_magic_links", test_source_open_rejects_magic_links);
    g_test_add_func("/lxc_volume_guard/parse_mount_entries", test_parse_mount_entries);
    g_test_add_func("/lxc_volume_guard/check_passes_real_sources", test_check_passes_real_sources);
    g_test_add_func("/lxc_volume_guard/check_rejects_link_swap", test_check_rejects_link_swap);
    g_test_add_func("/lxc_volume_guard/check_leaves_missing_source_to_liblxc", test_check_leaves_missing_source_to_liblxc);
    g_test_add_func("/lxc_volume_guard/check_missing_config_ok", test_check_missing_config_ok);
    g_test_add_func("/lxc_volume_guard/hook_main_exit_codes", test_hook_main_exit_codes);
    g_test_add_func("/lxc_volume_guard/hook_main_stderr", test_hook_main_stderr);
    g_test_add_func("/lxc_volume_guard/with_guard_hook", test_with_guard_hook);
    g_test_add_func("/lxc_volume_guard/config_save_adds_hook_once", test_config_save_adds_hook_once);
    g_test_add_func("/lxc_volume_guard/hook_installed", test_hook_installed);
    g_test_add_func("/lxc_volume_guard/volstage_slot_deterministic", test_volstage_slot_deterministic);
    g_test_add_func("/lxc_volume_guard/dest_canon", test_dest_canon);
    g_test_add_func("/lxc_volume_guard/runtime_config_replaces_sources", test_runtime_config_replaces_sources);
    g_test_add_func("/lxc_volume_guard/runtime_config_no_host_source", test_runtime_config_no_host_source);
    g_test_add_func("/lxc_volume_guard/runtime_config_rejects_unparsable", test_runtime_config_rejects_unparsable);
    g_test_add_func("/lxc_volume_guard/root_gid", test_root_gid);
    g_test_add_func("/lxc_volume_guard/hook_staging_prefix_table", test_hook_staging_prefix_table);
    g_test_add_func("/lxc_volume_guard/check_start_clears_old_volgen_only", test_check_start_clears_old_volgen_only);
    g_test_add_func("/lxc_volume_guard/forget_clears_volgen", test_forget_clears_volgen);
    g_test_add_func("/lxc_volume_guard/config_save_write_failure_keeps_config", test_config_save_write_failure_keeps_config);
}
