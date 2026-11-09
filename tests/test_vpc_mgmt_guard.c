                                                                             
                                                                                 
                                                                    
                                                                         
                                                                      
                                                               
                                                                     
                                                                         
   
                              
                                                                                   
                                                            
  
                                               
                                                                     
                                                                                
                                                                                             
                                                                                    
                                                                                  
                                                   
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-glib/json-glib.h>

#include "modules/network/vpc/vpc_mgmt_guard.h"
#include "modules/network/vpc/vpc_model.h"
#include "utils/pcv_config.h"

#define FX_TCP "tests/fixtures/vpc_mgmt_guard/proc_net_tcp.txt"
#define FX_TCP6 "tests/fixtures/vpc_mgmt_guard/proc_net_tcp6.txt"

static gchar *read_fixture(const gchar *path) {
    gchar *txt = NULL;
    GError *e = NULL;
    g_assert_true(g_file_get_contents(path, &txt, NULL, &e));
    g_assert_no_error(e);
    return txt;
}

                                                            
static gchar *conf_open(const gchar *body) {
    gchar *dir = g_dir_make_tmp("pcv-vpcmg-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *conf = g_build_filename(dir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(conf, body ? body : "", -1, NULL));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();
    g_free(conf);
    return dir;
}

                                                                   
static void conf_close(gchar *dir) {
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d))) {
            gchar *p = g_build_filename(dir, n, NULL);
            g_remove(p);
            g_free(p);
        }
        g_dir_close(d);
    }
    g_rmdir(dir);
    g_free(dir);
}

static void test_parse_proc_net_tcp_states_and_ports(void) {
    const guint16 ports[] = {22, 80, 443, 8080};
    gchar *txt = read_fixture(FX_TCP);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    g_assert_true(pcv_vpc_mgmt_parse_proc_net_tcp(txt, FALSE, ports, 4, out));
    g_assert_cmpuint(out->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(out, 0), ==, "10.168.3.73/32");
    g_assert_false(pcv_vpc_mgmt_parse_proc_net_tcp("garbage\n", FALSE, ports, 4, out));
    g_ptr_array_free(out, TRUE);
    g_free(txt);
}

static void test_parse_proc_net_tcp6_mapped(void) {
    const guint16 ports[] = {22, 80, 443, 8080};
    gchar *txt = read_fixture(FX_TCP6);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    g_assert_true(pcv_vpc_mgmt_parse_proc_net_tcp(txt, TRUE, ports, 4, out));
    g_assert_cmpuint(out->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(out, 0), ==, "10.20.30.40/32");
    g_ptr_array_free(out, TRUE);
    g_free(txt);
}

static void test_reserved_set_config_and_peers(void) {
    gchar *dir = conf_open("[vpc]\nreserved_cidrs = 10.253.98.0/24, 10.168.9.0/24x\nmgmt_ports = 22\n");
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, FX_TCP, FX_TCP6);

    guint errs = 0;
    GError *err = NULL;
    GPtrArray *set = pcv_vpc_mgmt_reserved_set(&errs, &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_cmpuint(errs, ==, 1);
    g_assert_cmpuint(set->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(set, 0), ==, "10.253.98.0/24");
    g_assert_cmpstr(g_ptr_array_index(set, 1), ==, "10.168.3.73/32");

    const gchar *hit = NULL;
    g_assert_true(pcv_vpc_mgmt_cidr_overlaps_any("10.168.3.0/24", set, &hit));
    g_assert_cmpstr(hit, ==, "10.168.3.73/32");
    g_assert_false(pcv_vpc_mgmt_cidr_overlaps_any("10.168.4.0/24", set, NULL));
    g_ptr_array_free(set, TRUE);

                                                           
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, "/nonexistent/pcv-no-tcp", FX_TCP6);
    err = NULL;
    g_assert_null(pcv_vpc_mgmt_reserved_set(&errs, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, NULL, NULL);
    conf_close(dir);
}

static void test_cidr_within_edges(void) {
    g_assert_true(pcv_vpc_mgmt_cidr_within("10.10.5.0/24", "10.10.0.0/16"));
    g_assert_true(pcv_vpc_mgmt_cidr_within("10.10.0.0/16", "10.10.0.0/16"));
    g_assert_false(pcv_vpc_mgmt_cidr_within("10.11.0.0/24", "10.10.0.0/16"));
    g_assert_true(pcv_vpc_mgmt_cidr_within("10.10.254.0/23", "10.10.0.0/16"));
    g_assert_false(pcv_vpc_mgmt_cidr_within("10.9.255.0/24", "10.10.0.0/16"));
    g_assert_false(pcv_vpc_mgmt_cidr_within("10.10.0.0/15", "10.10.0.0/16"));
    g_assert_false(pcv_vpc_mgmt_cidr_within("bad", "10.10.0.0/16"));
}

                                                                                             
                                                                                   
                                                                               
                                                                     
static void test_pool_covers_fail_closed(void) {
    g_assert_false(pcv_vpc_mgmt_pool_covers(NULL, "10.10.1.0/24"));                         
    GPtrArray *pool = g_ptr_array_new_with_free_func(g_free);
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, "10.10.1.0/24"));                 
    g_ptr_array_add(pool, g_strdup("10.10.0.0/16"));
    g_ptr_array_add(pool, g_strdup("10.20.0.0/24"));
    g_ptr_array_add(pool, g_strdup("10.20.1.0/24"));
    g_assert_true(pcv_vpc_mgmt_pool_covers(pool, "10.10.0.0/16"));                        
    g_assert_true(pcv_vpc_mgmt_pool_covers(pool, "10.10.255.252/30"));                     
    g_assert_true(pcv_vpc_mgmt_pool_covers(pool, "10.20.1.128/25"));                    
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, "10.10.0.0/15"));                         
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, "10.20.0.0/23"));                             
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, "10.30.0.0/24"));                 
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, "bad"));
    g_assert_false(pcv_vpc_mgmt_pool_covers(pool, NULL));
    g_ptr_array_unref(pool);
}

                                                      
static JsonArray *json_array_from(const gchar *text) {
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, text, -1, NULL));
    JsonNode *root = json_parser_get_root(p);
    g_assert_true(JSON_NODE_HOLDS_ARRAY(root));
    JsonArray *a = json_array_ref(json_node_get_array(root));
    g_object_unref(p);
    return a;
}

                                                        
static void assert_outside(GPtrArray *pool, const gchar *subnets, const gchar *want) {
    JsonArray *a = subnets ? json_array_from(subnets) : NULL;
    gchar *got = pcv_vpc_mgmt_pool_outside(pool, a);
    g_assert_cmpstr(got, ==, want);
    g_free(got);
    if (a)
        json_array_unref(a);
}

                                                                                                            
                                                                                     
                                                                         
                                                                            
