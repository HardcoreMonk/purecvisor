                                                                                                    
                                                                                              
                                                                                                 
                                                                                    
                                                                              
                                                                                  
   
                         
                                                        
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "modules/network/vpc/vpc_backend_ovn.h"
#include "modules/network/vpc/vpc_fence.h"
#include "modules/network/vpc/vpc_model.h"
#include "modules/network/vpc/vpc_store.h"
#include "utils/pcv_spawn.h"

typedef struct {
    gchar *dir;
    gchar *state;                                                                     
    gchar *nb_log;
    gchar *nft_log;
    gchar *ip_log;
    gchar *saved_path;
    PcvVpcStore *store;
} FenceFx;

static const gchar MOCK_NBCTL[] =
    "#!/bin/sh\n"
    "D=\"@STATE@\"\n"
    "printf '%s\\n' \"$*\" >> \"$D/nb.log\"\n"
    "[ -e \"$D/nb-down\" ] && { echo 'database connection failed' >&2; exit 1; }\n"
    "pg=''; prev=''\n"
    "for a in \"$@\"; do\n"
    "  case \"$prev\" in Port_Group|acl-add|acl-del) pg=$a ;; esac\n"
    "  prev=$a\n"
    "done\n"
    "case \" $* \" in\n"
    "  *' get Port_Group '*)\n"
    "    [ -e \"$D/pg-$pg\" ] || exit 0\n"
    "    case \" $* \" in\n"
    "      *' _uuid '*) echo 11111111-1111-1111-1111-111111111111 ;;\n"
    "      *purecvisor-owner*) cut -d' ' -f1 \"$D/pg-$pg\" ;;\n"
    "      *purecvisor-vpc-id*) cut -d' ' -f2 \"$D/pg-$pg\" ;;\n"
    "    esac ;;\n"
    "  *' acl-add '*|*' acl-del '*)\n"
    "    [ -e \"$D/pg-$pg\" ] || { echo \"ovn-nbctl: $pg: port group name not found\" >&2; exit 1; }\n"
                                                                                     
    "    [ -e \"$D/wait-timeout\" ] && { echo 'terminating with signal 14 (Alarm clock)' >&2; exit 142; } ;;\n"
    "esac\n"
    "exit 0\n";

static void
_exec(const gchar *dir, const gchar *name, const gchar *body)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, body, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
    g_free(path);
}

static void
_fx_setup(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    pcv_spawn_launcher_shutdown();
    f->dir = g_dir_make_tmp("pcv-vpc-fence-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->state = g_build_filename(f->dir, "state", NULL);
    f->nb_log = g_build_filename(f->state, "nb.log", NULL);
    f->nft_log = g_build_filename(f->state, "nft.log", NULL);
    f->ip_log = g_build_filename(f->state, "ip.log", NULL);
    g_assert_cmpint(g_mkdir(f->state, 0700), ==, 0);
    gchar *bin = g_build_filename(f->dir, "bin", NULL);
    g_assert_cmpint(g_mkdir(bin, 0700), ==, 0);

    GString *nbctl = g_string_new(MOCK_NBCTL);
    g_string_replace(nbctl, "@STATE@", f->state, 0);
    _exec(bin, "ovn-nbctl", nbctl->str);
    g_string_free(nbctl, TRUE);
                                                               
    gchar *nft = g_strdup_printf(
        "#!/bin/sh\n[ -e '%s/nft-fail' ] && exit 1\n"
        "case \" $* \" in *' -c '*) cat >/dev/null ;; "
        "*) echo '=== apply' >> '%s'; cat >> '%s' ;; esac\nexit 0\n",
        f->state, f->nft_log, f->nft_log);
    _exec(bin, "nft", nft);
    g_free(nft);
    gchar *ip = g_strdup_printf("#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%s'\nexit 0\n", f->ip_log);
    _exec(bin, "ip", ip);
    g_free(ip);
    _exec(bin, "ovs-vsctl", "#!/bin/sh\nexit 0\n");
    _exec(bin, "ovn-sbctl", "#!/bin/sh\nexit 1\n");

    f->saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, f->saved_path ? f->saved_path : "");
    g_setenv("PATH", mock_path, TRUE);
    g_free(mock_path);
    g_free(bin);

    gchar *db = g_build_filename(f->dir, "vpc.db", NULL);
    f->store = pcv_vpc_store_open(db, NULL);
    g_assert_nonnull(f->store);
    g_free(db);
}

