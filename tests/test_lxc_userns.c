                                                                                  
                                                                                         
                                                                                                                          
                                                                                      
                                                                                
                                                                                               
                                                                           
                                                                       
                                                                                                      
                                                                           
                                                                      
                                                               
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include "modules/lxc/lxc_userns.h"
#include "modules/lxc/lxc_volume_guard.h"
#include "utils/pcv_config.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_privdrop.h"

typedef struct { gchar *dir; GPtrArray *files; } SubidDir;

static SubidDir
subid_dir_new(void)
{
    GError *error = NULL;
    SubidDir d = { g_dir_make_tmp("pcv-z17-userns-XXXXXX", &error), g_ptr_array_new_with_free_func(g_free) };
    g_assert_no_error(error);
    return d;
}

static const gchar *
subid_put(SubidDir *d, const gchar *name, const gchar *text)
{
    gchar *path = g_build_filename(d->dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_ptr_array_add(d->files, path);
    return path;
}

static void
subid_dir_free(SubidDir *d)
{
    for (guint i = 0; i < d->files->len; i++)
        g_unlink(g_ptr_array_index(d->files, i));
    g_rmdir(d->dir);
    g_ptr_array_unref(d->files);
    g_free(d->dir);
}

#define HC(u, g) pcv_lxc_userns_host_check((u), (g), 100000, 65536)

static void
test_host_root_delegation(void)
{
    SubidDir d = subid_dir_new();
    const gchar *ok = subid_put(&d, "ok", "root:100000:65536\n");
    const gchar *zero = subid_put(&d, "zero", "0:100000:65536\n");
    const gchar *wide = subid_put(&d, "wide", "root:65536:1000000\n");
    const gchar *part = subid_put(&d, "part", "root:100000:65535\n");                            
    const gchar *user = subid_put(&d, "user", "z17acct:200000:65536\n");                               
    const gchar *noise = subid_put(&d, "noise", "\n# comment\nbad line\nroot:x:65536\nroot:1:0\nroot:100000:65536\n");
    const gchar *empty = subid_put(&d, "empty", "");
    gchar *missing = g_build_filename(d.dir, "absent", NULL);

    g_assert_cmpint(HC(ok, ok), ==, PCV_LXC_USERNS_READY);
    g_assert_cmpint(HC(zero, zero), ==, PCV_LXC_USERNS_READY);                                     
    g_assert_cmpint(HC(wide, ok), ==, PCV_LXC_USERNS_READY);
    g_assert_cmpint(HC(noise, noise), ==, PCV_LXC_USERNS_READY);                                            
    g_assert_cmpint(HC(part, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(HC(ok, part), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);                              
    g_assert_cmpint(HC(user, user), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);                                           
    g_assert_cmpint(HC(empty, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(HC(missing, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(HC(ok, missing), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(pcv_lxc_userns_host_check(NULL, ok, 100000, 65536), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_free(missing);
    subid_dir_free(&d);
}

static void
test_host_overlap_boundaries(void)
{
    SubidDir d = subid_dir_new();
    const gchar *root = subid_put(&d, "root", "root:100000:65536\n");
    const gchar *lh = subid_put(&d, "lh", "hardcoremonk:100000:65536\n");                                           
    const gchar *last = subid_put(&d, "last", "root:100000:65536\nz17acct:165535:10\n");                       
    const gchar *first = subid_put(&d, "first", "z17acct:99999:2\nroot:100000:65536\n");                     
    const gchar *after = subid_put(&d, "after", "root:100000:65536\nz17acct:165536:65536\n");
    const gchar *before = subid_put(&d, "before", "z17acct:34464:65536\nroot:100000:65536\n");                           
    const gchar *rootdup = subid_put(&d, "rootdup", "root:100000:65536\n0:120000:10\n");                          
    const gchar *empty_acct = subid_put(&d, "empty_acct", "root:100000:65536\nz17acct:120000:0\n");                

    g_assert_cmpint(HC(empty_acct, empty_acct), ==, PCV_LXC_USERNS_READY);                                                 
    g_assert_cmpint(HC(lh, lh), ==, PCV_LXC_USERNS_OVERLAP);                                                      
    g_assert_cmpint(HC(last, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(root, last), ==, PCV_LXC_USERNS_OVERLAP);                                  
    g_assert_cmpint(HC(first, first), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(after, after), ==, PCV_LXC_USERNS_READY);                                   
    g_assert_cmpint(HC(before, before), ==, PCV_LXC_USERNS_READY);                        
    g_assert_cmpint(HC(rootdup, rootdup), ==, PCV_LXC_USERNS_READY);
    subid_dir_free(&d);
}

                                                                                                   
                                                                                
                                                           
static void
test_host_numbers_like_shadow(void)
{
    SubidDir d = subid_dir_new();
    const gchar *root = subid_put(&d, "root", "root:100000:65536\n");
    const gchar *hex = subid_put(&d, "hex", "root:100000:65536\nz17acct:0x186a0:65536\n");                     
    const gchar *oct = subid_put(&d, "oct", "root:100000:65536\nz17acct:0303240:65536\n");                    
    const gchar *sp = subid_put(&d, "sp", "root:100000:65536\nz17acct: 100000:65536\n");                 
    const gchar *plus = subid_put(&d, "plus", "root:100000:65536\nz17acct:+100000:65536\n");             
    const gchar *cnt = subid_put(&d, "cnt", "root:100000:65536\nz17acct:99999:\t0x2\n");                         
    const gchar *negz = subid_put(&d, "negz", "root:100000:65536\nz17acct:-0:200000\n");                                 
    const gchar *root_oct = subid_put(&d, "root_oct", "root:0100000:65536\n");                                          
    const gchar *root_hex = subid_put(&d, "root_hex", "root:0x186A0:0x10000\n");                                     
                                                                       
    const gchar *junk = subid_put(&d, "junk",
                                  "root:100000:65536\n"
                                  "z17acct:-100000:65536\n"               
                                  "z17acct:100000 :65536\n"                         
                                  "z17acct:100000x:65536\n"
                                  "z17acct:0x186a0z:65536\n"
                                  "z17acct:1e5:65536\n"
                                  "z17acct:+-100000:65536\n"                
                                  "z17acct:+ 100000:65536\n"                   
                                  "z17acct:0x:200000\n"                          
                                  "z17acct:099999:65536\n"                       
                                  "z17acct:99999:-65536\n");

    g_assert_cmpint(HC(hex, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(oct, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(sp, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(plus, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(root, cnt), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(negz, root), ==, PCV_LXC_USERNS_OVERLAP);
    g_assert_cmpint(HC(root_oct, root_oct), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(HC(root_hex, root_hex), ==, PCV_LXC_USERNS_READY);
    g_assert_cmpint(HC(junk, junk), ==, PCV_LXC_USERNS_READY);
    subid_dir_free(&d);
}

                                                                                        
                                                                                        
                                                                               
static void
test_host_other_lines_like_shadow_4174(void)
{
    SubidDir d = subid_dir_new();
    const gchar *root = subid_put(&d, "root", "root:100000:65536\n");
    const gchar *const wide[] = {
        "z17acct:100000:4294967295",                                         
        "z17acct:99999:4294967295",
        "z17acct:99999:4294967296",                            
        "z17acct:100000:65536:x",                                 
        "z17acct:165535:1:",                                         
        "z17acct:0:0",                                                                
        "z17acct:0:18446744073709551615",                         
        NULL };
    for (guint i = 0; wide[i]; i++) {
        g_autofree gchar *name = g_strdup_printf("wide%u", i);
        g_autofree gchar *text = g_strdup_printf("root:100000:65536\n%s\n", wide[i]);
        const gchar *p = subid_put(&d, name, text);
        g_test_message("other line: %s", wide[i]);
        g_assert_cmpint(HC(p, root), ==, PCV_LXC_USERNS_OVERLAP);
        g_assert_cmpint(HC(root, p), ==, PCV_LXC_USERNS_OVERLAP);
    }
                                        
    const gchar *far = subid_put(&d, "far",
                                 "root:100000:65536\n"
                                 "z17acct:4295067296:65536\n"                                            
                                 "z17acct:165536:4294967295\n"                                
                                 "z17acct:18446744073709551615:2\n"                               
                                 "z17acct:18446744073709551616:1\n"                             
                                 "z17acct:1:0\n");                                              
    g_assert_cmpint(HC(far, far), ==, PCV_LXC_USERNS_READY);
    subid_dir_free(&d);
}

                                                                                          
                                                                                        
                                                         
static void
test_host_root_line_strict(void)
{
    SubidDir d = subid_dir_new();
    const gchar *ok = subid_put(&d, "ok", "root:100000:65536\n");
    const gchar *num_sp = subid_put(&d, "num_sp", "root:\t100000: 65536\n");                            
    g_autofree gchar *pad = g_strnfill(1010, ' ');
    g_autofree gchar *long_root = g_strdup_printf("root:%s100000:65536", pad);                           
    const gchar *const bad[] = {
        "root:100000:65536\r",                                         
        "root:100000:65536 ",                           
        "  root:100000:65536",                                        
        "  0:100000:65536",
        "root:100000:65536:x",                                                   
        "root:0:0",                                                                   
        "root:100000:4294967295",                                               
        long_root,
        NULL };
    g_assert_cmpint(HC(num_sp, num_sp), ==, PCV_LXC_USERNS_READY);
    for (guint i = 0; bad[i]; i++) {
        g_autofree gchar *name = g_strdup_printf("bad%u", i);
        g_autofree gchar *text = g_strdup_printf("%s\n", bad[i]);
        const gchar *p = subid_put(&d, name, text);
        g_test_message("root line: '%s'", bad[i]);
        g_assert_cmpint(HC(p, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);                                
        g_assert_cmpint(HC(ok, p), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    }
                                                                
    const gchar *crlf_acct = subid_put(&d, "crlf_acct", "root:100000:65536\nz17acct:100000:65536\r\n");
    g_assert_cmpint(HC(crlf_acct, ok), ==, PCV_LXC_USERNS_OVERLAP);
    subid_dir_free(&d);
}

                                                             
static void
_warn_capture(const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer user_data)
{
    (void)dom; (void)lvl;
    g_ptr_array_add(user_data, g_strdup(msg));
}

                                                                                     
                                                                         
                                 
static void
test_host_root_line_rejection_warns(void)
{
    SubidDir d = subid_dir_new();
    const gchar *ok = subid_put(&d, "ok", "root:100000:65536\n");
    g_autofree gchar *pad = g_strnfill(1010, ' ');
    g_autofree gchar *long_root = g_strdup_printf("root:%s100000:65536\n", pad);
    static const struct { const gchar *text, *line, *why; } rows[] = {
        { "root:100000:65536\r\n",                 ":1: ", "whitespace" },             
        { "# c\nz17other:300000:65536\nroot:100000:65536 \n", ":3: ", "whitespace" },                       
        { "  0:100000:65536\n",                    ":1: ", "whitespace" },
        { "root:100000:65536:x\n",                 ":1: ", "3 fields" },
        { "root:100000\n",                         ":1: ", "3 fields" },                            
        { "root:x:65536\n",                        ":1: ", "start" },
        { "root:0:0\n",                            ":1: ", "count" },
        { "root:100000:4294967295\n",              ":1: ", "count" },
        { NULL,                                    ":1: ", "1024" },                        
    };
    g_autoptr(GPtrArray) msgs = g_ptr_array_new_with_free_func(g_free);
    guint hid = g_log_set_handler("lxc_userns", G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL, _warn_capture, msgs);
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_autofree gchar *name = g_strdup_printf("w%zu", i);
        const gchar *p = subid_put(&d, name, rows[i].text ? rows[i].text : long_root);
        g_ptr_array_set_size(msgs, 0);
        g_assert_cmpint(HC(p, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
        g_assert_cmpuint(msgs->len, ==, 1);
        const gchar *m = g_ptr_array_index(msgs, 0);
        g_test_message("WARN: %s", m);
        g_autofree gchar *where = g_strconcat(p, rows[i].line, NULL);
        g_assert_true(g_str_has_prefix(m, where));
        g_assert_nonnull(strstr(m, rows[i].why));
        g_assert_null(strstr(m, "z17other"));
        g_assert_null(strstr(m, "100000"));                      
    }
    const gchar *quiet = subid_put(&d, "quiet", "root:200000:65536\nz17other:300000:65536\n");
    g_ptr_array_set_size(msgs, 0);
    g_assert_cmpint(HC(ok, ok), ==, PCV_LXC_USERNS_READY);
    g_assert_cmpint(HC(quiet, ok), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);                         
    g_assert_cmpuint(msgs->len, ==, 0);
    g_log_remove_handler("lxc_userns", hid);
    subid_dir_free(&d);
}

static void
test_host_bad_range(void)
{
    SubidDir d = subid_dir_new();
    const gchar *ok = subid_put(&d, "ok", "root:100000:65536\n");
    const gchar *top = subid_put(&d, "top", "root:4294901759:65536\n");
    const gchar *top_wide = subid_put(&d, "top_wide", "root:4294901759:65537\n");                                 
                                                                             
    const gchar *top_over = subid_put(&d, "top_over", "root:4294901759:65538\n");
    const gchar *eq = subid_put(&d, "eq", "root:65536:65536\n");
    gchar *missing = g_build_filename(d.dir, "absent", NULL);

    g_assert_cmpint(pcv_lxc_userns_host_check(ok, ok, 100000, 65535), ==, PCV_LXC_USERNS_BAD_RANGE);                      
    g_assert_cmpint(pcv_lxc_userns_host_check(ok, ok, 65535, 65536), ==, PCV_LXC_USERNS_BAD_RANGE);                       
    g_assert_cmpint(pcv_lxc_userns_host_check(eq, eq, 65536, 65536), ==, PCV_LXC_USERNS_READY);                                
    g_assert_cmpint(pcv_lxc_userns_host_check(ok, ok, -1, 65536), ==, PCV_LXC_USERNS_BAD_RANGE);
    g_assert_cmpint(pcv_lxc_userns_host_check(top, top, 4294901759, 65536), ==, PCV_LXC_USERNS_READY);                     
    g_assert_cmpint(pcv_lxc_userns_host_check(top, top, 4294901760, 65536), ==, PCV_LXC_USERNS_BAD_RANGE);
    g_assert_cmpint(pcv_lxc_userns_host_check(top_wide, top_wide, 4294901759, 65536), ==, PCV_LXC_USERNS_READY);
    g_assert_cmpint(pcv_lxc_userns_host_check(top_over, top_over, 4294901759, 65536), ==, PCV_LXC_USERNS_NO_ROOT_RANGE);
    g_assert_cmpint(pcv_lxc_userns_host_check(missing, missing, 100000, 1), ==, PCV_LXC_USERNS_BAD_RANGE);              
    g_free(missing);
    subid_dir_free(&d);
}

static void
test_host_message_guides_without_account_names(void)
{
    const gchar *pfx = "unprivileged containers are not available on this host: ";
    gchar *no_root = pcv_lxc_userns_host_message(PCV_LXC_USERNS_NO_ROOT_RANGE, 100000, 65536);
    gchar *overlap = pcv_lxc_userns_host_message(PCV_LXC_USERNS_OVERLAP, 100000, 65536);
    gchar *bad = pcv_lxc_userns_host_message(PCV_LXC_USERNS_BAD_RANGE, 100000, 1000);
    for (gchar **m = (gchar *[]){ no_root, overlap, bad, NULL }; *m; m++) {
        g_assert_true(g_str_has_prefix(*m, pfx));
        g_assert_nonnull(strstr(*m, "rootless:false"));                                        
        g_assert_null(strstr(*m, "hardcoremonk"));
        g_assert_null(strstr(*m, "z17acct"));                                           
    }
    g_assert_nonnull(strstr(no_root, "root:100000:65536"));
    g_assert_nonnull(strstr(no_root, "/etc/subuid and /etc/subgid"));
    g_assert_nonnull(strstr(overlap, "100000-165535"));
    g_assert_nonnull(strstr(overlap, "rootless_uid_start"));
    g_assert_nonnull(strstr(overlap, "root:100000:65536"));                             
    g_assert_nonnull(strstr(overlap, "if you changed rootless_uid_start"));                                 
    g_assert_nonnull(strstr(bad, "rootless_uid_count"));
    g_free(no_root); g_free(overlap); g_free(bad);
}

static void
test_settings_parse_default(void)
{
    gboolean unknown = TRUE;
    g_assert_true(pcv_lxc_userns_parse_default(NULL, &unknown));     g_assert_false(unknown);
    for (const gchar *const *v = (const gchar *const[]){ "true", "TRUE", "1", "yes", " yes ", NULL }; *v; v++) {
        g_assert_true(pcv_lxc_userns_parse_default(*v, &unknown));   g_assert_false(unknown);
    }
    for (const gchar *const *v = (const gchar *const[]){ "false", "0", "no", "No", " false\n", NULL }; *v; v++) {
        g_assert_false(pcv_lxc_userns_parse_default(*v, &unknown));  g_assert_false(unknown);
    }
    for (const gchar *const *v = (const gchar *const[]){ "flase", "", "off", "2", NULL }; *v; v++) {
        g_assert_true(pcv_lxc_userns_parse_default(*v, &unknown));   g_assert_true(unknown);                    
    }
}

                                                                              
                                                                          
static PcvLxcUsernsSettings
settings_with(const gchar *container_body)
{
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("pcv-z17-cfg-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *conf = g_build_filename(dir, "daemon.conf", NULL);
    gchar *body = g_strdup_printf("[container]\n%s", container_body);
    g_assert_true(g_file_set_contents(conf, body, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();
    PcvLxcUsernsSettings s = pcv_lxc_userns_settings_load();
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    g_unlink(conf); g_rmdir(dir);
    g_free(body); g_free(conf); g_free(dir);
    return s;
}

                                                                        
static void
test_settings_load_range(void)
{
    PcvLxcUsernsSettings s = settings_with("rootless = false\n");
    g_assert_cmpint(s.start, ==, 100000);                                    
    g_assert_cmpint(s.count, ==, 65536);
    g_assert_false(s.default_on);
    s = settings_with("rootless_uid_start = 2200000000\nrootless_uid_count = 65536\n");
    g_assert_cmpint(s.start, ==, 2200000000);                                   
    s = settings_with("rootless_uid_start = 4294901759\nrootless_uid_count = 65536\n");
    g_assert_cmpint(s.start, ==, 4294901759);
    s = settings_with("rootless_uid_start = 1000000\nrootless_uid_count = 70000\n");
    g_assert_cmpint(s.start, ==, 1000000);
    g_assert_cmpint(s.count, ==, 70000);
                                                                   
    for (const gchar *const *v = (const gchar *const[]){ "rootless_uid_start = 1_000_000\n",
                                                         "rootless_uid_start = 100000 # comment\n",
                                                         "rootless_uid_start = 0x186a0\n",
                                                         "rootless_uid_start = 4294967296\n",
                                                         "rootless_uid_start = -100000\n",
                                                         "rootless_uid_count = 65536k\n",
                                                         "rootless_uid_count = 99999999999999999999\n", NULL }; *v; v++) {
        s = settings_with(*v);
        g_assert_true(s.start == -1 || s.count == -1);
        g_assert_cmpint(pcv_lxc_userns_host_check(NULL, NULL, s.start, s.count), ==, PCV_LXC_USERNS_BAD_RANGE);
    }
}

                                                                                     
static void
test_resource_lines(void)
{
    gchar *a = pcv_lxc_userns_resource_lines("virbr0", 512, 1);                                             
    g_assert_cmpstr(a, ==, "lxc.cgroup.memory.limit_in_bytes = 512M\nlxc.cgroup2.memory.max = 512M\n"
                           "lxc.cgroup2.cpu.max = 100000 100000\nlxc.net.0.type = veth\nlxc.net.0.link = virbr0\n"
                           "lxc.net.0.flags = up\nlxc.net.0.hwaddr = 00:16:3e:xx:xx:xx\n");
    gchar *b = pcv_lxc_userns_resource_lines("pcvbr-z17", 2048, 4);
    g_assert_nonnull(strstr(b, "\nlxc.cgroup2.memory.max = 2048M\n"));
    g_assert_nonnull(strstr(b, "\nlxc.cgroup2.cpu.max = 400000 100000\n"));
    g_assert_nonnull(strstr(b, "\nlxc.net.0.link = pcvbr-z17\n"));
                                       
    for (const gchar *const *bad = (const gchar *const[]){ "br\nlxc.hook.pre-start = /x", "br\r", "br=x", "br#x", "", NULL };
         *bad; bad++)
        g_assert_null(pcv_lxc_userns_resource_lines(*bad, 512, 1));
    g_assert_null(pcv_lxc_userns_resource_lines(NULL, 512, 1));
    g_free(a); g_free(b);
}

static void
test_build_create_config(void)
{
    gboolean dropped = FALSE;
    gchar *res = pcv_lxc_userns_resource_lines("virbr0", 512, 1);
                                                                                                
                                                                                   
    gchar *a = pcv_lxc_userns_build_create_config(
        "lxc.net.0.type=veth\nlxc.net.0.link = lxcbr0\n  lxc.idmap = u 0 1 1\nlxc.id_map = g 0 1 1\n"
        "# lxc.net.0.link = keep\n lxc.net.0.flags = up\nlxc.apparmor.profile = generated", 100000, 65536, res, &dropped);
    g_assert_true(dropped);
    g_assert_cmpstr(a, ==, "# lxc.net.0.link = keep\nlxc.apparmor.profile = generated\n"
                           "lxc.cgroup.memory.limit_in_bytes = 512M\nlxc.cgroup2.memory.max = 512M\n"
                           "lxc.cgroup2.cpu.max = 100000 100000\nlxc.net.0.type = veth\nlxc.net.0.link = virbr0\n"
                           "lxc.net.0.flags = up\nlxc.net.0.hwaddr = 00:16:3e:xx:xx:xx\n"
                           "lxc.idmap = u 0 100000 65536\nlxc.idmap = g 0 100000 65536\n"
                           "lxc.init.uid = 0\nlxc.init.gid = 0\n");
    gchar *b = pcv_lxc_userns_build_create_config(NULL, 200000, 70000, NULL, &dropped);                                 
    g_assert_false(dropped);
    g_assert_cmpstr(b, ==, "lxc.idmap = u 0 200000 70000\nlxc.idmap = g 0 200000 70000\nlxc.init.uid = 0\nlxc.init.gid = 0\n");
    gchar *c = pcv_lxc_userns_build_create_config("lxc.idmapx = 1\n", 100000, 65536, "x = 1", &dropped);                     
    g_assert_false(dropped);
    g_assert_true(g_str_has_prefix(c, "lxc.idmapx = 1\nx = 1\nlxc.idmap = u "));                                            
    g_free(a); g_free(b); g_free(c); g_free(res);
}

                                                     
static gchar *
edits_apply(const gchar *text, const PcvLxcConfigEdit *edits, gsize n)
{
    GError *error = NULL;
    gchar *out = pcv_lxc_userns_config_apply_edits(text, edits, n, &error);
    g_assert_no_error(error);
    g_assert_nonnull(out);
    return out;
}

                                                                                    
static void
test_config_apply_edits(void)
{
    const gchar *base = "lxc.net.0.type = veth\n# lxc.net.0.link = x\nlxc.net.0.link=lxcbr0\n  lxc.net.0.link = old\n"
                        "lxc.net.1.type = veth\nlxc.net.1.link = br1\nlxc.net.10.type = veth\n\nlxc.include = /a/common.conf\n";
                                                                         
    const PcvLxcConfigEdit set_link[] = { { "lxc.net.0.link", "pcvbr0", FALSE } };
    gchar *a = edits_apply(base, set_link, 1);
    g_assert_cmpstr(a, ==, "lxc.net.0.type = veth\n# lxc.net.0.link = x\nlxc.net.1.type = veth\nlxc.net.1.link = br1\n"
                           "lxc.net.10.type = veth\n\nlxc.include = /a/common.conf\nlxc.net.0.link = pcvbr0\n");
                                                                                                
    const PcvLxcConfigEdit net1[] = { { "lxc.net.1.", NULL, FALSE } };
    gchar *b = edits_apply(base, net1, 1);
    g_assert_cmpstr(b, ==, "lxc.net.0.type = veth\n# lxc.net.0.link = x\nlxc.net.0.link=lxcbr0\n  lxc.net.0.link = old\n"
                           "lxc.net.10.type = veth\n\nlxc.include = /a/common.conf\n");
    const PcvLxcConfigEdit del[] = { { "lxc.net.10.type", NULL, FALSE } };
    gchar *c = edits_apply("lxc.net.10.type = veth\nlxc.net.1.type = veth\n", del, 1);
    g_assert_cmpstr(c, ==, "lxc.net.1.type = veth\n");
                                                                 
    const PcvLxcConfigEdit two[] = { { "a.b", "1", FALSE }, { "a.b", "2", FALSE } };
    gchar *d = edits_apply("x = 1\na.b = 0", two, 2);
    g_assert_cmpstr(d, ==, "x = 1\na.b = 2\n");
    gchar *e = edits_apply(NULL, set_link, 1);
    g_assert_cmpstr(e, ==, "lxc.net.0.link = pcvbr0\n");
                                                                                          
    const PcvLxcConfigEdit inc[] = { { "lxc.include", "/usr/share/lxc/config/userns.conf", TRUE } };
    gchar *f = edits_apply("lxc.include = /a/common.conf", inc, 1);
    g_assert_cmpstr(f, ==, "lxc.include = /a/common.conf\nlxc.include = /usr/share/lxc/config/userns.conf\n");
                      
    static const PcvLxcConfigEdit bad[][1] = {
        { { "lxc.net.1.link", "br\nlxc.hook.pre-start = /x", FALSE } }, { { "lxc.net.1.link\nlxc.x", "br", FALSE } },
        { { "lxc.net.1.link", "br\r", FALSE } }, { { "", "br", FALSE } }, { { NULL, "br", FALSE } },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *error = NULL;
        g_assert_null(pcv_lxc_userns_config_apply_edits(base, bad[i], 1, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
    g_free(a); g_free(b); g_free(c); g_free(d); g_free(e); g_free(f);
}

                                                                                                 
                                                                                        
static void
test_config_apply_edits_rejects_control_chars(void)
{
    static const PcvLxcConfigEdit bad[][1] = {
        { { "lxc.net.1.hwaddr", "00:16:3e:00:00:01\t", FALSE } }, { { "lxc.net.1.link", "br\x01", FALSE } },
        { { "lxc.net.1.link", "br\x7f", FALSE } },                { { "lxc.net.1.link\t", "br", FALSE } },
        { { "lxc.net.1\x1b.link", "br", FALSE } },                { { "lxc.net.1.link", "\x1f", FALSE } },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *error = NULL;
        g_assert_null(pcv_lxc_userns_config_apply_edits("lxc.net.0.type = veth\n", bad[i], 1, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
    const PcvLxcConfigEdit ok[] = { { "lxc.mount.entry", "/srv/a mnt/a none bind,ro 0 0", TRUE },
                                    { "lxc.environment", "K=v w#x~", TRUE } };
    gchar *out = edits_apply(NULL, ok, G_N_ELEMENTS(ok));
    g_assert_cmpstr(out, ==, "lxc.mount.entry = /srv/a mnt/a none bind,ro 0 0\nlxc.environment = K=v w#x~\n");
    g_free(out);
}

static void
test_idmap_and_include(void)
{
    g_assert_true(pcv_lxc_userns_idmap_complete("u 0 100000 65536\ng 0 100000 65536"));
    g_assert_true(pcv_lxc_userns_idmap_complete("g 0 1 1\nu 0 2 2\n"));
    g_assert_false(pcv_lxc_userns_idmap_complete("u 0 100000 65536"));
    g_assert_false(pcv_lxc_userns_idmap_complete("u 0 100000 0\ng 0 100000 65536"));
    g_assert_false(pcv_lxc_userns_idmap_complete(""));
    g_assert_false(pcv_lxc_userns_idmap_complete(NULL));
    g_assert_true(pcv_lxc_userns_config_has_include("lxc.include = /usr/share/lxc/config/ubuntu.userns.conf\n"));
    g_assert_true(pcv_lxc_userns_config_has_include("x = 1\n  lxc.include=/usr/share/lxc/config/userns.conf"));
    g_assert_false(pcv_lxc_userns_config_has_include("lxc.include = /usr/share/lxc/config/ubuntu.common.conf\n"));
    g_assert_false(pcv_lxc_userns_config_has_include("# lxc.include = /usr/share/lxc/config/userns.conf\n"));
    g_assert_false(pcv_lxc_userns_config_has_include(NULL));
}

static void
test_classify_and_shift(void)
{
    g_assert_cmpint(pcv_lxc_userns_classify_id(100000, 100000, 65536), ==, PCV_LXC_ID_MAPPED);
    g_assert_cmpint(pcv_lxc_userns_classify_id(165535, 100000, 65536), ==, PCV_LXC_ID_MAPPED);
    g_assert_cmpint(pcv_lxc_userns_classify_id(165536, 100000, 65536), ==, PCV_LXC_ID_OUTSIDE);
    g_assert_cmpint(pcv_lxc_userns_classify_id(0, 100000, 65536), ==, PCV_LXC_ID_UNSHIFTED);
    g_assert_cmpint(pcv_lxc_userns_classify_id(65535, 100000, 65536), ==, PCV_LXC_ID_UNSHIFTED);
    g_assert_cmpint(pcv_lxc_userns_classify_id(65536, 100000, 65536), ==, PCV_LXC_ID_OUTSIDE);
    g_assert_cmpint(pcv_lxc_userns_classify_id(99999, 100000, 65536), ==, PCV_LXC_ID_OUTSIDE);
    g_assert_cmpint(pcv_lxc_userns_shift_id(0, 100000, 65536), ==, 100000);
    g_assert_cmpint(pcv_lxc_userns_shift_id(65534, 100000, 65536), ==, 165534);               
    g_assert_cmpint(pcv_lxc_userns_shift_id(100042, 100000, 65536), ==, 100042);                    
}

static void
test_getcap_text(void)
{
    gchar *a = pcv_lxc_userns_getcap_text("/r/usr/bin/ping cap_net_raw=ep\n", "/r/usr/bin/ping");                       
    gchar *b = pcv_lxc_userns_getcap_text("/r/usr/bin/ping = cap_net_raw+ep\n", "/r/usr/bin/ping");             
    gchar *c = pcv_lxc_userns_getcap_text("/r/a b =ep\n", "/r/a b");                                                
    g_assert_cmpstr(a, ==, "cap_net_raw=ep");
    g_assert_cmpstr(b, ==, "cap_net_raw+ep");
    g_assert_cmpstr(c, ==, "=ep");
    g_assert_null(pcv_lxc_userns_getcap_text("", "/r/x"));
    g_assert_null(pcv_lxc_userns_getcap_text("/r/y cap_chown=ep\n", "/r/x"));
    g_assert_null(pcv_lxc_userns_getcap_text("/r/x\n", "/r/x"));
    g_assert_null(pcv_lxc_userns_getcap_text("/r/x cap_a=ep\n/r/x cap_b=ep\n", "/r/x"));                        
    g_free(a); g_free(b); g_free(c);
}

                                                                                  
                                                                                 
static void
test_mountinfo_fields(void)
{
    const gchar *t =
        "22 1 0:21 / / rw,relatime shared:1 - zfs rpool/ROOT rw,xattr\n"
        "36 22 0:50 /vol /mnt/vro ro,nodev,relatime master:7 - zfs rpool/vol rw,xattr\n"
        "37 22 0:50 /vol /mnt/vrw rw,nodev,relatime master:7 - zfs rpool/vol ro,xattr\n"
        "38 22 0:51 /mnt/other /mnt/vro2 rw,relatime - tmpfs tmpfs ro\n"
        "39 22 0:52 / /srv rw,nosuid,relatime - ext4 /dev/sda1 ro,errors=remount-ro\n"
        "43 22 0:56 / /mnt/tok rw,relatime,errors=remount-ro - tmpfs tmpfs rw\n";                     
    gboolean found = FALSE;
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt/vro", &found));
    g_assert_true(found);
    found = FALSE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/mnt/vrw", &found));                             
    g_assert_true(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/srv", &found));
    g_assert_true(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/mnt/other", &found));                            
    g_assert_false(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/mnt", &found));                     
    g_assert_false(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/", &found));
    g_assert_true(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/mnt/tok", &found));                                 
    g_assert_true(found);
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt/vro", NULL));                         
}

                                                                    
static void
test_mountinfo_escape_and_lexical(void)
{
    const gchar *t =
        "22 1 0:21 / / rw,relatime shared:1 - zfs rpool/ROOT rw\n"
        "40 22 0:53 / /mnt/a\\040b ro,relatime - tmpfs tmpfs rw\n"
        "41 22 0:54 / /mnt/t\\011x\\134y ro,relatime - tmpfs tmpfs rw\n"
        "42 22 0:55 / /mnt/vro ro,relatime - tmpfs tmpfs rw";                              
    gboolean found = FALSE;
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt/a b", &found));
    g_assert_true(found);
    g_assert_false(pcv_lxc_mountinfo_is_readonly(t, "/mnt/a\\040b", &found));                          
    g_assert_false(found);
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt/t\tx\\y", &found));
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt/vro/", &found));
    g_assert_true(pcv_lxc_mountinfo_is_readonly(t, "/mnt//./vro", &found));
    g_assert_true(found);
}

                                               
static void
test_mountinfo_stacked_last_line(void)
{
    const gchar *rw_last =
        "36 22 0:50 /vol /mnt/vro ro,relatime master:7 - zfs rpool/vol rw\n"
        "50 36 0:60 / /mnt/vro rw,relatime - tmpfs tmpfs rw\n";
    const gchar *ro_last =
        "36 22 0:50 /vol /mnt/vro rw,relatime master:7 - zfs rpool/vol rw\n"
        "50 36 0:60 / /mnt/vro ro,nosuid,relatime - tmpfs tmpfs rw\n";
    gboolean found = FALSE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly(rw_last, "/mnt/vro", &found));
    g_assert_true(found);
    found = FALSE;
    g_assert_true(pcv_lxc_mountinfo_is_readonly(ro_last, "/mnt/vro", &found));
    g_assert_true(found);
}

                                                               
static void
test_mountinfo_absent_and_invalid(void)
{
    gboolean found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly("22 1 0:21 / / rw - zfs r rw\n", "/mnt/vro", &found));
    g_assert_false(found);
    found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly("", "/mnt/vro", &found));
    g_assert_false(found);
    found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly(NULL, "/mnt/vro", &found));
    g_assert_false(found);
    found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly("36 22 0:50 /vol /mnt/vro ro - zfs r rw\n", NULL, &found));
    g_assert_false(found);
    found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly("36 22 0:50 /vol mnt/vro ro - zfs r rw\n", "mnt/vro", &found));
    g_assert_false(found);
    found = TRUE;
    g_assert_false(pcv_lxc_mountinfo_is_readonly("36 22 0:50 /vol /mnt/vro\n", "/mnt/vro", &found));              
    g_assert_false(found);
}

                                                                                
static void
test_rootfs_walk_dir(void)
{
    GError *error = NULL;
    gchar *base = g_dir_make_tmp("pcv-z17-walk-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *rootfs = g_build_filename(base, "rootfs", NULL);
    gchar *host   = g_build_filename(base, "HOSTX", NULL);                                      
    gchar *mnt    = g_build_filename(rootfs, "mnt", NULL);
    gchar *ok     = g_build_filename(mnt, "ok", NULL);
    g_assert_cmpint(g_mkdir_with_parents(ok, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(host, 0700), ==, 0);
    gchar *abslink = g_build_filename(mnt, "trap", NULL);                           
    gchar *rellink = g_build_filename(rootfs, "a", NULL);                             
    g_assert_cmpint(symlink(host, abslink), ==, 0);
    g_assert_cmpint(symlink("../../..", rellink), ==, 0);

    int rootfd = open(rootfs, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    g_assert_cmpint(rootfd, >=, 0);

                         
    int fd = pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/ok", FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

                                                              
    fd = pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/new/deep", TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    gchar *deep = g_build_filename(rootfs, "mnt", "new", "deep", NULL);
    g_assert_true(g_file_test(deep, G_FILE_TEST_IS_DIR));

                                         
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/none", FALSE, &error), ==, -1);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

                                        
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/trap", TRUE, &error), ==, -1);
    g_assert_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_assert_nonnull(g_strstr_len(error->message, -1, "symbolic links"));
    g_assert_cmpint(pcv_lxc_mount_error_classify(error), ==, PCV_LXC_MOUNT_CLASS_BAD_REQUEST);
    g_clear_error(&error);

                                                 
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/trap/x", TRUE, &error), ==, -1);
    g_assert_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_clear_error(&error);

                       
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "/a/etc", TRUE, &error), ==, -1);
    g_assert_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_clear_error(&error);

                      
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "/mnt/../x", TRUE, &error), ==, -1);
    g_assert_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_clear_error(&error);

                           
    g_assert_cmpint(pcv_lxc_rootfs_walk_dir(rootfd, "mnt/ok", FALSE, &error), ==, -1);
    g_assert_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH);
    g_clear_error(&error);

                                             
    gchar *hostx = g_build_filename(host, "x", NULL);
    g_assert_false(g_file_test(hostx, G_FILE_TEST_EXISTS));
    g_free(hostx);
    guint n = 0;
    GDir *d = g_dir_open(host, 0, NULL);
    while (d && g_dir_read_name(d)) n++;
    if (d) g_dir_close(d);
    g_assert_cmpuint(n, ==, 0);

    close(rootfd);
    g_free(abslink); g_free(rellink); g_free(ok); g_free(mnt); g_free(host); g_free(rootfs); g_free(base); g_free(deep);
}

                                                                                              
static void
test_mount_error_classify(void)
{
    GError *e = g_error_new_literal(PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, "x");
    g_assert_cmpint(pcv_lxc_mount_error_classify(e), ==, PCV_LXC_MOUNT_CLASS_BAD_REQUEST);
    g_error_free(e);
    e = g_error_new_literal(PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_NO_KERNEL_SUPPORT, "x");
    g_assert_cmpint(pcv_lxc_mount_error_classify(e), ==, PCV_LXC_MOUNT_CLASS_NO_KERNEL_SUPPORT);
    g_error_free(e);
                                                                                                             
    e = g_error_new_literal(G_IO_ERROR, g_io_error_from_errno(EINVAL), "x");
    g_assert_cmpint(pcv_lxc_mount_error_classify(e), ==, PCV_LXC_MOUNT_CLASS_INTERNAL);
    g_error_free(e);
    e = g_error_new_literal(G_IO_ERROR, g_io_error_from_errno(EOPNOTSUPP), "x");
    g_assert_cmpint(pcv_lxc_mount_error_classify(e), ==, PCV_LXC_MOUNT_CLASS_INTERNAL);
    g_error_free(e);
    e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "ro not enforced");
    g_assert_cmpint(pcv_lxc_mount_error_classify(e), ==, PCV_LXC_MOUNT_CLASS_INTERNAL);
    g_error_free(e);
    g_assert_cmpint(pcv_lxc_mount_error_classify(NULL), ==, PCV_LXC_MOUNT_CLASS_INTERNAL);
}

                                                                
static void
test_config_remove_mount_entry(void)
{
    guint n = 99;
    const gchar *cfg =
        "lxc.rootfs.path = dir:/x\n"
        "lxc.mount.entry = /srv/a data none bind 0 0\n"
        "lxc.mount.entry = /srv/b mnt/database none bind 0 0\n"
        "lxc.mount.entry = /srv/data2 mnt/other none bind,ro 0 0\n"
        "# lxc.mount.entry = /srv/c data none bind 0 0\n"
        "lxc.mount.entry = /srv/d data2 none bind 0 0\n";
    gchar *out = pcv_lxc_config_remove_mount_entry(cfg, "data", &n);
    g_assert_cmpuint(n, ==, 1);                
    g_assert_cmpstr(out, ==,
        "lxc.rootfs.path = dir:/x\n"
        "lxc.mount.entry = /srv/b mnt/database none bind 0 0\n"
        "lxc.mount.entry = /srv/data2 mnt/other none bind,ro 0 0\n"
        "# lxc.mount.entry = /srv/c data none bind 0 0\n"
        "lxc.mount.entry = /srv/d data2 none bind 0 0\n");
    g_free(out);

                                                
    const gchar *ws = "  lxc.mount.entry\t=\t/srv/a \t mnt/v/ \t none  bind 0 0\nlxc.mount.entry=/srv/b mnt/v2 none bind 0 0";
    out = pcv_lxc_config_remove_mount_entry(ws, "/mnt/v/", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "lxc.mount.entry=/srv/b mnt/v2 none bind 0 0");                       
    g_free(out);

                                                     
    out = pcv_lxc_config_remove_mount_entry(cfg, "mnt", &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, cfg);
    g_free(out);

                           
    out = pcv_lxc_config_remove_mount_entry("lxc.mount.entryX = /a data none bind 0 0\n", "data", &n);
    g_assert_cmpuint(n, ==, 0);
    g_free(out);
    out = pcv_lxc_config_remove_mount_entry(cfg, "/", &n);
    g_assert_cmpuint(n, ==, 0);
    g_free(out);
    g_assert_null(pcv_lxc_config_remove_mount_entry(NULL, "data", &n));
    g_assert_null(pcv_lxc_config_remove_mount_entry(cfg, NULL, &n));

                                                                                                    
                                                      
    out = pcv_lxc_config_remove_mount_entry("lxc.uts.name = c\r\nlxc.mount.entry = /srv/a data none bind 0 0\r\n"
                                            "lxc.mount.entry = /srv/b data2 none bind 0 0\r\n", "data", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "lxc.uts.name = c\r\nlxc.mount.entry = /srv/b data2 none bind 0 0\r\n");
    g_free(out);
    out = pcv_lxc_config_remove_mount_entry("lxc.uts.name = c\rlxc.mount.entry = /srv/a data none bind 0 0\r"
                                            "lxc.net.0.type = empty\r", "/data", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "lxc.uts.name = c\rlxc.net.0.type = empty\r");
    g_free(out);
    out = pcv_lxc_config_remove_mount_entry("lxc.mount.entry = /srv/a data\r\nlxc.mount.entry = /srv/b data\n", "data", &n);
    g_assert_cmpuint(n, ==, 2);                                   
    g_assert_cmpstr(out, ==, "");
    g_free(out);

                                                                                                            
    out = pcv_lxc_config_remove_mount_entry("lxc.mount.entry = /srv/a mnt//a none bind 0 0\nlxc.mount.entry = /srv/b mnt/ab none bind 0 0\n",
                                            "/mnt/a", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "lxc.mount.entry = /srv/b mnt/ab none bind 0 0\n");
    g_free(out);
    out = pcv_lxc_config_remove_mount_entry("lxc.mount.entry = /srv/a mnt/a none bind 0 0\n", "//mnt//a/", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "");
    g_free(out);
                                                                                              
    out = pcv_lxc_config_remove_mount_entry("lxc.mount.entry = /srv/a mnt/./a none bind 0 0\nlxc.mount.entry = /srv/b mnt/./ab none bind 0 0\n",
                                            "/mnt/a", &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(out, ==, "lxc.mount.entry = /srv/b mnt/./ab none bind 0 0\n");
    g_free(out);
}

                                                               
static void
test_decide_explicit_request_fails_closed(void)
{
    g_assert_cmpint(pcv_lxc_rootless_decide(1, FALSE, FALSE), ==, PCV_LXC_ROOTLESS_REJECT);
    g_assert_cmpint(pcv_lxc_rootless_decide(1, TRUE, FALSE), ==, PCV_LXC_ROOTLESS_REJECT);
    g_assert_cmpint(pcv_lxc_rootless_decide(1, FALSE, TRUE), ==, PCV_LXC_ROOTLESS_ON);
}

                                                                                   
                                                                                   
                                                                
static void
test_decide_table(void)
{
    static const struct { gint req; gboolean def, ready; PcvLxcRootlessDecision want; } rows[] = {
        {  0, TRUE,  TRUE,  PCV_LXC_ROOTLESS_OFF },   {  0, TRUE,  FALSE, PCV_LXC_ROOTLESS_OFF },
        {  0, FALSE, TRUE,  PCV_LXC_ROOTLESS_OFF },   {  0, FALSE, FALSE, PCV_LXC_ROOTLESS_OFF },
        {  1, TRUE,  TRUE,  PCV_LXC_ROOTLESS_ON },    {  1, TRUE,  FALSE, PCV_LXC_ROOTLESS_REJECT },
        {  1, FALSE, TRUE,  PCV_LXC_ROOTLESS_ON },    {  1, FALSE, FALSE, PCV_LXC_ROOTLESS_REJECT },
        { -1, TRUE,  TRUE,  PCV_LXC_ROOTLESS_ON },    { -1, FALSE, TRUE,  PCV_LXC_ROOTLESS_OFF },
        { -1, FALSE, FALSE, PCV_LXC_ROOTLESS_OFF },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_test_message("decide(%d, %d, %d)", rows[i].req, rows[i].def, rows[i].ready);
        g_assert_cmpint(pcv_lxc_rootless_decide(rows[i].req, rows[i].def, rows[i].ready), ==, rows[i].want);
    }
    g_assert_cmpint(pcv_lxc_rootless_decide(-1, TRUE, FALSE), ==, PCV_LXC_ROOTLESS_REJECT);                                    
}

                                                                                                   

                                                                                                 
                                                            
static void
tree_fill(const gchar *root, gchar **file_s)
{
    gchar *a = g_build_filename(root, "a", NULL), *f = g_build_filename(root, "a", "f", NULL);
    gchar *s = g_build_filename(root, "s", NULL), *l = g_build_filename(root, "l", NULL);
    g_assert_cmpint(g_mkdir(a, 0755), ==, 0);
    g_assert_true(g_file_set_contents(f, "x", -1, NULL));
    g_assert_true(g_file_set_contents(s, "x", -1, NULL));
    g_assert_cmpint(g_chmod(s, 04755), ==, 0);
    g_assert_cmpint(symlink("/etc/passwd", l), ==, 0);                              
    *file_s = s;
    g_free(a); g_free(f); g_free(l);
}

                                                                    
static gchar *
tree_new(gchar **file_s)
{
    GError *error = NULL;
    gchar *root = g_dir_make_tmp("pcv-z17-scan-XXXXXX", &error);
    g_assert_no_error(error);
    tree_fill(root, file_s);
    return root;
}

static void
tree_free(gchar *root, gchar *file_s)
{
    const gchar *argv[] = { "rm", "-rf", "--", root, NULL };
    g_assert_true(pcv_spawn_sync(argv, NULL, NULL, NULL));
    g_free(root); g_free(file_s);
}

                                                                                                      
                                                                    
                                                                  
static gchar *
lxc_tree_new(gchar **lxc_path, gchar **file_s)
{
    GError *error = NULL;
    *lxc_path = g_dir_make_tmp("pcv-z17-rfs-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *c = g_build_filename(*lxc_path, "c", NULL), *rootfs = g_build_filename(c, "rootfs", NULL);
    g_assert_cmpint(g_mkdir(c, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(rootfs, 0755), ==, 0);
    tree_fill(rootfs, file_s);
    g_free(c);
    return rootfs;
}

static void
test_scan_classifies_tree(void)
{
                                                                 
    gchar *s = NULL, *root = tree_new(&s);
    PcvLxcUsernsScan scan;
    GError *error = NULL;
                                                          
    g_assert_true(pcv_lxc_userns_scan(root, G_GINT64_CONSTANT(1) << 30, G_GINT64_CONSTANT(1) << 29, &scan, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(scan.unshifted, ==, 5);                                         
    g_assert_cmpuint(scan.mapped + scan.outside, ==, 0);
    g_assert_cmpuint(g_hash_table_size(scan.pairs), ==, 1);
    g_assert_cmpuint(scan.suid_files->len, ==, 1);
    PcvLxcUsernsFile *sf = g_ptr_array_index(scan.suid_files, 0);
    g_assert_cmpstr(sf->path, ==, s);
    g_assert_cmpuint(sf->mode, ==, 04755);
    g_assert_cmpuint(scan.cap_files->len, ==, 0);
    pcv_lxc_userns_scan_clear(&scan);

    if (getuid() > 0 && getgid() > 0) {                                                                     
        g_assert_true(pcv_lxc_userns_scan(root, G_GINT64_CONSTANT(1) << 30, 1, &scan, &error));             
        g_assert_cmpuint(scan.outside, ==, 5);
        g_assert_nonnull(scan.first_outside);
        pcv_lxc_userns_scan_clear(&scan);
    }

    gint64 lo = MIN(getuid(), getgid()), hi = MAX(getuid(), getgid());
    if (lo >= hi - lo + 1) {                                                                   
        g_assert_true(pcv_lxc_userns_scan(root, lo, hi - lo + 1, &scan, &error));
        g_assert_cmpuint(scan.mapped, ==, 5);
        pcv_lxc_userns_scan_clear(&scan);
    }
    tree_free(root, s);
}

                                                                                                     

                                                                                     
                                                                                                   
static void
test_scan_counts_acl(void)
{
    gchar *lxc = NULL, *s = NULL, *rootfs = lxc_tree_new(&lxc, &s);
    gchar *a = g_build_filename(rootfs, "a", NULL), *f = g_build_filename(rootfs, "a", "f", NULL);
    const gint64 start = G_GINT64_CONSTANT(1) << 30, count = G_GINT64_CONSTANT(1) << 29;                 
    PcvLxcUsernsScan scan;
    GError *error = NULL;
    g_assert_true(pcv_lxc_userns_scan(rootfs, start, count, &scan, &error));
    g_assert_cmpuint(scan.acl, ==, 0);
    pcv_lxc_userns_scan_clear(&scan);
    g_autofree gchar *setfacl = g_find_program_in_path("setfacl");
    if (!setfacl ||
        !pcv_spawn_sync((const gchar *const[]){ "setfacl", "-m", "u:65534:r", "--", f, NULL }, NULL, NULL, NULL) ||
        !pcv_spawn_sync((const gchar *const[]){ "setfacl", "-d", "-m", "u:65534:r", "--", a, NULL }, NULL, NULL, NULL)) {
        g_test_skip("setfacl·POSIX ACL 지원 필요");
    } else {
        g_assert_true(pcv_lxc_userns_scan(rootfs, start, count, &scan, &error));
        g_assert_cmpuint(scan.acl, ==, 2);                                                     
        g_assert_cmpuint(scan.unshifted, ==, 5);                          
        pcv_lxc_userns_scan_clear(&scan);
        gboolean fell_back = TRUE;                                                               
        g_assert_false(pcv_lxc_userns_settle_rootfs(lxc, "c", start, count, &fell_back, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
        g_assert_nonnull(strstr(error->message, "2 entries with POSIX ACLs"));
        g_assert_false(fell_back);
        g_clear_error(&error);
    }
    g_free(a); g_free(f); g_free(rootfs);
    tree_free(lxc, s);
}

                                                                                     
                                                                                                 
static void
test_settle_rootfs_outside_and_mapped(void)
{
    gchar *lxc = NULL, *s = NULL, *rootfs = lxc_tree_new(&lxc, &s);
    if (geteuid() == 0)
        g_assert_true(pcv_spawn_sync((const gchar *const[]){ "chown", "-R", "-h", "70000:70000", "--", rootfs, NULL },
                                     NULL, NULL, NULL));
    gboolean fell_back = TRUE;
    GError *error = NULL;
    g_assert_false(pcv_lxc_userns_settle_rootfs(lxc, "c", G_GINT64_CONSTANT(1) << 30, 1, &fell_back, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "has 5 entries with ids outside"));                    
    g_assert_false(fell_back);
    g_clear_error(&error);
    struct stat st;
    g_assert_cmpint(lstat(rootfs, &st), ==, 0);
    gint64 lo = MIN(st.st_uid, st.st_gid), hi = MAX(st.st_uid, st.st_gid);
    if (lo >= hi - lo + 1) {                                                                  
        fell_back = TRUE;
        g_assert_true(pcv_lxc_userns_settle_rootfs(lxc, "c", lo, hi - lo + 1, &fell_back, &error));
        g_assert_no_error(error);
        g_assert_false(fell_back);
    }
    g_free(rootfs);
    tree_free(lxc, s);
}

                                                                               
                                                           
                                                        
static void
test_scan_unreadable_fails(void)
{
    gchar *s = NULL, *root = tree_new(&s);
    gchar *locked = g_build_filename(root, "a", "locked", NULL);
    g_assert_cmpint(g_mkdir(locked, 0700), ==, 0);
    g_assert_cmpint(g_chmod(locked, 0000), ==, 0);
    if (geteuid() != 0) {
        PcvLxcUsernsScan scan;
        GError *error = NULL;
        g_assert_false(pcv_lxc_userns_scan(root, G_GINT64_CONSTANT(1) << 30, G_GINT64_CONSTANT(1) << 29, &scan, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_nonnull(strstr(error->message, "1 unreadable"));
        g_assert_cmpuint(scan.unreadable, ==, 1);
        g_clear_error(&error);
        pcv_lxc_userns_scan_clear(&scan);
    } else {
        g_test_message("root: DAC_OVERRIDE로 0000 디렉터리를 읽는다 — 비root 전용 단언 생략");
    }
    g_assert_cmpint(g_chmod(locked, 0700), ==, 0);
    g_free(locked);
    tree_free(root, s);
}

static void
test_create_config_written_0700_and_removed(void)
{
    GError *error = NULL;
    gchar *base_dir = g_dir_make_tmp("pcv-z17-def-XXXXXX", &error);
    gchar *def = g_build_filename(base_dir, "default.conf", NULL);
    g_assert_true(g_file_set_contents(def, "lxc.apparmor.profile = generated\nlxc.idmap = u 0 1 1\nlxc.net.0.link = lxcbr0\n", -1, NULL));
    gchar *dir = pcv_lxc_userns_write_create_config(def, 100000, 65536, "lxc.net.0.link = virbr0\n", &error);
    g_assert_no_error(error);
    GStatBuf st;
    g_assert_cmpint(g_stat(dir, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
    gchar *conf = g_build_filename(dir, "config", NULL), *text = NULL;
    g_assert_cmpint(g_stat(conf, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0600);                                              
    g_assert_true(g_file_get_contents(conf, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "lxc.apparmor.profile = generated\nlxc.net.0.link = virbr0\nlxc.idmap = u 0 100000 65536\n"
                              "lxc.idmap = g 0 100000 65536\nlxc.init.uid = 0\nlxc.init.gid = 0\n");
    pcv_lxc_userns_remove_create_config(dir);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
    gchar *missing_def = g_build_filename(base_dir, "absent.conf", NULL);                         
    gchar *dir2 = pcv_lxc_userns_write_create_config(missing_def, 100000, 65536, NULL, &error);
    g_assert_no_error(error);
    pcv_lxc_userns_remove_create_config(dir2);
    g_unlink(def); g_rmdir(base_dir);
    g_free(text); g_free(conf); g_free(dir); g_free(dir2); g_free(def); g_free(missing_def); g_free(base_dir);
}

                                                                                    
                                                                                   
                                                                                  
static gchar *
searchable_tmp_dir(const gchar *prefix)
{
    gchar *t = g_strdup_printf("/tmp/%s-XXXXXX", prefix);
    g_assert_nonnull(g_mkdtemp(t));
    return t;
}

static void
test_unsearchable_ancestor(void)
{
    gchar *t = searchable_tmp_dir("pcv-z17-anc");                             
    gchar *x = g_build_filename(t, "x", NULL);
    g_assert_cmpint(g_mkdir(x, 0755), ==, 0);
    gchar *hit = pcv_lxc_userns_unsearchable_ancestor(x);
    g_assert_cmpstr(hit, ==, t);
    g_assert_cmpint(g_chmod(t, 0710), ==, 0);                                                   
    gchar *hit_g = pcv_lxc_userns_unsearchable_ancestor(x);
    g_assert_cmpstr(hit_g, ==, t);
    g_assert_cmpint(g_chmod(t, 0701), ==, 0);                                          
    gchar *none = pcv_lxc_userns_unsearchable_ancestor(x);
    g_assert_null(none);                                                             
    g_rmdir(x); g_rmdir(t);
    g_free(hit); g_free(hit_g); g_free(x); g_free(t);
}

                                                                                
                                                             
static void
test_settle_dirs_refuses_unsearchable_parent(void)
{
    gchar *t = searchable_tmp_dir("pcv-z17-l5");                                           
    gchar *lxc = g_build_filename(t, "lxc", NULL), *dir = g_build_filename(lxc, "c", NULL);
    g_assert_cmpint(g_mkdir(lxc, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_assert_cmpint(g_chmod(dir, 0755), ==, 0);
    GError *error = NULL;
    g_assert_false(pcv_lxc_userns_settle_dirs(lxc, "c", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, t));
    g_clear_error(&error);
    GStatBuf st;
    g_assert_cmpint(g_stat(dir, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 07777, ==, 0755);                             
    g_assert_cmpint(g_stat(lxc, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 07777, ==, 0700);
    g_rmdir(dir); g_rmdir(lxc); g_rmdir(t);
    g_free(dir); g_free(lxc); g_free(t);
}

static void
test_shift_rootfs_refuses_acl(void)
{
    PcvLxcUsernsScan scan = { .unshifted = 1, .acl = 1 };
    GError *error = NULL;
    g_assert_false(pcv_lxc_userns_shift_rootfs("/nonexistent", &scan, 100000, 65536, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "POSIX ACL"));
    g_clear_error(&error);
}

                                                                                                             
static gchar *
edit_dir_new(const gchar *text, guint mode, gchar **conf)
{
    GError *error = NULL;
    gchar *t = g_dir_make_tmp("pcv-z17-edit-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *c = g_build_filename(t, "c", NULL);
    *conf = g_build_filename(c, "config", NULL);
    g_assert_cmpint(g_mkdir(c, 0711), ==, 0);
    g_assert_true(g_file_set_contents(*conf, text, -1, NULL));
    g_assert_cmpint(g_chmod(*conf, mode), ==, 0);
    g_free(c);
    return t;
}

                                       
static void
assert_file_text(const gchar *path, const gchar *want)
{
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, want);
    g_free(text);
}

                                                                                        
                                        
static void
test_config_edit_atomic(void)
{
    const PcvLxcConfigEdit set_link[] = { { "lxc.net.0.link", "pcvbr0", FALSE } };
    GError *error = NULL;
    gchar *conf = NULL;
    gchar *t = edit_dir_new("lxc.net.0.link = lxcbr0\n", 0640, &conf);
    GStatBuf before, after;
    g_assert_cmpint(g_lstat(conf, &before), ==, 0);
    mode_t old_mask = umask(077);                                                                 
    gboolean ok = pcv_lxc_userns_config_edit(t, "c", set_link, 1, &error);
    umask(old_mask);
    g_assert_no_error(error);
    g_assert_true(ok);
    assert_file_text(conf, "lxc.net.0.link = pcvbr0\n");
    g_assert_cmpint(g_lstat(conf, &after), ==, 0);
    g_assert_true(S_ISREG(after.st_mode));
    g_assert_cmpuint(after.st_mode & 0777, ==, 0640);
    g_assert_cmpuint(after.st_nlink, ==, 1);
    g_assert_cmpuint(after.st_ino, !=, before.st_ino);                                           
    gchar *cdir = g_build_filename(t, "c", NULL);
    GDir *dir = g_dir_open(cdir, 0, NULL);
    g_assert_nonnull(dir);
    g_assert_cmpstr(g_dir_read_name(dir), ==, "config");                            
    g_assert_null(g_dir_read_name(dir));
    g_dir_close(dir);
                                   
    const PcvLxcConfigEdit inject[] = { { "lxc.net.0.link", "x\nlxc.hook.pre-start = /x", FALSE } };
    g_assert_false(pcv_lxc_userns_config_edit(t, "c", inject, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    assert_file_text(conf, "lxc.net.0.link = pcvbr0\n");
                                     
    gchar *target = g_build_filename(t, "target", NULL);
    g_assert_true(g_file_set_contents(target, "keep\n", -1, NULL));
    g_assert_cmpint(g_unlink(conf), ==, 0);
    g_assert_cmpint(symlink(target, conf), ==, 0);
    g_assert_false(pcv_lxc_userns_config_edit(t, "c", set_link, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    assert_file_text(target, "keep\n");
    g_assert_cmpint(g_lstat(conf, &after), ==, 0);
    g_assert_true(S_ISLNK(after.st_mode));
    g_assert_cmpint(g_unlink(conf), ==, 0);
    g_assert_true(g_file_set_contents(conf, "a = 1\n", -1, NULL));
                           
    gchar *alias = g_build_filename(t, "alias", NULL);
    g_assert_cmpint(symlink("c", alias), ==, 0);
    g_assert_false(pcv_lxc_userns_config_edit(t, "alias", set_link, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    assert_file_text(conf, "a = 1\n");
                                                                     
    gchar *hard = g_build_filename(t, "hard", NULL);
    g_assert_cmpint(link(conf, hard), ==, 0);
    g_assert_false(pcv_lxc_userns_config_edit(t, "c", set_link, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    assert_file_text(conf, "a = 1\n");
    g_assert_cmpint(g_unlink(hard), ==, 0);
    if (geteuid() == 0) {                                                              
        g_assert_cmpint(lchown(conf, 100000, 100000), ==, 0);
        g_assert_false(pcv_lxc_userns_config_edit(t, "c", set_link, 1, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_clear_error(&error);
        g_assert_cmpint(lchown(conf, 0, 0), ==, 0);
        g_assert_cmpint(lchown(cdir, 100000, 100000), ==, 0);
        g_assert_false(pcv_lxc_userns_config_edit(t, "c", set_link, 1, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_clear_error(&error);
        assert_file_text(conf, "a = 1\n");
    } else {
        g_test_message("비root — 소유 불일치 단언은 root 실행(.101)에서 본다");
    }
    tree_free(t, NULL);
    g_free(conf); g_free(cdir); g_free(target); g_free(alias); g_free(hard);
}

                                                                                                              
static gchar *
top_dir_new(gchar **cdir)
{
    GError *error = NULL;
    gchar *t = g_dir_make_tmp("pcv-z17-top-XXXXXX", &error);
    g_assert_no_error(error);
    *cdir = g_build_filename(t, "c", NULL);
    gchar *conf = g_build_filename(*cdir, "config", NULL), *rootfs = g_build_filename(*cdir, "rootfs", NULL);
    gchar *meta = g_build_filename(*cdir, "purecvisor.meta", NULL);
    g_assert_cmpint(g_mkdir(*cdir, 0711), ==, 0);
    g_assert_cmpint(g_mkdir(rootfs, 0755), ==, 0);
    g_assert_true(g_file_set_contents(conf, "lxc.idmap = u 0 100000 65536\n", -1, NULL));
    g_assert_true(g_file_set_contents(meta, "alpine:3.22", -1, NULL));
    g_free(conf); g_free(rootfs); g_free(meta);
    return t;
}

                                                        
static void
assert_top_refused(const gchar *t, const gchar *name, const gchar *item)
{
    GError *error = NULL;
    g_assert_false(pcv_lxc_userns_check_top(t, name, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_autofree gchar *prefix = g_strdup_printf("%s: ", item);
    g_test_message("check_top: %s", error->message);
    g_assert_true(g_str_has_prefix(error->message, prefix));
    g_clear_error(&error);
}

                                                                                                    
                                                                                        
static void
test_check_top(void)
{
    GError *error = NULL;
    gchar *cdir = NULL;
    gchar *t = top_dir_new(&cdir);
    g_assert_true(pcv_lxc_userns_check_top(t, "c", &error));                          
    g_assert_no_error(error);
    gchar *evil = g_build_filename(cdir, "lxc.hooks", NULL);                                 
    g_assert_cmpint(symlink("/etc/passwd", evil), ==, 0);
    assert_top_refused(t, "c", "lxc.hooks");
    g_assert_cmpint(g_unlink(evil), ==, 0);
    gchar *rootfs = g_build_filename(cdir, "rootfs", NULL), *real = g_build_filename(t, "real", NULL);
    g_assert_cmpint(g_rmdir(rootfs), ==, 0);                                                    
    g_assert_cmpint(g_mkdir(real, 0755), ==, 0);
    g_assert_cmpint(symlink(real, rootfs), ==, 0);
    assert_top_refused(t, "c", "rootfs");
    g_assert_cmpint(g_unlink(rootfs), ==, 0);
    g_assert_true(g_file_set_contents(rootfs, "x", -1, NULL));                                       
    assert_top_refused(t, "c", "rootfs");
    g_assert_cmpint(g_unlink(rootfs), ==, 0);
    g_assert_cmpint(g_mkdir(rootfs, 0755), ==, 0);
    gchar *conf = g_build_filename(cdir, "config", NULL), *hard = g_build_filename(t, "config.hard", NULL);
    g_assert_cmpint(link(conf, hard), ==, 0);                                                       
    assert_top_refused(t, "c", "config");
    g_assert_cmpint(g_unlink(hard), ==, 0);
    g_assert_cmpint(g_unlink(conf), ==, 0);                                                    
    assert_top_refused(t, "c", "config");
    g_assert_true(g_file_set_contents(conf, "x = 1\n", -1, NULL));
    gchar *alias = g_build_filename(t, "alias", NULL);                                            
    g_assert_cmpint(symlink("c", alias), ==, 0);
    assert_top_refused(t, "alias", "alias");
    if (geteuid() == 0) {                                                                    
        gchar *owner = g_build_filename(cdir, "purecvisor.owner", NULL);
        g_assert_true(g_file_set_contents(owner, "alice", -1, NULL));
        g_assert_cmpint(lchown(owner, 100000, 100000), ==, 0);
        assert_top_refused(t, "c", "purecvisor.owner");
        g_assert_cmpint(g_unlink(owner), ==, 0);
        g_assert_cmpint(lchown(rootfs, 100000, 100000), ==, 0);                                       
        g_assert_true(pcv_lxc_userns_check_top(t, "c", &error));
        g_assert_no_error(error);
        g_free(owner);
    } else {
        g_test_message("비root — 소유 불일치 단언은 root 실행(.101)에서 본다");
    }
    tree_free(t, NULL);
    g_free(cdir); g_free(evil); g_free(rootfs); g_free(real); g_free(conf); g_free(hard); g_free(alias);
}

                                                                                                 

                                                                      
static gint64
root_start(void)
{
    const gchar *e = g_getenv("PCV_TEST_USERNS_START");
    return e ? g_ascii_strtoll(e, NULL, 10) : 100000;
}

                                                                                
static gboolean
root_ready(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요(Level 2 .101 make test)");
        return FALSE;
    }
    for (const gchar *const *t = (const gchar *const[]){ "chown", "chmod", "getcap", "setcap", "lxc-usernsexec", NULL }; *t; t++) {
        g_autofree gchar *p = g_find_program_in_path(*t);
        if (!p) {
            g_test_skip("chown·chmod·getcap·setcap·lxc-usernsexec 필요");
            return FALSE;
        }
    }
    if (pcv_lxc_userns_host_check(PCV_LXC_SUBUID_PATH, PCV_LXC_SUBGID_PATH, root_start(), 65536) != PCV_LXC_USERNS_READY) {
        g_test_skip("호스트 /etc/subuid·subgid에 root 위임 필요(PCV_TEST_USERNS_START)");
        return FALSE;
    }
    return TRUE;
}

                                                                
static void
run_as_root(const gchar *const *argv)
{
    GError *error = NULL;
    g_assert_true(pcv_spawn_sync(argv, NULL, NULL, &error));
    g_assert_no_error(error);
}

static void
test_root_shift_rootfs_under_daemon_caps(void)
{
    gint64 start = root_start();
    if (g_test_subprocess()) {
        const gchar *rootfs = g_getenv("PCV_TEST_SHIFT_ROOTFS");
        g_assert_true(pcv_privdrop_capabilities());                                                        
        PcvLxcUsernsScan scan;
        GError *error = NULL;
        g_assert_true(pcv_lxc_userns_scan(rootfs, start, 65536, &scan, &error));
        g_assert_no_error(error);
        g_assert_true(pcv_lxc_userns_shift_rootfs(rootfs, &scan, start, 65536, &error));
        g_assert_no_error(error);
        pcv_lxc_userns_scan_clear(&scan);
        return;
    }
    if (!root_ready())
        return;
    GError *error = NULL;
    gchar *t = searchable_tmp_dir("pcv-z17-root");
    g_assert_cmpint(g_chmod(t, 0711), ==, 0);                                                   
    gchar *r = g_build_filename(t, "rootfs", NULL);
    gchar *bin = g_build_filename(r, "bin", NULL), *etc = g_build_filename(r, "etc", NULL);
    gchar *ping = g_build_filename(bin, "ping", NULL), *su = g_build_filename(bin, "su", NULL);
    gchar *su2 = g_build_filename(bin, "su.hard", NULL), *shadow = g_build_filename(etc, "shadow", NULL);
    gchar *hostlink = g_build_filename(r, "hostname", NULL);
    g_assert_cmpint(g_mkdir(r, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(bin, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(etc, 0755), ==, 0);
    g_assert_true(g_file_set_contents(ping, "x", -1, NULL));
    g_assert_true(g_file_set_contents(su, "x", -1, NULL));
    g_assert_true(g_file_set_contents(shadow, "x", -1, NULL));
    g_assert_cmpint(g_chmod(su, 04755), ==, 0);
    g_assert_cmpint(link(su, su2), ==, 0);                                                   
    g_assert_cmpint(symlink("/etc/hostname", hostlink), ==, 0);
    run_as_root((const gchar *const[]){ "chown", "0:42", "--", shadow, NULL });
    run_as_root((const gchar *const[]){ "setcap", "cap_net_raw=ep", ping, NULL });

    g_setenv("PCV_TEST_SHIFT_ROOTFS", r, TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
    g_test_trap_assert_passed();

    struct stat st;
    g_assert_cmpint(lstat(r, &st), ==, 0);       g_assert_cmpint(st.st_uid, ==, start); g_assert_cmpint(st.st_gid, ==, start);
    g_assert_cmpint(lstat(su, &st), ==, 0);      g_assert_cmpint(st.st_uid, ==, start); g_assert_cmpuint(st.st_mode & 07777, ==, 04755);
    g_assert_cmpint(lstat(shadow, &st), ==, 0);  g_assert_cmpint(st.st_gid, ==, start + 42);
    g_assert_cmpint(lstat(hostlink, &st), ==, 0); g_assert_cmpint(st.st_uid, ==, start);                 
    g_assert_cmpint(stat("/etc/hostname", &st), ==, 0); g_assert_cmpint(st.st_uid, ==, 0);              
    guint8 cap[64];
    gssize n = lgetxattr(ping, "security.capability", cap, sizeof cap);
    g_assert_cmpint(n, ==, 24);                                                                             
    guint32 magic = cap[0] | cap[1] << 8 | cap[2] << 16 | (guint32)cap[3] << 24;
    guint32 rootid = cap[20] | cap[21] << 8 | cap[22] << 16 | (guint32)cap[23] << 24;
    g_assert_cmpuint(magic >> 24, ==, 3);
    g_assert_cmpuint(rootid, ==, start);
    PcvLxcUsernsScan scan;
    g_assert_true(pcv_lxc_userns_scan(r, start, 65536, &scan, &error));
    g_assert_cmpuint(scan.unshifted + scan.outside, ==, 0);
    pcv_lxc_userns_scan_clear(&scan);
    run_as_root((const gchar *const[]){ "rm", "-rf", "--", t, NULL });
    g_free(t); g_free(r); g_free(bin); g_free(etc); g_free(ping); g_free(su); g_free(su2); g_free(shadow); g_free(hostlink);
}

                                                                                                          
                                                                                         
                                                
                                                        
static void
settle_case(const gchar *t, const gchar *lxc, guint path_mode, const gchar *name, guint dir_mode,
            guint *dir_out, guint *path_out)
{
    gchar *path = g_build_filename(t, lxc, NULL), *dir = g_build_filename(path, name, NULL);
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    g_assert_cmpint(g_chmod(path, path_mode), ==, 0);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_assert_cmpint(g_chmod(dir, dir_mode), ==, 0);
    run_as_root((const gchar *const[]){ "chown", "100000:100000", "--", dir, NULL });
    g_setenv("PCV_TEST_SETTLE_PATH", path, TRUE);
    g_setenv("PCV_TEST_SETTLE_NAME", name, TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
    g_test_trap_assert_passed();
    struct stat st;
    g_assert_cmpint(stat(dir, &st), ==, 0);
    g_assert_cmpint(st.st_uid, ==, 0);
    g_assert_cmpint(st.st_gid, ==, 0);
    *dir_out = st.st_mode & 07777;
    g_assert_cmpint(stat(path, &st), ==, 0);
    *path_out = st.st_mode & 07777;
    g_free(dir); g_free(path);
}

static void
test_root_settle_dirs_under_daemon_caps(void)
{
    if (g_test_subprocess()) {
        g_assert_true(pcv_privdrop_capabilities());
        GError *error = NULL;
        g_assert_true(pcv_lxc_userns_settle_dirs(g_getenv("PCV_TEST_SETTLE_PATH"), g_getenv("PCV_TEST_SETTLE_NAME"), &error));
        g_assert_no_error(error);
        return;
    }
    if (!root_ready())
        return;
    gchar *t = searchable_tmp_dir("pcv-z17-settle");
    g_assert_cmpint(g_chmod(t, 0711), ==, 0);
    guint dir_mode = 0, path_mode = 0;
    settle_case(t, "lxc", 0700, "c1", 0770, &dir_mode, &path_mode);                                     
    g_assert_cmpuint(dir_mode, ==, 0711);                                                                
    g_assert_cmpuint(path_mode, ==, 0711);
    settle_case(t, "lxc2", 0750, "c2", 0755, &dir_mode, &path_mode);                                       
    g_assert_cmpuint(dir_mode, ==, 0711);                                                   
    g_assert_cmpuint(path_mode, ==, 0751);                                                              
    run_as_root((const gchar *const[]){ "rm", "-rf", "--", t, NULL });
    g_free(t);
}

                                                                                                           
                                                                                                            
                                                                     
                                                                                                        
                                                                                                                      
                                                                                               
                                                                               
static void
test_mount_move_from_source_fd(void)
{
    if (g_test_subprocess()) {
        const gchar *t = g_getenv("PCV_TEST_MOVE_DIR");
        g_assert_true(pcv_privdrop_capabilities());
        gchar *src = g_build_filename(t, "src", NULL), *old = g_build_filename(t, "src.old", NULL);
        gchar *evil = g_build_filename(t, "evil", NULL), *dst = g_build_filename(t, "dst", NULL);
        gchar *m1 = g_build_filename(dst, "m1", NULL), *m2 = g_build_filename(dst, "m2", NULL);
        GError *error = NULL;
        int src_fd = pcv_lxc_volume_source_open(src, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpint(src_fd, >=, 0);
        g_assert_cmpint(rename(src, old), ==, 0);                                              
        g_assert_cmpint(symlink(evil, src), ==, 0);
        int target_fd = open(dst, O_PATH | O_DIRECTORY | O_CLOEXEC);
        g_assert_cmpint(target_fd, >=, 0);
        guint64 clone_id = 0;
        gboolean ok = pcv_lxc_userns_mount_clone(src_fd, target_fd, PCV_LXC_MOUNT_RO, &clone_id, &error);
        close(src_fd);
        close(target_fd);
        g_assert_no_error(error);
        g_assert_true(ok);
        gboolean saw_m1 = g_file_test(m1, G_FILE_TEST_EXISTS), saw_m2 = g_file_test(m2, G_FILE_TEST_EXISTS);
        struct statvfs vfs;
        int vrc = statvfs(dst, &vfs);
        struct statx now = { 0 };                                                                     
        int src2 = statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &now);
        int urc = umount2(dst, 0);                                                                
        g_assert_cmpint(src2, ==, 0);
        g_assert_true(now.stx_mask & STATX_MNT_ID);
        g_assert_cmpuint(clone_id, !=, 0);
        g_assert_cmpuint(clone_id, ==, now.stx_mnt_id);                                                         
        g_assert_true(saw_m1);
        g_assert_false(saw_m2);
        g_assert_cmpint(vrc, ==, 0);
        g_assert_true(vfs.f_flag & ST_RDONLY);
        g_assert_cmpint(urc, ==, 0);
        g_free(src); g_free(old); g_free(evil); g_free(dst); g_free(m1); g_free(m2);
        return;
    }
    if (geteuid() != 0) {
        g_test_skip("root 필요(Level 2 .101 make test)");
        return;
    }
    GError *error = NULL;
    gchar *made = g_dir_make_tmp("pcv-a02-move-XXXXXX", &error);
    g_assert_no_error(error);
    char real[PATH_MAX];
    g_assert_nonnull(realpath(made, real));                                                            
    gchar *src = g_build_filename(real, "src", NULL), *evil = g_build_filename(real, "evil", NULL);
    gchar *dst = g_build_filename(real, "dst", NULL);
    gchar *m1 = g_build_filename(src, "m1", NULL), *m2 = g_build_filename(evil, "m2", NULL);
    g_assert_cmpint(g_mkdir(src, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(evil, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(dst, 0755), ==, 0);
    g_assert_true(g_file_set_contents(m1, "1", -1, NULL));
    g_assert_true(g_file_set_contents(m2, "2", -1, NULL));
    g_setenv("PCV_TEST_MOVE_DIR", real, TRUE);
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
    g_test_trap_assert_passed();
    run_as_root((const gchar *const[]){ "rm", "-rf", "--", real, NULL });
    g_free(made); g_free(src); g_free(evil); g_free(dst); g_free(m1); g_free(m2);
}

void
test_lxc_userns_register(void)
{
    g_test_add_func("/lxc_userns/host/root_delegation", test_host_root_delegation);
    g_test_add_func("/lxc_userns/host/overlap_boundaries", test_host_overlap_boundaries);
    g_test_add_func("/lxc_userns/host/numbers_like_shadow", test_host_numbers_like_shadow);
    g_test_add_func("/lxc_userns/host/other_lines_like_shadow_4174", test_host_other_lines_like_shadow_4174);
    g_test_add_func("/lxc_userns/host/root_line_strict", test_host_root_line_strict);
    g_test_add_func("/lxc_userns/host/root_line_rejection_warns", test_host_root_line_rejection_warns);
    g_test_add_func("/lxc_userns/host/bad_range", test_host_bad_range);
    g_test_add_func("/lxc_userns/host/message_guides_without_account_names", test_host_message_guides_without_account_names);
    g_test_add_func("/lxc_userns/settings/parse_default", test_settings_parse_default);
    g_test_add_func("/lxc_userns/settings/load_range", test_settings_load_range);
    g_test_add_func("/lxc_userns/create_config/resource_lines", test_resource_lines);
    g_test_add_func("/lxc_userns/create_config/body", test_build_create_config);
    g_test_add_func("/lxc_userns/config/apply_edits", test_config_apply_edits);
    g_test_add_func("/lxc_userns/config/apply_edits_rejects_control_chars", test_config_apply_edits_rejects_control_chars);
    g_test_add_func("/lxc_userns/config/idmap_and_include", test_idmap_and_include);
    g_test_add_func("/lxc_userns/ids/classify_and_shift", test_classify_and_shift);
    g_test_add_func("/lxc_userns/fallback/getcap_text", test_getcap_text);
    g_test_add_func("/lxc_userns/mountinfo/fields", test_mountinfo_fields);
    g_test_add_func("/lxc_userns/mountinfo/escape_and_lexical", test_mountinfo_escape_and_lexical);
    g_test_add_func("/lxc_userns/mountinfo/stacked_last_line", test_mountinfo_stacked_last_line);
    g_test_add_func("/lxc_userns/mountinfo/absent_and_invalid", test_mountinfo_absent_and_invalid);
    g_test_add_func("/lxc_userns/rootfs/walk_dir", test_rootfs_walk_dir);
    g_test_add_func("/lxc_userns/mount/error_classify", test_mount_error_classify);
    g_test_add_func("/lxc_userns/config/remove_mount_entry", test_config_remove_mount_entry);
    g_test_add_func("/lxc_userns/decide/explicit_request_fails_closed", test_decide_explicit_request_fails_closed);
    g_test_add_func("/lxc_userns/decide/table", test_decide_table);
    g_test_add_func("/lxc_userns/scan/classifies_tree", test_scan_classifies_tree);
    g_test_add_func("/lxc_userns/scan/counts_acl", test_scan_counts_acl);
    g_test_add_func("/lxc_userns/scan/unreadable_fails", test_scan_unreadable_fails);
    g_test_add_func("/lxc_userns/settle/rootfs_outside_and_mapped", test_settle_rootfs_outside_and_mapped);
    g_test_add_func("/lxc_userns/create_config/written_0700_and_removed", test_create_config_written_0700_and_removed);
    g_test_add_func("/lxc_userns/settle/unsearchable_ancestor", test_unsearchable_ancestor);
    g_test_add_func("/lxc_userns/settle/dirs_refuses_unsearchable_parent", test_settle_dirs_refuses_unsearchable_parent);
    g_test_add_func("/lxc_userns/fallback/refuses_acl", test_shift_rootfs_refuses_acl);
    g_test_add_func("/lxc_userns/config/edit_atomic", test_config_edit_atomic);
    g_test_add_func("/lxc_userns/settle/check_top", test_check_top);
    g_test_add_func("/lxc_userns/root/shift_rootfs_under_daemon_caps", test_root_shift_rootfs_under_daemon_caps);
    g_test_add_func("/lxc_userns/root/settle_dirs_under_daemon_caps", test_root_settle_dirs_under_daemon_caps);
    g_test_add_func("/lxc_userns/mount_move_from_source_fd", test_mount_move_from_source_fd);                        
}