static void test_pool_outside_lists_and_fail_closed(void) {
    GPtrArray *pool = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(pool, g_strdup("10.10.0.0/16"));
    assert_outside(pool, "[]", NULL);
    assert_outside(pool, "[{\"cidr\":\"10.10.1.0/24\",\"vpc_id\":\"v1\"},{\"cidr\":\"10.10.0.0/16\"}]", NULL);
    assert_outside(pool, "[{\"cidr\":\"10.10.1.0/24\"},{\"cidr\":\"10.11.0.0/24\"},{\"cidr\":\"10.10.0.0/15\"}]",
                   "10.11.0.0/24, 10.10.0.0/15");
    assert_outside(pool, "[\"10.10.1.0/24\",{\"vpc_id\":\"v1\"},{\"cidr\":null},{\"cidr\":5},{\"cidr\":\"bad\"}]",
                   "(unknown cidr), (unknown cidr), (unknown cidr), (unknown cidr), bad");
    assert_outside(pool, NULL, "(subnet list unavailable)");
    assert_outside(NULL, "[{\"cidr\":\"10.10.1.0/24\"}]", "10.10.1.0/24");                             
    g_ptr_array_set_size(pool, 0);
    assert_outside(pool, "[{\"cidr\":\"10.10.1.0/24\"}]", "10.10.1.0/24");
    assert_outside(pool, "[]", NULL);                                                               
    g_ptr_array_unref(pool);
}

static void timeout_case(const gchar *body, guint want) {
    gchar *dir = conf_open(body);
    g_assert_cmpuint(pcv_vpc_mgmt_confirm_timeout_sec(), ==, want);
    conf_close(dir);
}

static void test_confirm_timeout_bounds(void) {
    timeout_case("", 120);
    timeout_case("[vpc]\nconfirm_timeout_sec = 30\n", 30);
    timeout_case("[vpc]\nconfirm_timeout_sec = 4\n", 120);
    timeout_case("[vpc]\nconfirm_timeout_sec = 3601\n", 120);
    timeout_case("[vpc]\nconfirm_timeout_sec = abc\n", 120);
}

static void test_warning_text_fixed(void) {
    gchar *a = pcv_vpc_mgmt_warning_text("10.168.3.73/32", 120, NULL);
    g_assert_cmpstr(a, ==,
        "관리 대역 겹침(10.168.3.73/32) — Job 완료 뒤 120초 안에 vpc.subnet.confirm을 보내지 않으면 자동 원복합니다");
    g_free(a);
    gchar *b = pcv_vpc_mgmt_warning_text("10.253.98.0/24", 5, "<uuid>");
    g_assert_cmpstr(b, ==,
        "관리 대역 겹침(10.253.98.0/24) — 5초 안에 vpc.subnet.confirm(pcvctl vpc subnet-confirm <uuid>)을 보내지 않으면 자동 원복합니다");
    g_free(b);
}

static void test_skip_flag_state(void) {
    gchar *dir = g_dir_make_tmp("pcv-vpcflag-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *flag = g_build_filename(dir, "skip", NULL);
    gchar *target = g_build_filename(dir, "target", NULL);
    gchar *subdir = g_build_filename(dir, "isdir", NULL);
    pcv_vpc_mgmt_set_paths_for_test(NULL, flag, NULL, NULL);

    g_assert_cmpint(pcv_vpc_mgmt_skip_flag_state(), ==, PCV_VPC_MGMT_FLAG_ABSENT);
    g_assert_true(g_file_set_contents(flag, "", 0, NULL));
    g_assert_cmpint(pcv_vpc_mgmt_skip_flag_state(), ==, PCV_VPC_MGMT_FLAG_VALID);

    g_remove(flag);
    g_assert_true(g_file_set_contents(target, "", 0, NULL));
    g_assert_cmpint(symlink(target, flag), ==, 0);
    g_assert_cmpint(pcv_vpc_mgmt_skip_flag_state(), ==, PCV_VPC_MGMT_FLAG_IGNORED);
    g_remove(flag);

    pcv_vpc_mgmt_set_paths_for_test(NULL, subdir, NULL, NULL);
    g_assert_cmpint(g_mkdir(subdir, 0700), ==, 0);
    g_assert_cmpint(pcv_vpc_mgmt_skip_flag_state(), ==, PCV_VPC_MGMT_FLAG_IGNORED);

    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, NULL, NULL);
    g_rmdir(subdir);
    g_remove(target);
    g_rmdir(dir);
    g_free(subdir);
    g_free(target);
    g_free(flag);
    g_free(dir);
}

                                                                      
static void test_reserved_set_without_tcp6(void) {
    gchar *dir = conf_open("[vpc]\nreserved_cidrs = 10.253.98.0/24\nmgmt_ports = 22,443\n");
    guint errs = 0;
    GError *err = NULL;

                                                
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, FX_TCP, FX_TCP6);
    GPtrArray *set = pcv_vpc_mgmt_reserved_set(&errs, &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_cmpuint(set->len, ==, 3);
    g_ptr_array_free(set, TRUE);

    gchar *missing = g_build_filename(dir, "no-tcp6", NULL);
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, FX_TCP, missing);
    set = pcv_vpc_mgmt_reserved_set(&errs, &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_cmpuint(errs, ==, 0);
    g_assert_cmpuint(set->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(set, 0), ==, "10.253.98.0/24");
    g_assert_cmpstr(g_ptr_array_index(set, 1), ==, "10.168.3.73/32");
    g_ptr_array_free(set, TRUE);

                                           
    gchar *subdir = g_build_filename(dir, "tcp6-dir", NULL);
    g_assert_cmpint(g_mkdir(subdir, 0700), ==, 0);
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, FX_TCP, subdir);
    g_assert_null(pcv_vpc_mgmt_reserved_set(&errs, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_rmdir(subdir);

    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, NULL, NULL);
    g_free(subdir);
    g_free(missing);
    conf_close(dir);
}

                                                                     

#define AUTO_REVERT "vpc.subnet.auto_revert"

                                                 
static GString *s_trace;
static gint s_del_code = -1;
static gint s_vdel_code = -1;
static guint s_arm_count;
static guint s_arm_attempts;
static gint64 s_arm_deadline;
static guint s_audit_ok;
static guint s_audit_fail;
static gchar *s_last_target;
static gchar *s_last_actor;
static gint s_last_code;
                                                     
