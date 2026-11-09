                                                                                   
                                                                             
                                                                        
                                                                                   
                                                                       
                                               
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/modules/backup/replication_hostkey.h"
#include "../src/modules/dispatcher/rpc_utils.h"
#include "../src/api/dispatcher_owner_scope.h"

                                                                             
#define KEY_A "AAAAC3NzaC1lZDI1NTE5AAAAIAE0Z2xt6Yvu4H6ptdw9C/JSPmnrkHjESJ9R2GiQ33yw"
#define FP_A  "SHA256:rBbYAUGcs8skM8lqu8SNzYTuKrv89qMai86nLA2VFZE"
#define KEY_B "AAAAC3NzaC1lZDI1NTE5AAAAIFjVDdBLZ4Mbgu1oSPMibGOEhTwCdW7ogbQvuzvNZeZt"
#define FP_B  "SHA256:5qiAQJdiPP8DLHnwmaeHnl1yBGdH3/uGmt6v05D395s"
#define KEY_E "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBD9Tcis1M/TDFWmj5HwRpeCSCVTwDI2zr4bKukU2wn4OGXS2h7CP750+c3xb+0fDJ9FQRqOz9Pa780yAz7P5I4w="
#define FP_E  "SHA256:h8k0j7Pi4d9mGv7Pg5qOxtPd1ZFta3k+1HDXdU5Sge0"

static gchar *g_kh_dir = NULL;
static gchar *g_kh_file = NULL;

static void
kh_setup(void)
{
    g_kh_dir = g_dir_make_tmp("pcv-repl-hostkey-XXXXXX", NULL);
    g_kh_file = g_build_filename(g_kh_dir, "replication_known_hosts", NULL);
    pcv_repl_known_hosts_set_path_for_test(g_kh_file);
}

static void
kh_teardown(void)
{
    pcv_repl_known_hosts_set_path_for_test(NULL);
    g_unlink(g_kh_file);
    g_rmdir(g_kh_dir);
    g_clear_pointer(&g_kh_file, g_free);
    g_clear_pointer(&g_kh_dir, g_free);
}

                                    
static gboolean
has_pair(gchar **argv, const gchar *flag, const gchar *value)
{
    for (guint i = 0; argv[i] && argv[i + 1]; i++)
        if (g_str_equal(argv[i], flag) && g_str_equal(argv[i + 1], value))
            return TRUE;
    return FALSE;
}

                                                                     
static void
test_ssh_argv_required_options(void)
{
    kh_setup();
    const gchar *cmd[] = { "zfs", "list", NULL };
    gchar **argv = pcv_repl_ssh_argv("pcv", "Node-B.Example", cmd);
    g_assert_nonnull(argv);
    g_assert_cmpstr(argv[0], ==, "ssh");
    gchar *khf = g_strdup_printf("UserKnownHostsFile=%s", g_kh_file);
    g_assert_true(has_pair(argv, "-F", "/dev/null"));
    g_assert_true(has_pair(argv, "-o", "BatchMode=yes"));
    g_assert_true(has_pair(argv, "-o", "StrictHostKeyChecking=yes"));
    g_assert_true(has_pair(argv, "-o", khf));
    g_assert_true(has_pair(argv, "-o", "GlobalKnownHostsFile=/dev/null"));
    g_assert_true(has_pair(argv, "-o", "HostKeyAlias=node-b.example"));
    g_assert_true(has_pair(argv, "-o", "CheckHostIP=no"));
    g_assert_true(has_pair(argv, "-o", "UpdateHostKeys=no"));
    g_assert_true(has_pair(argv, "-o", "IdentitiesOnly=yes"));
    g_assert_true(has_pair(argv, "-o", "ConnectTimeout=10"));
    g_assert_true(has_pair(argv, "-i", "/etc/purecvisor/cluster_id_ed25519"));
    guint n = g_strv_length(argv);
    g_assert_cmpstr(argv[n - 3], ==, "pcv@node-b.example");
    g_assert_cmpstr(argv[n - 2], ==, "zfs");
    g_assert_cmpstr(argv[n - 1], ==, "list");
    for (guint i = 0; argv[i]; i++) {
        g_assert_null(strstr(argv[i], "StrictHostKeyChecking=no"));
        g_assert_null(strstr(argv[i], "accept-new"));
        g_assert_null(strstr(argv[i], "UserKnownHostsFile=/dev/null"));
    }
    g_free(khf);
    g_strfreev(argv);
    kh_teardown();
}

                                                    
static void
test_ssh_argv_rejects_bad_input(void)
{
    const gchar *cmd[] = { "true", NULL };
    const gchar *empty[] = { NULL };
    g_assert_null(pcv_repl_ssh_argv("pcv", "-oProxyCommand=sh", cmd));
    g_assert_null(pcv_repl_ssh_argv("pcv", "node b", cmd));
    g_assert_null(pcv_repl_ssh_argv("pcv", "node-b:22", cmd));
    g_assert_null(pcv_repl_ssh_argv("-oX", "node-b", cmd));
    g_assert_null(pcv_repl_ssh_argv(NULL, "node-b", cmd));
    g_assert_null(pcv_repl_ssh_argv("pcv", "node-b", NULL));
    g_assert_null(pcv_repl_ssh_argv("pcv", "node-b", empty));
}

                                                                   
                                                                  
                                                        
#define SEAM_PATH_A "/tmp/pcv-kh-seam-a"
#define SEAM_PATH_B "/tmp/pcv-kh-seam-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

                                                              
                                                                   
                                                                     
                                                                             
                                  
typedef struct {
    gint stop;
    gint flips;
} SeamFlipCtx;