static void
_remove_tree(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            gchar *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR)) _remove_tree(child);
            else g_remove(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static void
_fx_teardown(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    pcv_vpc_store_free(f->store);
    if (f->saved_path) g_setenv("PATH", f->saved_path, TRUE); else g_unsetenv("PATH");
    pcv_spawn_launcher_shutdown();
    _remove_tree(f->dir);
    g_free(f->saved_path); g_free(f->ip_log); g_free(f->nft_log); g_free(f->nb_log);
    g_free(f->state); g_free(f->dir);
}

static gchar *
_ovn_vpc(FenceFx *f, const gchar *name, const gchar *state)
{
    gchar *id = NULL;
    gint64 revision = 0;
    GError *error = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, name, "t-b", "nat", "ovn", &id, &revision, &error));
    g_assert_no_error(error);
    JsonObject *binding = pcv_vpc_store_ensure_ovn_binding(f->store, id, "t-b", "100.64.0.0/16", &error);
    g_assert_no_error(error);
    json_object_unref(binding);
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "vpcs", id, state, NULL, &error));
    return id;
}

static gchar *
_pg_name(const gchar *vpc_id)
{
    gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, vpc_id, -1);
    gchar *pg = g_strdup_printf("pcvv_pg_%.16s", digest);
    g_free(digest);
    return pg;
}

                                                                             
static void
_pg_add(FenceFx *f, const gchar *vpc_id, const gchar *owner)
{
    gchar *pg = _pg_name(vpc_id);
    gchar *leaf = g_strdup_printf("pg-%s", pg);
    gchar *path = g_build_filename(f->state, leaf, NULL);
    gchar *value = g_strdup_printf("%s %s\n", owner, vpc_id);
    g_assert_true(g_file_set_contents(path, value, -1, NULL));
    g_free(value); g_free(path); g_free(leaf); g_free(pg);
}

static void
_nb_down(FenceFx *f)
{
    gchar *path = g_build_filename(f->state, "nb-down", NULL);
    g_assert_true(g_file_set_contents(path, "", 0, NULL));
    g_free(path);
}

static gboolean
_log_has(const gchar *path, const gchar *needle)
{
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return FALSE;
    gboolean found = strstr(text, needle) != NULL;
    g_free(text);
    return found;
}

static JsonObject *
_get_vpc(FenceFx *f, const gchar *id)
{
    JsonObject *vpc = pcv_vpc_store_get_vpc(f->store, id, NULL, NULL);
    g_assert_nonnull(vpc);
    return vpc;
}

                                                                       
                     