static gboolean s_del_called;
static gchar *s_del_tenant;
static gboolean s_vdel_called;
static gchar *s_vdel_tenant;
                                                              
static const gchar *s_del_fail_only;

static void trace_add(const gchar *a, const gchar *b) {
    if (s_trace->len > 0) g_string_append_c(s_trace, '|');
    g_string_append(s_trace, a);
    if (b) g_string_append(s_trace, b);
}

                                                                            
                            
static guint ledger_file_attempts(const gchar *path, const gchar *id) {
    gchar *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return G_MAXUINT;
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, txt, -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray *arr = json_object_get_array_member(root, "pending");
    guint v = G_MAXUINT;
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (g_strcmp0(json_object_get_string_member(o, "subnet_id"), id) == 0) {
            v = (guint)json_object_get_int_member(o, "attempts");
        }
    }
    g_object_unref(parser);
    g_free(txt);
    return v;
}

                                                                   
                                                     
typedef struct {
    guint file_attempts;                                              
    gboolean confirmed;                             
    gint confirm_code;                                                          
    gchar *confirm_msg;                        
    gboolean second_claim;                                  
    gboolean lookup;                     
    guint count;                         
} InflightObs;
static InflightObs s_obs;
static const gchar *s_obs_path;

static void observe_inflight(const gchar *id) {
    s_obs.file_attempts = ledger_file_attempts(s_obs_path, id);
    GError *e = NULL;
    s_obs.confirmed = pcv_vpc_mgmt_confirm(id, &e);
    s_obs.confirm_code = e && e->domain == PCV_VPC_ERROR ? e->code : -1;
    g_free(s_obs.confirm_msg);
    s_obs.confirm_msg = e ? g_strdup(e->message) : NULL;
    g_clear_error(&e);
    PcvVpcMgmtPending *again = pcv_vpc_mgmt_pending_claim_for_revert(id);
    s_obs.second_claim = again != NULL;
    pcv_vpc_mgmt_pending_free(again);
    s_obs.lookup = pcv_vpc_mgmt_pending_lookup(id, NULL);
    s_obs.count = pcv_vpc_mgmt_pending_count(NULL);
}

static gboolean fake_subnet_delete(const gchar *id, const gchar *tenant, GError **error) {
    if (s_obs_path) observe_inflight(id);
    s_del_called = TRUE;
    g_free(s_del_tenant);
    s_del_tenant = g_strdup(tenant);
    trace_add("del:", id);
    if (s_del_code < 0 || (s_del_fail_only && g_strcmp0(id, s_del_fail_only) != 0)) return TRUE;
    g_set_error(error, PCV_VPC_ERROR, s_del_code, "fake subnet delete");
    return FALSE;
}

static gboolean fake_vpc_delete(const gchar *id, const gchar *tenant, GError **error) {
    s_vdel_called = TRUE;
    g_free(s_vdel_tenant);
    s_vdel_tenant = g_strdup(tenant);
    trace_add("vdel:", id);
    if (s_vdel_code < 0) return TRUE;
    g_set_error(error, PCV_VPC_ERROR, s_vdel_code, "fake vpc delete");
    return FALSE;
}

static gboolean fake_reconcile(GError **error) {
    (void)error;
    trace_add("reconcile", NULL);
    return TRUE;
}

static void fake_audit(const gchar *actor, const gchar *method, const gchar *target,
                       const gchar *result, gint code) {
    s_last_code = code;
    gchar *s = g_strdup_printf("%s:%s", method, result);
    trace_add("audit:", s);
    g_free(s);
    if (g_strcmp0(result, "ok") == 0) s_audit_ok++;
    if (g_strcmp0(result, "fail") == 0) s_audit_fail++;
    g_free(s_last_target);
    s_last_target = g_strdup(target);
    g_free(s_last_actor);
    s_last_actor = g_strdup(actor);
}

static void fake_arm(const PcvVpcMgmtPending *p) {
    trace_add("arm:", p->subnet_id);
    s_arm_count++;
    s_arm_attempts = p->attempts;
    s_arm_deadline = p->deadline_unix;
}

static const PcvVpcMgmtOps FAKE_OPS = {
    .subnet_delete = fake_subnet_delete,
    .vpc_delete = fake_vpc_delete,
    .reconcile = fake_reconcile,
    .audit = fake_audit,
    .arm = fake_arm,
};

static void fake_reset(void) {
    if (!s_trace) s_trace = g_string_new(NULL);
    g_string_truncate(s_trace, 0);
    s_del_code = -1;
    s_vdel_code = -1;
    s_arm_count = 0;
    s_arm_attempts = 0;
    s_arm_deadline = 0;
    s_audit_ok = 0;
    s_audit_fail = 0;
    g_clear_pointer(&s_last_target, g_free);
    g_clear_pointer(&s_last_actor, g_free);
    s_last_code = 0;
    s_del_called = FALSE;
    s_vdel_called = FALSE;
    s_del_fail_only = NULL;
    g_clear_pointer(&s_del_tenant, g_free);
    g_clear_pointer(&s_vdel_tenant, g_free);
    s_obs_path = NULL;
    g_clear_pointer(&s_obs.confirm_msg, g_free);
    s_obs = (InflightObs){.file_attempts = G_MAXUINT, .confirm_code = -1};
}

                                                       
static gchar *guard_open(const gchar *conf_body) {
    gchar *dir = conf_open(conf_body);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    gchar *flag = g_build_filename(dir, "skip", NULL);
    pcv_vpc_mgmt_set_paths_for_test(pend, flag, NULL, NULL);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_pending_load(&e));
    g_assert_no_error(e);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_free(flag);
    g_free(pend);
    fake_reset();
    return dir;
}

                                                         
static void guard_close(gchar *dir) {
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    g_remove(pend);
    g_assert_true(pcv_vpc_mgmt_pending_load(NULL));
    g_free(pend);
    pcv_vpc_mgmt_set_paths_for_test(NULL, NULL, NULL, NULL);
    fake_reset();
    conf_close(dir);
}