static gpointer
seam_flipper(gpointer data)
{
    SeamFlipCtx *ctx = data;
    for (guint i = 0; !g_atomic_int_get(&ctx->stop); i++) {
        pcv_repl_known_hosts_set_path_for_test((i & 1u) ? SEAM_PATH_A : SEAM_PATH_B);
        g_atomic_int_inc(&ctx->flips);
        g_thread_yield();
    }
    return NULL;
}

                                                                        
                        
static void
test_ssh_argv_path_seam_concurrent(void)
{
    const gchar *cmd[] = { "true", NULL };
    SeamFlipCtx ctx = { 0, 0 };
    pcv_repl_known_hosts_set_path_for_test(SEAM_PATH_A);
    GThread *t = g_thread_new("kh-seam-flip", seam_flipper, &ctx);
    guint bad = 0;
    gint64 t0 = g_get_monotonic_time();
    for (guint i = 0; i < 200000; i++) {
        gchar **argv = pcv_repl_ssh_argv("pcv", "node-b", cmd);
        const gchar *khf = argv ? argv[8] : NULL;
        if (!khf || (!g_str_equal(khf, "UserKnownHostsFile=" SEAM_PATH_A) &&
                     !g_str_equal(khf, "UserKnownHostsFile=" SEAM_PATH_B)))
            bad++;
        g_strfreev(argv);
        if ((i & 0xfu) == 0xfu) {
                                                                               
                                                                         
            g_usleep(50);
            gint64 elapsed = g_get_monotonic_time() - t0;
            if (elapsed >= 30 * G_USEC_PER_SEC)
                break;
            if (elapsed >= 2 * G_USEC_PER_SEC && g_atomic_int_get(&ctx.flips) >= 100)
                break;
        }
    }
    g_atomic_int_set(&ctx.stop, 1);
    g_thread_join(t);
    pcv_repl_known_hosts_set_path_for_test(NULL);
    g_test_message("seam concurrent: flips=%d bad=%u", g_atomic_int_get(&ctx.flips), bad);
    g_assert_cmpuint(bad, ==, 0);
    g_assert_cmpint(g_atomic_int_get(&ctx.flips), >=, 100);
}

                                                 
static void
test_probe_argv_runs_true(void)
{
    gchar **argv = pcv_repl_probe_argv("pcv", "10.0.0.5");
    g_assert_nonnull(argv);
    guint n = g_strv_length(argv);
    g_assert_cmpstr(argv[n - 2], ==, "pcv@10.0.0.5");
    g_assert_cmpstr(argv[n - 1], ==, "true");
    g_assert_true(has_pair(argv, "-o", "HostKeyAlias=10.0.0.5"));
    g_strfreev(argv);
}

                                                      
static void
test_canonical_target(void)
{
    gchar *c = pcv_repl_canonical_target("Node-B.Example");
    g_assert_cmpstr(c, ==, "node-b.example");
    g_free(c);
    c = pcv_repl_canonical_target("10.0.0.5");
    g_assert_cmpstr(c, ==, "10.0.0.5");
    g_free(c);
    g_assert_null(pcv_repl_canonical_target("node_b"));
    g_assert_null(pcv_repl_canonical_target("10.0.0.5:22"));
    g_assert_null(pcv_repl_canonical_target(""));
    g_assert_null(pcv_repl_canonical_target(NULL));
                                                            
    const gchar *bad[] = { "a,b", "node*", "n?de", "#x", "[h]", "h!", "h|x", "u@h", "a\nb" };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++)
        g_assert_null(pcv_repl_canonical_target(bad[i]));
}

                                                     
static void
test_fingerprint_matches_ssh_keygen(void)
{
    gchar *fp = pcv_repl_fingerprint(KEY_A);
    g_assert_cmpstr(fp, ==, FP_A);
    g_free(fp);
    fp = pcv_repl_fingerprint(KEY_E);
    g_assert_cmpstr(fp, ==, FP_E);
    g_free(fp);
    g_assert_null(pcv_repl_fingerprint("not*base64ok"));
    g_assert_null(pcv_repl_fingerprint("AAAA=AAA"));
    g_assert_null(pcv_repl_fingerprint(""));
    g_assert_null(pcv_repl_fingerprint(NULL));
}

                                                                      
static void
test_known_hosts_ignores_malformed_lines(void)
{
    kh_setup();
    GError *err = NULL;
    g_assert_null(pcv_repl_hostkey_lookup("node-c", &err));
    g_assert_no_error(err);
    const gchar *text =
        "# comment\n"
        "\n"
        "node-c ssh-ed25519 " KEY_B "\n"
        "Node-D ssh-ed25519 " KEY_A "\n"
        "node-e ssh-rsa " KEY_A "\n"
        "node-f ssh-ed25519 !!!!\n"
        "node-c ssh-ed25519 " KEY_A "\n"
        "garbage\n";
    g_assert_true(g_file_set_contents(g_kh_file, text, -1, NULL));
    PcvReplHostKey *hit = pcv_repl_hostkey_lookup("NODE-C", &err);
    g_assert_no_error(err);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->target, ==, "node-c");
    g_assert_cmpstr(hit->type, ==, "ssh-ed25519");
    g_assert_cmpstr(hit->fingerprint, ==, FP_B);
    pcv_repl_host_key_free(hit);
    g_assert_null(pcv_repl_hostkey_lookup("node-d", &err));
    g_assert_null(pcv_repl_hostkey_lookup("node-e", &err));
    g_assert_null(pcv_repl_hostkey_lookup("node-f", &err));
    g_assert_no_error(err);
    g_assert_null(pcv_repl_hostkey_lookup("bad host", &err));
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_INVALID);
    g_clear_error(&err);
    g_assert_true(pcv_repl_hostkey_require("node-c", &err));
    g_assert_no_error(err);
    g_assert_false(pcv_repl_hostkey_require("node-z", &err));
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_UNTRUSTED);
    g_assert_true(g_str_has_prefix(err->message, "HOSTKEY_UNTRUSTED:"));
    g_assert_nonnull(strstr(err->message, "pcvctl backup hostkey scan node-z"));
    g_clear_error(&err);
    kh_teardown();
}

                                                                            
static void
test_classify_ssh_failures(void)
{
    g_assert_cmpint(pcv_repl_classify_ssh_stderr("Host key verification failed.\r\n"), ==,
                    PCV_REPL_HOSTKEY_ERROR_MISMATCH);
    g_assert_cmpint(pcv_repl_classify_ssh_stderr(
        "No ED25519 host key is known for node-b and you have requested strict checking.\r\n"
        "Host key verification failed.\r\n"), ==, PCV_REPL_HOSTKEY_ERROR_MISMATCH);
    g_assert_cmpint(pcv_repl_classify_ssh_stderr(
        "@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@\nHost key verification failed.\n"), ==,
        PCV_REPL_HOSTKEY_ERROR_MISMATCH);
    g_assert_cmpint(pcv_repl_classify_ssh_stderr("ssh: connect to host node-b port 22: Connection refused\r\n"),
                    ==, PCV_REPL_HOSTKEY_ERROR_FAILED);
    g_assert_cmpint(pcv_repl_classify_ssh_stderr("pcv@node-b: Permission denied (publickey).\r\n"), ==,
                    PCV_REPL_HOSTKEY_ERROR_FAILED);
    g_assert_cmpint(pcv_repl_classify_ssh_stderr(NULL), ==, PCV_REPL_HOSTKEY_ERROR_FAILED);
    GError *err = NULL;
    pcv_repl_set_ssh_error(&err, "node-b", "Host key verification failed.\r\n");
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_MISMATCH);
    g_assert_true(g_str_has_prefix(err->message, "HOSTKEY_MISMATCH:"));
    g_assert_nonnull(strstr(err->message, "pcvctl backup hostkey remove node-b"));
    g_clear_error(&err);
    pcv_repl_set_ssh_error(&err, "node-b", "ssh: connect to host node-b port 22: Connection refused\r\n");
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_FAILED);
    g_assert_nonnull(strstr(err->message, "Connection refused"));
    g_clear_error(&err);
}

                                                                                         
static void
test_parse_keyscan_output(void)
{
    const gchar *out =
        "# node-b:22 SSH-2.0-OpenSSH_9.6\n"
        "node-b ssh-ed25519 " KEY_A "\n"
        "node-b ecdsa-sha2-nistp256 " KEY_E "\r\n"
        "node-b ssh-rsa " KEY_A "\n"
        "node-b ssh-ed25519 AAAA@@@@\n"
        "node-b ssh-ed25519\n"
        "node-b ssh-dss " KEY_A "\n"
        "node-b ssh-ed25519 " KEY_A "\n";
    GPtrArray *keys = pcv_repl_parse_keyscan(out);
    g_assert_cmpuint(keys->len, ==, 2);
    const PcvReplHostKey *k0 = g_ptr_array_index(keys, 0);
    const PcvReplHostKey *k1 = g_ptr_array_index(keys, 1);
    g_assert_cmpstr(k0->type, ==, "ssh-ed25519");
    g_assert_cmpstr(k0->fingerprint, ==, FP_A);
    g_assert_null(k0->target);
    g_assert_cmpstr(k1->type, ==, "ecdsa-sha2-nistp256");
    g_assert_cmpstr(k1->fingerprint, ==, FP_E);
    g_ptr_array_unref(keys);
    keys = pcv_repl_parse_keyscan(NULL);
    g_assert_cmpuint(keys->len, ==, 0);
    g_ptr_array_unref(keys);
    g_assert_true(pcv_repl_fingerprint_valid(FP_A));
    g_assert_false(pcv_repl_fingerprint_valid("SHA256:abc"));
    g_assert_false(pcv_repl_fingerprint_valid("MD5:rBbYAUGcs8skM8lqu8SNzYTuKrv89qMai86nLA2VFZE"));
    g_assert_false(pcv_repl_fingerprint_valid("SHA256:rBbYAUGcs8skM8lqu8SNzYTuKrv89qMai86nLA2VFZ="));
    g_assert_false(pcv_repl_fingerprint_valid(NULL));
}

                               
static PcvReplHostKey *
key_of(const gchar *type, const gchar *b64)
{
    gchar *line = g_strdup_printf("x %s %s\n", type, b64);
    GPtrArray *keys = pcv_repl_parse_keyscan(line);
    g_assert_cmpuint(keys->len, ==, 1);
    PcvReplHostKey *k = g_ptr_array_steal_index(keys, 0);
    g_ptr_array_unref(keys);
    g_free(line);
    return k;
}

                                                            
static void
test_known_hosts_add_lookup_remove(void)
{
    kh_setup();
    GError *err = NULL;
    gboolean added = FALSE;
    PcvReplHostKey *a = key_of("ssh-ed25519", KEY_A);
    PcvReplHostKey *b = key_of("ssh-ed25519", KEY_B);
    g_assert_true(pcv_repl_hostkey_add("Node-B", a, &added, &err));
    g_assert_no_error(err);
    g_assert_true(added);
    g_assert_true(pcv_repl_hostkey_add("node-b", a, &added, &err));
    g_assert_no_error(err);
    g_assert_false(added);
    g_assert_false(pcv_repl_hostkey_add("node-b", b, &added, &err));
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_CONFLICT);
    g_clear_error(&err);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(g_kh_file, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "node-b ssh-ed25519 " KEY_A "\n");
    g_free(text);
    GStatBuf st;
    g_assert_cmpint(g_stat(g_kh_file, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    PcvReplHostKey *hit = pcv_repl_hostkey_lookup("NODE-B", &err);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->fingerprint, ==, FP_A);
    pcv_repl_host_key_free(hit);
    GPtrArray *all = pcv_repl_hostkey_list(&err);
    g_assert_no_error(err);
    g_assert_cmpuint(all->len, ==, 1);
    g_ptr_array_unref(all);
                                                
    g_assert_true(pcv_repl_hostkey_add("node-c", b, &added, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_repl_hostkey_remove("node-b", &err));
    g_assert_no_error(err);
    hit = pcv_repl_hostkey_lookup("node-c", &err);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->fingerprint, ==, FP_B);
    pcv_repl_host_key_free(hit);
    g_assert_null(pcv_repl_hostkey_lookup("node-b", &err));
    g_clear_error(&err);
    g_assert_true(pcv_repl_hostkey_remove("node-c", &err));
    g_assert_no_error(err);
    g_assert_false(pcv_repl_hostkey_remove("node-b", &err));
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_NOT_FOUND);
    g_clear_error(&err);
    g_assert_false(pcv_repl_hostkey_remove("bad host", &err));
    g_assert_error(err, PCV_REPL_HOSTKEY_ERROR, PCV_REPL_HOSTKEY_ERROR_INVALID);
    g_clear_error(&err);
    pcv_repl_host_key_free(a);
    pcv_repl_host_key_free(b);
    kh_teardown();
}

                                                                                      
static void
test_audit_target_from_target_node(void)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "target_node", "node-b");
    g_assert_cmpstr(pcv_disp_dispatcher_audit_target_from_params("backup.replication.hostkey.remove", p), ==, "node-b");
    json_object_unref(p);
}

                                     
static void
test_rpc_code_mapping(void)
{
    const struct { gint code; gint rpc; } t[] = {
        { PCV_REPL_HOSTKEY_ERROR_INVALID,   PURE_RPC_ERR_INVALID_PARAMS },
        { PCV_REPL_HOSTKEY_ERROR_UNTRUSTED, PURE_RPC_ERR_HOSTKEY_UNTRUSTED },
        { PCV_REPL_HOSTKEY_ERROR_MISMATCH,  PURE_RPC_ERR_HOSTKEY_MISMATCH },
        { PCV_REPL_HOSTKEY_ERROR_CONFLICT,  PURE_RPC_ERR_CONFLICT },
        { PCV_REPL_HOSTKEY_ERROR_NOT_FOUND, PURE_RPC_ERR_NOT_FOUND },
        { PCV_REPL_HOSTKEY_ERROR_FAILED,    PURE_RPC_ERR_ZFS_OPERATION },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(t); i++) {
        GError *e = g_error_new_literal(PCV_REPL_HOSTKEY_ERROR, t[i].code, "x");
        g_assert_cmpint(pcv_repl_hostkey_rpc_code(e), ==, t[i].rpc);
        g_error_free(e);
    }
    GError *io = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "x");
    g_assert_cmpint(pcv_repl_hostkey_rpc_code(io), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_error_free(io);
    g_assert_cmpint(pcv_repl_hostkey_rpc_code(NULL), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_cmpint(PURE_RPC_ERR_HOSTKEY_UNTRUSTED, ==, -32012);
    g_assert_cmpint(PURE_RPC_ERR_HOSTKEY_MISMATCH, ==, -32013);
}

void
test_replication_hostkey_register(void)
{
    g_test_add_func("/replication_hostkey/ssh_argv/required_options", test_ssh_argv_required_options);
    g_test_add_func("/replication_hostkey/ssh_argv/rejects_bad_input", test_ssh_argv_rejects_bad_input);
    g_test_add_func("/replication_hostkey/ssh_argv/path_seam_concurrent", test_ssh_argv_path_seam_concurrent);
    g_test_add_func("/replication_hostkey/probe_argv/runs_true",       test_probe_argv_runs_true);
    g_test_add_func("/replication_hostkey/canonical_target",           test_canonical_target);
    g_test_add_func("/replication_hostkey/fingerprint/ssh_keygen",     test_fingerprint_matches_ssh_keygen);
    g_test_add_func("/replication_hostkey/known_hosts/malformed",      test_known_hosts_ignores_malformed_lines);
    g_test_add_func("/replication_hostkey/classify_ssh_failures",      test_classify_ssh_failures);
    g_test_add_func("/replication_hostkey/parse_keyscan",              test_parse_keyscan_output);
    g_test_add_func("/replication_hostkey/known_hosts/add_lookup_remove", test_known_hosts_add_lookup_remove);
    g_test_add_func("/replication_hostkey/audit_target_node",          test_audit_target_from_target_node);
    g_test_add_func("/replication_hostkey/rpc_code_mapping",           test_rpc_code_mapping);
}