static gboolean
_last_nft_is_full_policy(FenceFx *f)
{
    gchar *text = NULL;
    if (!g_file_get_contents(f->nft_log, &text, NULL, NULL)) return FALSE;
    const gchar *last = g_strrstr(text, "=== apply");
    gboolean full = last && strstr(last, "add table inet pcv_vpc\n") &&
        !strstr(last, "add rule inet pcv_vpc_quarantine");
    g_free(text);
    return full;
}

                                                                      
static void
test_absent_passes(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "DEGRADED");
    JsonObject *vpc = _get_vpc(f, id);
    JsonArray *attachments = json_array_new();
    GError *error = NULL;
    g_assert_true(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_vpc_ovn_set_quarantine(vpc, FALSE, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_no_error(error);
    g_assert_false(_log_has(f->nb_log, "acl-add"));
    g_assert_false(_log_has(f->nb_log, "acl-del"));
    json_array_unref(attachments);
    json_object_unref(vpc);
    g_free(id);
}

                                                                                     
                                                                              
static void
test_absent_with_active_attachment_fails(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    JsonObject *vpc = _get_vpc(f, id);
    JsonArray *attachments = json_array_new();
    JsonObject *attachment = json_object_new();
    json_object_set_string_member(attachment, "state", "DETACHING");
    json_array_add_object_element(attachments, attachment);
    GError *error = NULL;
    g_assert_true(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_no_error(error);
    json_object_set_string_member(attachment, "state", "ACTIVE");
    g_assert_false(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_PORT_GROUP_MISSING);
    g_clear_error(&error);
    g_assert_true(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_assert_no_error(error);
    g_assert_false(_log_has(f->nb_log, "acl-"));
    json_array_unref(attachments);
    json_object_unref(vpc);
    g_free(id);
}

                                                                                          
static void
_active_attachment(FenceFx *f, const gchar *vpc_id)
{
    gchar *subnet = NULL, *ref = NULL;
    gint64 subnet_revision = 0;
    GError *error = NULL;
    JsonObject *vpc = _get_vpc(f, vpc_id);
    const gint64 revision = json_object_get_int_member(vpc, "revision");
    json_object_unref(vpc);
    g_assert_true(pcv_vpc_store_create_subnet(f->store, vpc_id, "t-b", "s1", "10.202.1.0/24", 1500,
                                              revision, &subnet, &ref, &subnet_revision, &error));
    g_assert_no_error(error);
    JsonObject *attachment = NULL;
    g_assert_true(pcv_vpc_store_allocate_attachment(f->store, subnet, "t-b",
        "aaaaaaaa-0000-4000-8000-000000000001", "vm-a", "admin", NULL, &attachment, &error));
    g_assert_no_error(error);
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "attachments",
        json_object_get_string_member(attachment, "id"), "ACTIVE", NULL, &error));
    json_object_unref(attachment);
    g_free(ref); g_free(subnet);
}

static void
_assert_vpc_state(FenceFx *f, const gchar *vpc_id, const gchar *want)
{
    JsonObject *vpc = _get_vpc(f, vpc_id);
    g_assert_cmpstr(json_object_get_string_member(vpc, "state"), ==, want);
    json_object_unref(vpc);
}

                                                                                           
                                                                   
                                                          
static void
test_clear_degrades_non_target(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");
    gchar *target = _ovn_vpc(f, "b-target", "ACTIVE");
    _pg_add(f, target, "local-vpc");
    _active_attachment(f, broken);
    GError *error = NULL;
    g_assert_true(pcv_vpc_fence_clear_ovn(f->store, target, &error));
    g_assert_no_error(error);
    _assert_vpc_state(f, broken, "DEGRADED");
    _assert_vpc_state(f, target, "ACTIVE");
    gchar *edge_broken = pcv_vpc_ovn_edge_iface_name_from_id(broken);
    gchar *edge_target = pcv_vpc_ovn_edge_iface_name_from_id(target);
    g_assert_true(_log_has(f->ip_log, edge_broken));
    g_assert_false(_log_has(f->ip_log, edge_target));
    gchar *pg = _pg_name(target);
    gchar *del = g_strdup_printf("acl-del %s", pg);
    g_assert_true(_log_has(f->nb_log, del));
    g_free(del); g_free(pg); g_free(edge_target); g_free(edge_broken); g_free(target); g_free(broken);
}

                                                  
static void
test_clear_null_target_degrades(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");
    _active_attachment(f, broken);
    GError *error = NULL;
    g_assert_true(pcv_vpc_fence_clear_ovn(f->store, NULL, &error));
    g_assert_no_error(error);
    _assert_vpc_state(f, broken, "DEGRADED");
    g_free(broken);
}

                                                                                  
static void
test_clear_target_itself_fails(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");
    _active_attachment(f, broken);
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_clear_ovn(f->store, broken, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_PORT_GROUP_MISSING);
    g_clear_error(&error);
    _assert_vpc_state(f, broken, "DEGRADED");
    gchar *edge = pcv_vpc_ovn_edge_iface_name_from_id(broken);
    g_assert_true(_log_has(f->ip_log, edge));
    g_free(edge);
    g_free(broken);
}

                                                                                   
static void
test_policy_sync_failed_outcomes(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *missing = _ovn_vpc(f, "a-missing", "ACTIVE");
    gchar *other = _ovn_vpc(f, "b-other", "ACTIVE");
    GError *cause = g_error_new_literal(PCV_VPC_ERROR, PCV_VPC_ERROR_PORT_GROUP_MISSING, "no pg");
    pcv_vpc_fence_policy_sync_failed(f->store, missing, cause);
    g_error_free(cause);
    cause = g_error_new_literal(PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "nb down");
    pcv_vpc_fence_policy_sync_failed(f->store, other, cause);
    g_error_free(cause);
    _assert_vpc_state(f, missing, "DEGRADED");
    _assert_vpc_state(f, other, "DEGRADED");
    gchar *edge_missing = pcv_vpc_ovn_edge_iface_name_from_id(missing);
    gchar *edge_other = pcv_vpc_ovn_edge_iface_name_from_id(other);
    g_assert_true(_log_has(f->ip_log, edge_missing));
    g_assert_false(_log_has(f->ip_log, edge_other));
    g_free(edge_other); g_free(edge_missing); g_free(other); g_free(missing);
}

                                                                    
static void
test_clear_nb_outage_still_fails(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *other = _ovn_vpc(f, "a-other", "ACTIVE");
    _active_attachment(f, other);
    _nb_down(f);
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_clear_ovn(f->store, "aaaaaaaa-0000-4000-8000-00000000ffff", &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_clear_error(&error);
    _assert_vpc_state(f, other, "ACTIVE");
    g_assert_false(g_file_test(f->ip_log, G_FILE_TEST_EXISTS));
    g_free(other);
}

                                                
static void
test_release_only_logs_missing_port_group(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");
    _active_attachment(f, broken);
    pcv_vpc_fence_release(f->store, NULL);
    _assert_vpc_state(f, broken, "ACTIVE");
    g_assert_false(g_file_test(f->ip_log, G_FILE_TEST_EXISTS));
    g_free(broken);
}

                                                                               
static void
test_reconcile_clear_degrades_only_that_vpc(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");
    gchar *healthy = _ovn_vpc(f, "b-healthy", "ACTIVE");
    _pg_add(f, healthy, "local-vpc");
    GError *error = NULL;
    JsonObject *vpc = NULL;
    _active_attachment(f, broken);

    GHashTable *failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_assert_true(pcv_vpc_fence_reconcile_ovn_clear(f->store, failed, &error));
    g_assert_no_error(error);
    g_assert_true(g_hash_table_contains(failed, broken));
    g_assert_false(g_hash_table_contains(failed, healthy));
    vpc = _get_vpc(f, broken);
    g_assert_cmpstr(json_object_get_string_member(vpc, "state"), ==, "DEGRADED");
    json_object_unref(vpc);
    vpc = _get_vpc(f, healthy);
    g_assert_cmpstr(json_object_get_string_member(vpc, "state"), ==, "ACTIVE");
    json_object_unref(vpc);
    gchar *pg = _pg_name(healthy);
    gchar *del = g_strdup_printf("acl-del %s", pg);
    g_assert_true(_log_has(f->nb_log, del));
    g_free(del); g_free(pg);
    g_hash_table_unref(failed);
    g_free(healthy); g_free(broken);
}

                                                                 
static void
test_owned_applies_acl(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _pg_add(f, id, "local-vpc");
    JsonObject *vpc = _get_vpc(f, id);
    JsonArray *attachments = json_array_new();
    GError *error = NULL;
    g_assert_true(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_assert_true(_log_has(f->nb_log, "from-lport 30000"));
    g_assert_true(_log_has(f->nb_log, "to-lport 30000"));
    g_assert_true(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_true(_log_has(f->nb_log, "acl-del"));
    g_assert_no_error(error);
    json_array_unref(attachments);
    json_object_unref(vpc);
    g_free(id);
}

                                                         
static void
test_foreign_conflict(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _pg_add(f, id, "someone-else");
    JsonObject *vpc = _get_vpc(f, id);
    JsonArray *attachments = json_array_new();
    GError *error = NULL;
    g_assert_false(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    g_assert_false(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    g_assert_false(_log_has(f->nb_log, "acl-add"));
    g_assert_false(_log_has(f->nb_log, "acl-del"));
    json_array_unref(attachments);
    json_object_unref(vpc);
    g_free(id);
}

                                                     
static void
test_nb_outage_is_failure(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    JsonObject *vpc = _get_vpc(f, id);
    JsonArray *attachments = json_array_new();
    _nb_down(f);
    GError *error = NULL;
    g_assert_false(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_clear_error(&error);
    g_assert_false(pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_clear_error(&error);
    json_array_unref(attachments);
    json_object_unref(vpc);
    g_free(id);
}

                                                                      
                                                                
static void
test_quarantine_failure_rolls_back(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *first = _ovn_vpc(f, "a-first", "ACTIVE");                       
    gchar *second = _ovn_vpc(f, "b-second", "ACTIVE");
    _pg_add(f, first, "local-vpc");
    _pg_add(f, second, "someone-else");
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    gchar *pg = _pg_name(first);
    gchar *add = g_strdup_printf("acl-add %s from-lport 30000", pg);
    gchar *del = g_strdup_printf("acl-del %s from-lport 30000", pg);
    g_assert_true(_log_has(f->nb_log, add));
    g_assert_true(_log_has(f->nb_log, del));
    g_assert_false(g_file_test(f->nft_log, G_FILE_TEST_EXISTS));                            
    g_free(del); g_free(add); g_free(pg); g_free(second); g_free(first);
}

                                                                                        
static void
test_unquarantine_tries_both_directions(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _pg_add(f, id, "local-vpc");
    JsonObject *vpc = _get_vpc(f, id);
    gchar *marker = g_build_filename(f->state, "wait-timeout", NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    GError *error = NULL;
    g_assert_false(pcv_vpc_ovn_set_quarantine(vpc, FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *pg = _pg_name(id);
    gchar *from = g_strdup_printf("acl-del %s from-lport 30000", pg);
    gchar *to = g_strdup_printf("acl-del %s to-lport 30000", pg);
    g_assert_true(_log_has(f->nb_log, from));
    g_assert_true(_log_has(f->nb_log, to));
                            
    g_assert_cmpint(g_remove(f->nb_log), ==, 0);
    g_assert_false(pcv_vpc_ovn_set_quarantine(vpc, TRUE, &error));
    g_clear_error(&error);
    gchar *add_to = g_strdup_printf("acl-add %s to-lport 30000", pg);
    g_assert_false(_log_has(f->nb_log, add_to));
    g_free(add_to); g_free(to); g_free(from); g_free(pg); g_free(marker);
    json_object_unref(vpc);
    g_free(id);
}

                                                                             
static void
test_quarantine_failure_releases_failed_vpc(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _pg_add(f, id, "local-vpc");
    gchar *marker = g_build_filename(f->state, "wait-timeout", NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *pg = _pg_name(id);
    gchar *add = g_strdup_printf("acl-add %s from-lport 30000", pg);
    gchar *del_from = g_strdup_printf("acl-del %s from-lport 30000", pg);
    gchar *del_to = g_strdup_printf("acl-del %s to-lport 30000", pg);
    g_assert_true(_log_has(f->nb_log, add));
    g_assert_true(_log_has(f->nb_log, del_from));
    g_assert_true(_log_has(f->nb_log, del_to));
    g_free(del_to); g_free(del_from); g_free(add); g_free(pg); g_free(marker); g_free(id);
}

                                                                                               
                                                               
static void
test_quarantine_failure_keeps_degraded(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *degraded = _ovn_vpc(f, "a-degraded", "DEGRADED");
    gchar *active = _ovn_vpc(f, "b-active", "ACTIVE");
    gchar *foreign = _ovn_vpc(f, "c-foreign", "ACTIVE");
    _pg_add(f, degraded, "local-vpc");
    _pg_add(f, active, "local-vpc");
    _pg_add(f, foreign, "someone-else");
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    gchar *dpg = _pg_name(degraded);
    gchar *apg = _pg_name(active);
    gchar *dadd = g_strdup_printf("acl-add %s from-lport 30000", dpg);
    gchar *ddel = g_strdup_printf("acl-del %s", dpg);
    gchar *adel = g_strdup_printf("acl-del %s from-lport 30000", apg);
    g_assert_true(_log_has(f->nb_log, dadd));
    g_assert_false(_log_has(f->nb_log, ddel));
    g_assert_true(_log_has(f->nb_log, adel));
    g_free(adel); g_free(ddel); g_free(dadd); g_free(apg); g_free(dpg);
    g_free(foreign); g_free(active); g_free(degraded);
}

                                                     
static void
test_quarantine_nb_outage_leaves_nft(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *ovn = _ovn_vpc(f, "ovn-a", "ACTIVE");
    gchar *linux_vpc = NULL, *subnet = NULL, *bridge = NULL;
    gint64 revision = 0, subnet_revision = 0;
    GError *error = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "linux-a", "t-a", "nat", "linux",
                                           &linux_vpc, &revision, &error));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, linux_vpc, "t-a", "s1", "10.201.1.0/24", 1500,
                                              revision, &subnet, &bridge, &subnet_revision, &error));
    g_assert_no_error(error);
    _nb_down(f);
    g_assert_false(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_clear_error(&error);
    g_assert_false(g_file_test(f->nft_log, G_FILE_TEST_EXISTS));
    g_free(bridge); g_free(subnet); g_free(linux_vpc); g_free(ovn);
}

                                                             
static void
test_quarantine_nft_failure_releases_acl(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *ovn = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _pg_add(f, ovn, "local-vpc");
    gchar *linux_vpc = NULL, *subnet = NULL, *bridge = NULL;
    gint64 revision = 0, subnet_revision = 0;
    GError *error = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "linux-a", "t-a", "nat", "linux",
                                           &linux_vpc, &revision, &error));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, linux_vpc, "t-a", "s1", "10.201.1.0/24", 1500,
                                              revision, &subnet, &bridge, &subnet_revision, &error));
    g_assert_no_error(error);
    gchar *marker = g_build_filename(f->state, "nft-fail", NULL);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    g_assert_false(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *pg = _pg_name(ovn);
    gchar *add = g_strdup_printf("acl-add %s from-lport 30000", pg);
    gchar *del = g_strdup_printf("acl-del %s from-lport 30000", pg);
    g_assert_true(_log_has(f->nb_log, add));
    g_assert_true(_log_has(f->nb_log, del));
    g_free(del); g_free(add); g_free(pg); g_free(marker);
    g_free(bridge); g_free(subnet); g_free(linux_vpc); g_free(ovn);
}

                                                     
                                              
static void
test_clear_continues_past_failure(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "a-broken", "ACTIVE");                       
    gchar *healthy = _ovn_vpc(f, "b-healthy", "ACTIVE");
    _pg_add(f, broken, "someone-else");
    _pg_add(f, healthy, "local-vpc");
    GError *error = NULL;
    g_assert_false(pcv_vpc_fence_clear_ovn(f->store, healthy, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    gchar *pg = _pg_name(healthy);
    gchar *del = g_strdup_printf("acl-del %s", pg);
    g_assert_true(_log_has(f->nb_log, del));
    g_free(del); g_free(pg); g_free(healthy); g_free(broken);
}

                                                                                         
static void
test_release_restores_except_keep(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *kept = _ovn_vpc(f, "a-kept", "DEGRADED");
    gchar *other = _ovn_vpc(f, "b-other", "ACTIVE");
    _pg_add(f, kept, "local-vpc");
    _pg_add(f, other, "local-vpc");
    GError *error = NULL;
    g_assert_true(pcv_vpc_fence_quarantine(f->store, NULL, &error));
    g_assert_no_error(error);
    gchar *kept_pg = _pg_name(kept);
    gchar *other_pg = _pg_name(other);
    gchar *kept_del = g_strdup_printf("acl-del %s", kept_pg);
    gchar *other_del = g_strdup_printf("acl-del %s", other_pg);
    pcv_vpc_fence_release(f->store, kept);
    g_assert_true(_last_nft_is_full_policy(f));
    g_assert_true(_log_has(f->nb_log, other_del));
    g_assert_false(_log_has(f->nb_log, kept_del));
    pcv_vpc_fence_release(f->store, NULL);
    g_assert_true(_log_has(f->nb_log, kept_del));
    g_free(other_del); g_free(kept_del); g_free(other_pg); g_free(kept_pg); g_free(other); g_free(kept);
}

                                                               
static void
test_quarantine_skips_self(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *self = _ovn_vpc(f, "a-self", "DEGRADED");
    gchar *other = _ovn_vpc(f, "b-other", "ACTIVE");
    _pg_add(f, other, "local-vpc");
    gchar *self_pg = _pg_name(self);
    gchar *other_pg = _pg_name(other);
    GError *error = NULL;
    g_assert_true(pcv_vpc_fence_quarantine(f->store, self, &error));
    g_assert_no_error(error);
    g_assert_false(_log_has(f->nb_log, self_pg));
    g_assert_true(_log_has(f->nb_log, other_pg));
    g_free(other_pg); g_free(self_pg); g_free(other); g_free(self);
}

                                                                               
static void
test_reconcile_begin_records_per_vpc(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *id = _ovn_vpc(f, "ovn-a", "ACTIVE");
    _nb_down(f);
    GHashTable *failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    JsonArray *vpcs = pcv_vpc_store_list_vpcs(f->store, NULL, NULL);
    GError *error = NULL;
    g_assert_true(pcv_vpc_fence_reconcile_ovn_begin(f->store, vpcs, "100.64.0.0/16", failed, &error));
    g_assert_no_error(error);
    g_assert_true(g_hash_table_contains(failed, id));
    JsonObject *vpc = _get_vpc(f, id);
    g_assert_cmpstr(json_object_get_string_member(vpc, "state"), ==, "DEGRADED");
    g_assert_false(g_file_test(f->ip_log, G_FILE_TEST_EXISTS));
    json_object_unref(vpc);
    json_array_unref(vpcs);
    g_hash_table_unref(failed);
    g_free(id);
}

                                                                          
static void
test_down_scopes(FenceFx *f, gconstpointer unused)
{
    (void)unused;
    gchar *broken = _ovn_vpc(f, "ovn-a", "DEGRADED");
    gchar *healthy = _ovn_vpc(f, "ovn-b", "ACTIVE");
    gchar *linux_vpc = NULL, *subnet = NULL, *bridge = NULL;
    gint64 revision = 0, subnet_revision = 0;
    GError *error = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "linux-a", "t-a", "nat", "linux",
                                           &linux_vpc, &revision, &error));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, linux_vpc, "t-a", "s1", "10.201.1.0/24", 1500,
                                              revision, &subnet, &bridge, &subnet_revision, &error));
    g_assert_no_error(error);

    gchar *edge_broken = pcv_vpc_ovn_edge_iface_name_from_id(broken);
    gchar *edge_healthy = pcv_vpc_ovn_edge_iface_name_from_id(healthy);
    GHashTable *failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(failed, g_strdup(broken), g_strdup("x"));
    pcv_vpc_fence_ovn_failed_edges_down(failed);
    g_assert_true(_log_has(f->ip_log, edge_broken));
    g_assert_false(_log_has(f->ip_log, edge_healthy));
    g_assert_false(_log_has(f->ip_log, bridge));

    pcv_vpc_fence_all_down(f->store);
    g_assert_true(_log_has(f->ip_log, bridge));

    g_hash_table_unref(failed);
    g_free(edge_healthy); g_free(edge_broken); g_free(bridge); g_free(subnet);
    g_free(linux_vpc); g_free(healthy); g_free(broken);
}

void test_vpc_fence_register(void);

void
test_vpc_fence_register(void)
{
#define FENCE_ADD(path, fn) \
    g_test_add("/vpc/fence/" path, FenceFx, NULL, _fx_setup, fn, _fx_teardown)
    FENCE_ADD("absent_passes", test_absent_passes);
    FENCE_ADD("absent_with_active_attachment_fails", test_absent_with_active_attachment_fails);
    FENCE_ADD("reconcile_clear_degrades_only_that_vpc", test_reconcile_clear_degrades_only_that_vpc);
    FENCE_ADD("clear_degrades_non_target", test_clear_degrades_non_target);
    FENCE_ADD("clear_null_target_degrades", test_clear_null_target_degrades);
    FENCE_ADD("clear_target_itself_fails", test_clear_target_itself_fails);
    FENCE_ADD("policy_sync_failed_outcomes", test_policy_sync_failed_outcomes);
    FENCE_ADD("clear_nb_outage_still_fails", test_clear_nb_outage_still_fails);
    FENCE_ADD("release_only_logs_missing_port_group", test_release_only_logs_missing_port_group);
    FENCE_ADD("owned_applies_acl", test_owned_applies_acl);
    FENCE_ADD("foreign_conflict", test_foreign_conflict);
    FENCE_ADD("nb_outage_is_failure", test_nb_outage_is_failure);
    FENCE_ADD("quarantine_failure_rolls_back", test_quarantine_failure_rolls_back);
    FENCE_ADD("clear_continues_past_failure", test_clear_continues_past_failure);
    FENCE_ADD("release_restores_except_keep", test_release_restores_except_keep);
    FENCE_ADD("unquarantine_tries_both_directions", test_unquarantine_tries_both_directions);
    FENCE_ADD("quarantine_failure_releases_failed_vpc", test_quarantine_failure_releases_failed_vpc);
    FENCE_ADD("quarantine_failure_keeps_degraded", test_quarantine_failure_keeps_degraded);
    FENCE_ADD("quarantine_nb_outage_leaves_nft", test_quarantine_nb_outage_leaves_nft);
    FENCE_ADD("quarantine_nft_failure_releases_acl", test_quarantine_nft_failure_releases_acl);
    FENCE_ADD("quarantine_skips_self", test_quarantine_skips_self);
    FENCE_ADD("reconcile_begin_records_per_vpc", test_reconcile_begin_records_per_vpc);
    FENCE_ADD("down_scopes", test_down_scopes);
#undef FENCE_ADD
}