static void add_item(const gchar *id, const gchar *vpc, const gchar *tenant, gboolean revert_vpc,
                     gint64 deadline) {
    PcvVpcMgmtPending p = {
        .subnet_id = g_strdup(id),
        .vpc_id = g_strdup(vpc),
        .tenant = g_strdup(tenant),
        .cidr = g_strdup("10.253.98.0/24"),
        .requested_by = g_strdup("admin"),
        .revert_vpc = revert_vpc,
        .deadline_unix = deadline,
        .attempts = 0,
    };
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_pending_add(&p, &e));
    g_assert_no_error(e);
    g_free(p.subnet_id);
    g_free(p.vpc_id);
    g_free(p.tenant);
    g_free(p.cidr);
    g_free(p.requested_by);
}

                                                 
static guint snapshot_attempts(const gchar *id) {
    GPtrArray *snap = pcv_vpc_mgmt_pending_snapshot();
    guint v = G_MAXUINT;
    for (guint i = 0; i < snap->len; i++) {
        const PcvVpcMgmtPending *p = g_ptr_array_index(snap, i);
        if (g_strcmp0(p->subnet_id, id) == 0) v = p->attempts;
    }
    g_ptr_array_free(snap, TRUE);
    return v;
}

                                       
static void warn_collect(const gchar *domain, GLogLevelFlags level, const gchar *msg,
                         gpointer user) {
    (void)domain;
    (void)level;
    g_string_append(user, msg);
    g_string_append_c(user, '\n');
}

                                                                  
static gboolean warns_has_line(const GString *warns, const gchar *want) {
    gchar **lines = g_strsplit(warns->str, "\n", -1);
    gboolean found = FALSE;
    for (gchar **l = lines; *l && !found; l++) found = (g_strcmp0(*l, want) == 0);
    g_strfreev(lines);
    return found;
}

                                                                                   
static gchar *find_corrupt(const gchar *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(d);
    gchar *found = NULL;
    const gchar *n;
    while ((n = g_dir_read_name(d))) {
        if (!found && g_str_has_prefix(n, "vpc_confirm_pending.json.corrupt-")) {
            found = g_build_filename(dir, n, NULL);
        }
    }
    g_dir_close(d);
    return found;
}

static void test_pending_roundtrip_and_mode(void) {
    gchar *dir = guard_open(NULL);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    add_item("A", "V", "ta", TRUE, 1000);
    add_item("B", "W", NULL, FALSE, 2000);                               

    struct stat st;
    g_assert_cmpint(g_stat(pend, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_pending_load(&e));
    g_assert_no_error(e);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 2);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count("ta"), ==, 1);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count("tz"), ==, 0);
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
    g_assert_cmpint(dl, ==, 1000);
    g_assert_true(pcv_vpc_mgmt_pending_lookup("B", &dl));
    g_assert_cmpint(dl, ==, 2000);
    g_assert_false(pcv_vpc_mgmt_pending_lookup("Z", &dl));

                                
    GPtrArray *snap = pcv_vpc_mgmt_pending_snapshot();
    g_assert_cmpuint(snap->len, ==, 2);
    const PcvVpcMgmtPending *a = g_ptr_array_index(snap, 0);
    g_assert_cmpstr(a->subnet_id, ==, "A");
    g_assert_cmpstr(a->vpc_id, ==, "V");
    g_assert_cmpstr(a->tenant, ==, "ta");
    g_assert_cmpstr(a->cidr, ==, "10.253.98.0/24");
    g_assert_cmpstr(a->requested_by, ==, "admin");
    g_assert_true(a->revert_vpc);
    g_assert_cmpuint(a->attempts, ==, 0);
    const PcvVpcMgmtPending *b = g_ptr_array_index(snap, 1);
    g_assert_cmpstr(b->subnet_id, ==, "B");
    g_assert_false(b->revert_vpc);
                                                                           
    g_assert_null(b->tenant);
    g_ptr_array_free(snap, TRUE);

                                                      
    g_assert_true(g_file_set_contents(pend, "{\"version\":1,\"pending\":[{", -1, NULL));
    g_assert_false(pcv_vpc_mgmt_pending_load(&e));
    g_assert_nonnull(e);
    g_clear_error(&e);
    g_assert_false(g_file_test(pend, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    guint corrupt = 0;
    GDir *d = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(d);
    const gchar *n;
    while ((n = g_dir_read_name(d))) {
        if (g_str_has_prefix(n, "vpc_confirm_pending.json.corrupt-")) corrupt++;
    }
    g_dir_close(d);
    g_assert_cmpuint(corrupt, ==, 1);

    g_free(pend);
    guard_close(dir);
}

#define RACE_ROUNDS 1000

typedef struct {
    gint go;
    gboolean confirm_ok;
    gint confirm_code;                                  
    gboolean claim_ok;
} RaceCtx;

                                                                    
                                                         
static void race_wait(RaceCtx *c) {
    while (!g_atomic_int_get(&c->go)) g_thread_yield();
}

static gpointer race_confirm(gpointer data) {
    RaceCtx *c = data;
    race_wait(c);
    GError *e = NULL;
    c->confirm_ok = pcv_vpc_mgmt_confirm("A", &e);
    c->confirm_code = e ? e->code : -1;
    g_clear_error(&e);
    return NULL;
}

                                                                 
                                                                   
                                      
static gpointer race_claim(gpointer data) {
    RaceCtx *c = data;
    race_wait(c);
    PcvVpcMgmtPending *p = pcv_vpc_mgmt_pending_claim_for_revert("A");
    c->claim_ok = p != NULL && pcv_vpc_mgmt_revert(&FAKE_OPS, p, FALSE, 5000);
    return NULL;
}

                                                                  
                                                                    
static void test_confirm_and_expire_claim_once(void) {
    gchar *dir = guard_open(NULL);
    guint confirm_wins = 0;
    guint claim_wins = 0;
    guint inflight_rejects = 0;
    for (guint i = 0; i < RACE_ROUNDS; i++) {
        add_item("A", "V", "ta", FALSE, 1000);
        RaceCtx c = {0};
                                                
        GThread *t1, *t2;
        if (i % 2 == 0) {
            t1 = g_thread_new("race-confirm", race_confirm, &c);
            t2 = g_thread_new("race-claim", race_claim, &c);
        } else {
            t2 = g_thread_new("race-claim", race_claim, &c);
            t1 = g_thread_new("race-confirm", race_confirm, &c);
        }
        g_atomic_int_set(&c.go, 1);
        g_thread_join(t1);
        g_thread_join(t2);
        g_assert_cmpint((gint)c.confirm_ok + (gint)c.claim_ok, ==, 1);
        if (c.confirm_ok) {
            confirm_wins++;
        } else {
            claim_wins++;
                                                          
            g_assert_true(c.confirm_code == PCV_VPC_ERROR_CONFLICT ||
                          c.confirm_code == PCV_VPC_ERROR_NOT_FOUND);
            if (c.confirm_code == PCV_VPC_ERROR_CONFLICT) inflight_rejects++;
        }
        g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
        g_string_truncate(s_trace, 0);                          
    }
    g_assert_cmpuint(confirm_wins + claim_wins, ==, RACE_ROUNDS);
    g_test_message("confirm %u회 / claim %u회 승(진행 중 거절 %u회)", confirm_wins, claim_wins,
                   inflight_rejects);
    GError *e = NULL;
    g_assert_false(pcv_vpc_mgmt_confirm("A", &e));
    g_assert_error(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND);
    g_clear_error(&e);
    guard_close(dir);
}

                                                                         
                                                                     
                                                                                  
                                                                                      
                                                  
                                                              
static void test_revert_keeps_ledger_until_delete_succeeds(void) {
    gchar *dir = guard_open(NULL);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    GError *e = NULL;

                      
    add_item("A", "V", "ta", FALSE, 1000);
    PcvVpcMgmtPending *c = pcv_vpc_mgmt_pending_claim_for_revert("A");
    g_assert_nonnull(c);
    s_obs_path = pend;
    g_assert_true(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, 5000));
    g_assert_cmpuint(s_obs.file_attempts, ==, 0);                             
    g_assert_false(s_obs.confirmed);
    g_assert_cmpint(s_obs.confirm_code, ==, PCV_VPC_ERROR_CONFLICT);
    g_assert_cmpstr(s_obs.confirm_msg, ==, "subnet auto-revert in progress");
    g_assert_false(s_obs.second_claim);
    g_assert_true(s_obs.lookup);
    g_assert_cmpuint(s_obs.count, ==, 1);
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok");
                             
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, G_MAXUINT);
    g_assert_false(pcv_vpc_mgmt_pending_lookup("A", NULL));
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_confirm("A", &e));
    g_assert_error(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND);
    g_clear_error(&e);

                      
    fake_reset();
    add_item("A", "V", "ta", FALSE, 1000);
    c = pcv_vpc_mgmt_pending_claim_for_revert("A");
    g_assert_nonnull(c);
    s_obs_path = pend;
    s_del_code = PCV_VPC_ERROR_IO;
    g_assert_false(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, 5000));
    g_assert_cmpuint(s_obs.file_attempts, ==, 0);
    g_assert_cmpint(s_obs.confirm_code, ==, PCV_VPC_ERROR_CONFLICT);
    g_assert_false(s_obs.second_claim);
                                                           
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, 1);
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
    g_assert_cmpint(dl, ==, 5000 + 30);
    g_assert_cmpuint(s_arm_count, ==, 1);
                                                 
    s_obs_path = NULL;
    g_assert_true(pcv_vpc_mgmt_confirm("A", &e));
    g_assert_no_error(e);
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, G_MAXUINT);

                                  
    fake_reset();
    add_item("B", "W", "tb", FALSE, 1000);
    s_obs_path = pend;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpuint(s_obs.file_attempts, ==, 0);
    g_assert_cmpint(s_obs.confirm_code, ==, PCV_VPC_ERROR_CONFLICT);
    g_assert_false(s_obs.second_claim);
    g_assert_true(s_obs.lookup);
    g_assert_cmpstr(s_trace->str, ==, "del:B|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_cmpuint(ledger_file_attempts(pend, "B"), ==, G_MAXUINT);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());

    g_free(pend);
    guard_close(dir);
}

static void test_revert_not_found_is_success(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    PcvVpcMgmtPending *c = pcv_vpc_mgmt_pending_claim_for_revert("A");
    g_assert_nonnull(c);
    s_del_code = PCV_VPC_ERROR_NOT_FOUND;
    g_assert_true(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, 5000));
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok");
    g_assert_cmpstr(s_last_actor, ==, "system");
    g_assert_cmpstr(s_last_target, ==, "ta/V/A[10.253.98.0/24]");
    g_assert_cmpint(s_last_code, ==, 0);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    guard_close(dir);
}

static void test_revert_retries_then_keeps_entry(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    const gint64 now = 5000;
    for (guint i = 1; i <= 3; i++) {
        PcvVpcMgmtPending *c = pcv_vpc_mgmt_pending_claim_for_revert("A");
        g_assert_nonnull(c);
        s_del_code = PCV_VPC_ERROR_IO;
        g_assert_false(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, now));
        g_assert_cmpint(s_last_code, ==, -32603);                                  
        g_assert_cmpuint(snapshot_attempts("A"), ==, i);
        gint64 dl = 0;
        g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
        g_assert_cmpint(dl, ==, now + 30);
        if (i < 3) {
            g_assert_true(g_str_has_suffix(s_trace->str, "|arm:A"));
            g_assert_cmpuint(s_arm_count, ==, i);
            g_assert_cmpuint(s_arm_attempts, ==, i);
            g_assert_cmpint(s_arm_deadline, ==, now + 30);
        } else {
            g_assert_cmpuint(s_arm_count, ==, 2);
        }
    }
    g_assert_cmpstr(s_trace->str, ==,
                    "del:A|audit:" AUTO_REVERT ":fail|arm:A|"
                    "del:A|audit:" AUTO_REVERT ":fail|arm:A|"
                    "del:A|audit:" AUTO_REVERT ":fail");
    g_assert_cmpuint(s_audit_fail, ==, 3);
    g_assert_cmpuint(s_audit_ok, ==, 0);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);
                                     
    g_assert_true(pcv_vpc_mgmt_pending_load(NULL));
    g_assert_cmpuint(snapshot_attempts("A"), ==, 3);
    guard_close(dir);
}

static void test_revert_vpc_first_subnet(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", TRUE, 1000);
    PcvVpcMgmtPending *c = pcv_vpc_mgmt_pending_claim_for_revert("A");
    g_assert_nonnull(c);
    g_assert_true(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, 5000));
    g_assert_cmpstr(s_trace->str, ==, "del:A|vdel:V|audit:" AUTO_REVERT ":ok");
                                      
    g_assert_cmpstr(s_del_tenant, ==, "ta");
    g_assert_cmpstr(s_vdel_tenant, ==, "ta");
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    guard_close(dir);
}

                                                                            
                                                                            
static void test_revert_null_tenant_passes_null(void) {
    gchar *dir = guard_open(NULL);
    add_item("B", "W", NULL, TRUE, 1000);
    g_assert_true(pcv_vpc_mgmt_pending_load(NULL));                    
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==,
                    "del:B|vdel:W|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_true(s_del_called);
    g_assert_null(s_del_tenant);
    g_assert_true(s_vdel_called);
    g_assert_null(s_vdel_tenant);
    g_assert_cmpstr(s_last_target, ==, "-/W/B[10.253.98.0/24] boot");
    guard_close(dir);
}

static void test_revert_vpc_kept_when_other_subnets(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", TRUE, 1000);
    PcvVpcMgmtPending *c = pcv_vpc_mgmt_pending_claim_for_revert("A");
    g_assert_nonnull(c);
    s_vdel_code = PCV_VPC_ERROR_CONFLICT;
    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    g_assert_true(pcv_vpc_mgmt_revert(&FAKE_OPS, c, FALSE, 5000));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_cmpstr(s_trace->str, ==, "del:A|vdel:V|audit:" AUTO_REVERT ":ok");
                                                  
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, G_MAXUINT);
    g_free(pend);
                                           
    g_assert_cmpstr(warns->str, ==,
                    "자동 원복: VPC V 에 다른 subnet이 있어 subnet A 만 지우고 VPC는 남긴다\n");
    g_string_free(warns, TRUE);
    g_assert_cmpuint(s_audit_ok, ==, 1);
    g_assert_cmpuint(s_audit_fail, ==, 0);
    g_assert_cmpuint(s_arm_count, ==, 0);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    guard_close(dir);
}

#define BOOT_REVERT_TRACE \
    "del:A|audit:" AUTO_REVERT ":ok|del:B|audit:" AUTO_REVERT ":ok|reconcile"

static void test_boot_without_flag_reverts_before_reconcile(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    add_item("B", "W", "tb", FALSE, 1000);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, BOOT_REVERT_TRACE);
    g_assert_cmpstr(s_last_target, ==, "tb/W/B[10.253.98.0/24] boot");
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());
    guard_close(dir);
}

static void test_boot_with_flag_skips_and_rearms(void) {
    gchar *dir = guard_open("[vpc]\nconfirm_timeout_sec = 45\n");
    gchar *flag = g_build_filename(dir, "skip", NULL);
    g_assert_true(g_file_set_contents(flag, "", 0, NULL));
    const gint64 now = 5000;
    add_item("A", "V", "ta", FALSE, now - 100);              

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, now, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);

    g_assert_cmpstr(s_trace->str, ==, "audit:vpc.reconcile.skip:ok|arm:A");
    g_assert_cmpstr(s_last_actor, ==, "system");
    g_assert_cmpint(s_last_code, ==, 0);
                                          
    gchar *want = g_strdup_printf("VPC reconcile skipped by %s — remove it after console recovery\n",
                                  flag);
    g_assert_cmpstr(warns->str, ==, want);
    g_free(want);
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
    g_assert_cmpint(dl, ==, now + 45);
    g_assert_cmpint(s_arm_deadline, ==, now + 45);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());

    g_string_free(warns, TRUE);
    g_remove(flag);
    g_free(flag);
    guard_close(dir);
}

static void test_boot_ignored_flag_behaves_normally(void) {
    gchar *dir = guard_open(NULL);
    gchar *flag = g_build_filename(dir, "skip", NULL);
    gchar *target = g_build_filename(dir, "target", NULL);
    g_assert_true(g_file_set_contents(target, "", 0, NULL));
    g_assert_cmpint(symlink(target, flag), ==, 0);
    add_item("A", "V", "ta", FALSE, 1000);
    add_item("B", "W", "tb", FALSE, 1000);

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);

    g_assert_cmpstr(s_trace->str, ==, BOOT_REVERT_TRACE);
    g_assert_nonnull(strstr(warns->str, flag));
    g_assert_null(strstr(warns->str, "VPC reconcile skipped by"));
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());

    g_string_free(warns, TRUE);
    g_remove(flag);
    g_free(target);
    g_free(flag);
    guard_close(dir);
}

                                                                         
                                                                     
                                                                       
           
static void test_revert_take_survives_unwritable_ledger(void) {
    if (geteuid() == 0) {
        g_test_skip("root는 디렉터리 쓰기 권한을 무시해 기록 실패를 만들 수 없다");
        return;
    }
    gchar *dir = guard_open(NULL);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    g_assert_cmpint(g_chmod(dir, 0500), ==, 0);                                

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_false(pcv_vpc_mgmt_confirm("A", &e));
    g_assert_error(e, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_assert_cmpstr(e->message, ==, "confirm ledger write failed — subnet is still pending auto-revert");
    g_clear_error(&e);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", NULL));

    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_nonnull(strstr(warns->str, "확인 대기 원장 기록 실패 — A 는 원복됐지만 파일에 남는다(다음 기동이 "
                                        "이미 없음으로 정리한다): "));

    g_string_free(warns, TRUE);
    g_assert_cmpint(g_chmod(dir, 0700), ==, 0);
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, 0);                   
                                                                 
    fake_reset();
    s_del_code = PCV_VPC_ERROR_NOT_FOUND;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 6000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_cmpuint(ledger_file_attempts(pend, "A"), ==, G_MAXUINT);
    g_free(pend);
    guard_close(dir);
}

                                                                       
                                                                    
                                                                   
                                                                          
static void test_boot_corrupt_ledger_skips_reconcile(void) {
    gchar *dir = guard_open(NULL);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    g_assert_true(g_file_set_contents(pend, "{\"version\":1,\"pending\":[{", -1, NULL));

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);

                                               
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "audit:vpc.reconcile.skip:fail");
    g_assert_cmpstr(s_last_actor, ==, "system");
    g_assert_cmpstr(s_last_target, ==, pend);
    g_assert_cmpint(s_last_code, ==, -32603);
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_false(g_file_test(pend, G_FILE_TEST_EXISTS));                         
    gchar *corrupt = find_corrupt(dir);
    g_assert_nonnull(corrupt);
                                               
    gchar *want = g_strdup_printf("VPC reconcile skipped: confirm ledger was unreadable — inspect and "
                                  "remove %s, then run vpc.reconcile", corrupt);
    g_assert_true(warns_has_line(warns, want));

                                                                       
                                                              
    add_item("A", "V", "ta", FALSE, 1000);
    fake_reset();
    g_string_truncate(warns, 0);
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
                              
    g_assert_cmpstr(s_trace->str, ==, "audit:vpc.reconcile.skip:fail|arm:A");
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);                        
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
    g_assert_cmpint(dl, ==, 5000 + 120);
    g_assert_cmpint(s_arm_deadline, ==, 5000 + 120);
    gchar *want_nl = g_strconcat(want, "\n", NULL);
    g_assert_cmpstr(warns->str, ==, want_nl);                                
    g_free(want_nl);

                                                              
    g_assert_cmpint(g_remove(corrupt), ==, 0);
    fake_reset();
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);

                                                                                
                                                                             
    fake_reset();
    g_string_truncate(warns, 0);
    g_remove(pend);
    g_assert_cmpint(g_mkdir(pend, 0700), ==, 0);
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "audit:vpc.reconcile.skip:fail");
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());
    gchar *want_dir = g_strdup_printf("VPC reconcile skipped: confirm ledger unreadable — fix access to %s "
                                      "(do not delete it), then restart the daemon or run vpc.reconcile",
                                      pend);
    g_assert_true(warns_has_line(warns, want_dir));
                                                                                
    gchar *corrupt_form = g_strdup_printf("VPC reconcile skipped: confirm ledger was unreadable — inspect "
                                          "and remove %s, then run vpc.reconcile", pend);
    g_assert_cmpstr(want_dir, !=, corrupt_form);
    g_assert_false(warns_has_line(warns, corrupt_form));
    g_assert_null(strstr(want_dir, "remove"));
    g_assert_null(strstr(warns->str, "inspect and remove"));
    g_assert_null(strstr(want, "do not delete"));
    g_free(corrupt_form);
    g_free(want_dir);
    g_rmdir(pend);

    g_free(want);
    g_free(corrupt);
    g_string_free(warns, TRUE);
    g_free(pend);
    guard_close(dir);
}

                                                                 
                                             
static void test_expiry_due_rechecks_deadline(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    gint64 rem = -1;
    g_assert_false(pcv_vpc_mgmt_expiry_due("A", 970, &rem));             
    g_assert_cmpint(rem, ==, 30);
    rem = -1;
    g_assert_false(pcv_vpc_mgmt_expiry_due("A", 999, &rem));
    g_assert_cmpint(rem, ==, 1);
    rem = -1;
    g_assert_true(pcv_vpc_mgmt_expiry_due("A", 1000, &rem));                   
    g_assert_cmpint(rem, ==, 0);
    rem = -1;
    g_assert_true(pcv_vpc_mgmt_expiry_due("A", 5000, &rem));              
    g_assert_cmpint(rem, ==, 0);
                         
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("A", &dl));
    g_assert_cmpint(dl, ==, 1000);
                                                    
    g_assert_true(pcv_vpc_mgmt_confirm("A", NULL));
    rem = -1;
    g_assert_false(pcv_vpc_mgmt_expiry_due("A", 5000, &rem));
    g_assert_cmpint(rem, ==, 0);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_false(pcv_vpc_mgmt_expiry_due("A", 5000, NULL));                              
    g_assert_cmpstr(s_trace->str, ==, "");                         
    guard_close(dir);
}

                                                                    
                                                              
                                                             
static void test_boot_revert_failure_skips_reconcile(void) {
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);
    add_item("B", "W", "tb", FALSE, 1000);
    s_del_code = PCV_VPC_ERROR_IO;
    s_del_fail_only = "B";

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==,
                    "del:A|audit:" AUTO_REVERT ":ok|"
                    "del:B|audit:" AUTO_REVERT ":fail|arm:B|"
                    "audit:vpc.reconcile.skip:fail");
    g_assert_cmpstr(s_last_actor, ==, "system");
    g_assert_cmpstr(s_last_target, ==, "B");
    g_assert_cmpint(s_last_code, ==, -32603);
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_true(warns_has_line(warns, "VPC reconcile skipped: auto-revert failed for 1 pending "
                                        "subnet(s) — fix the cause and run vpc.reconcile"));
                                                    
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);
    g_assert_cmpuint(snapshot_attempts("B"), ==, 1);
    gint64 dl = 0;
    g_assert_true(pcv_vpc_mgmt_pending_lookup("B", &dl));
    g_assert_cmpint(dl, ==, 5000 + 30);

                                                     
    add_item("A", "V", "ta", FALSE, 1000);
    fake_reset();
    g_string_truncate(warns, 0);
    s_del_code = PCV_VPC_ERROR_IO;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 6000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==,
                    "del:A|audit:" AUTO_REVERT ":fail|arm:A|"
                    "del:B|audit:" AUTO_REVERT ":fail|arm:B|"
                    "audit:vpc.reconcile.skip:fail");
    g_assert_cmpstr(s_last_target, ==, "A,B");
    g_assert_true(warns_has_line(warns, "VPC reconcile skipped: auto-revert failed for 2 pending "
                                        "subnet(s) — fix the cause and run vpc.reconcile"));
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 2);

    g_string_free(warns, TRUE);
    guard_close(dir);
}

                                                                     
static gboolean try_add(const gchar *id, GError **error) {
    PcvVpcMgmtPending p = {
        .subnet_id = (gchar *)id, .vpc_id = (gchar *)"V", .tenant = (gchar *)"ta",
        .cidr = (gchar *)"10.253.98.0/24", .requested_by = (gchar *)"admin",
        .revert_vpc = FALSE, .deadline_unix = 9000, .attempts = 0,
    };
    return pcv_vpc_mgmt_pending_add(&p, error);
}

                                                                         
                                                                    
                                                                     
                                                            
static void test_unreadable_boot_blocks_pending_add(void) {
    gchar *dir = guard_open(NULL);
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    gchar *inner = g_build_filename(pend, "keep", NULL);
    g_assert_cmpint(g_mkdir(pend, 0700), ==, 0);
    g_assert_true(g_file_set_contents(inner, "evidence", -1, NULL));

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_true(pcv_vpc_mgmt_boot_reconcile_skipped());

    g_assert_false(try_add("N", &e));
    g_assert_error(e, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    gchar *want = g_strdup_printf("confirm ledger unavailable since boot — fix access to %s and restart "
                                  "the daemon", pend);
    g_assert_cmpstr(e->message, ==, want);
    g_free(want);
    g_clear_error(&e);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 0);
    g_assert_true(g_file_test(pend, G_FILE_TEST_IS_DIR));
    gchar *kept = NULL;
    g_assert_true(g_file_get_contents(inner, &kept, NULL, NULL));
    g_assert_cmpstr(kept, ==, "evidence");
    g_free(kept);

                                                  
    g_assert_cmpint(g_remove(inner), ==, 0);
    g_assert_cmpint(g_rmdir(pend), ==, 0);
    fake_reset();
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);
    g_assert_false(pcv_vpc_mgmt_boot_reconcile_skipped());
    g_assert_true(try_add("N", &e));
    g_assert_no_error(e);
    g_assert_true(pcv_vpc_mgmt_pending_lookup("N", NULL));

    g_string_free(warns, TRUE);
    g_free(inner);
    g_free(pend);
    guard_close(dir);
}

                                                                      
                                                              
                                                                 
                                                                                 
static void test_unreadable_ledger_file_kept_byte_for_byte(void) {
    if (geteuid() == 0) {
        g_test_skip("root는 파일 권한을 무시해 읽기 실패를 만들 수 없다");
        return;
    }
    gchar *dir = guard_open(NULL);
    add_item("A", "V", "ta", FALSE, 1000);                                    
    gchar *pend = g_build_filename(dir, "vpc_confirm_pending.json", NULL);
    gchar *before = NULL;
    gsize before_len = 0;
    g_assert_true(g_file_get_contents(pend, &before, &before_len, NULL));
    g_assert_cmpint(g_chmod(pend, 0000), ==, 0);

    GString *warns = g_string_new(NULL);
    guint h = g_log_set_handler("vpc_mgmt", G_LOG_LEVEL_WARNING, warn_collect, warns);
    GError *e = NULL;
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 5000, &e));
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "audit:vpc.reconcile.skip:fail");                        
    gchar *skip = g_strdup_printf("VPC reconcile skipped: confirm ledger unreadable — fix access to %s "
                                  "(do not delete it), then restart the daemon or run vpc.reconcile", pend);
    g_assert_true(warns_has_line(warns, skip));
    g_free(skip);

    g_assert_false(try_add("N", &e));
    g_assert_error(e, PCV_VPC_ERROR, PCV_VPC_ERROR_IO);
    g_assert_nonnull(strstr(e->message, "confirm ledger unavailable since boot"));
    g_clear_error(&e);
    g_assert_cmpint(g_chmod(pend, 0600), ==, 0);
    gchar *after = NULL;
    gsize after_len = 0;
    g_assert_true(g_file_get_contents(pend, &after, &after_len, NULL));
    g_assert_cmpmem(after, after_len, before, before_len);                          
    g_free(after);

                                                                    
    fake_reset();
    g_assert_true(pcv_vpc_mgmt_boot_with(&FAKE_OPS, 6000, &e));
    g_log_remove_handler("vpc_mgmt", h);
    g_assert_no_error(e);
    g_assert_cmpstr(s_trace->str, ==, "del:A|audit:" AUTO_REVERT ":ok|reconcile");
    g_assert_true(try_add("N", &e));
    g_assert_no_error(e);
    g_assert_cmpuint(pcv_vpc_mgmt_pending_count(NULL), ==, 1);

    g_free(before);
    g_string_free(warns, TRUE);
    g_free(pend);
    guard_close(dir);
}

void test_vpc_mgmt_guard_register(void) {
    g_test_add_func("/vpc_mgmt_guard/parse_proc_net_tcp_states_and_ports",
                    test_parse_proc_net_tcp_states_and_ports);
    g_test_add_func("/vpc_mgmt_guard/parse_proc_net_tcp6_mapped", test_parse_proc_net_tcp6_mapped);
    g_test_add_func("/vpc_mgmt_guard/reserved_set_config_and_peers",
                    test_reserved_set_config_and_peers);
    g_test_add_func("/vpc_mgmt_guard/cidr_within_edges", test_cidr_within_edges);
    g_test_add_func("/vpc_mgmt_guard/pool_covers_fail_closed", test_pool_covers_fail_closed);
    g_test_add_func("/vpc_mgmt_guard/pool_outside_lists_and_fail_closed",
                    test_pool_outside_lists_and_fail_closed);
    g_test_add_func("/vpc_mgmt_guard/confirm_timeout_bounds", test_confirm_timeout_bounds);
    g_test_add_func("/vpc_mgmt_guard/warning_text_fixed", test_warning_text_fixed);
    g_test_add_func("/vpc_mgmt_guard/skip_flag_state", test_skip_flag_state);
    g_test_add_func("/vpc_mgmt_guard/reserved_set_without_tcp6", test_reserved_set_without_tcp6);
    g_test_add_func("/vpc_mgmt_guard/pending_roundtrip_and_mode", test_pending_roundtrip_and_mode);
    g_test_add_func("/vpc_mgmt_guard/confirm_and_expire_claim_once",
                    test_confirm_and_expire_claim_once);
    g_test_add_func("/vpc_mgmt_guard/revert_keeps_ledger_until_delete_succeeds",
                    test_revert_keeps_ledger_until_delete_succeeds);
    g_test_add_func("/vpc_mgmt_guard/revert_not_found_is_success",
                    test_revert_not_found_is_success);
    g_test_add_func("/vpc_mgmt_guard/revert_retries_then_keeps_entry",
                    test_revert_retries_then_keeps_entry);
    g_test_add_func("/vpc_mgmt_guard/revert_vpc_first_subnet", test_revert_vpc_first_subnet);
    g_test_add_func("/vpc_mgmt_guard/revert_null_tenant_passes_null",
                    test_revert_null_tenant_passes_null);
    g_test_add_func("/vpc_mgmt_guard/revert_vpc_kept_when_other_subnets",
                    test_revert_vpc_kept_when_other_subnets);
    g_test_add_func("/vpc_mgmt_guard/boot_without_flag_reverts_before_reconcile",
                    test_boot_without_flag_reverts_before_reconcile);
    g_test_add_func("/vpc_mgmt_guard/boot_with_flag_skips_and_rearms",
                    test_boot_with_flag_skips_and_rearms);
    g_test_add_func("/vpc_mgmt_guard/boot_ignored_flag_behaves_normally",
                    test_boot_ignored_flag_behaves_normally);
    g_test_add_func("/vpc_mgmt_guard/revert_take_survives_unwritable_ledger",
                    test_revert_take_survives_unwritable_ledger);
    g_test_add_func("/vpc_mgmt_guard/boot_corrupt_ledger_skips_reconcile",
                    test_boot_corrupt_ledger_skips_reconcile);
    g_test_add_func("/vpc_mgmt_guard/expiry_due_rechecks_deadline",
                    test_expiry_due_rechecks_deadline);
    g_test_add_func("/vpc_mgmt_guard/boot_revert_failure_skips_reconcile",
                    test_boot_revert_failure_skips_reconcile);
    g_test_add_func("/vpc_mgmt_guard/unreadable_boot_blocks_pending_add",
                    test_unreadable_boot_blocks_pending_add);
    g_test_add_func("/vpc_mgmt_guard/unreadable_ledger_file_kept_byte_for_byte",
                    test_unreadable_ledger_file_kept_byte_for_byte);
}
