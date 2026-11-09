   
                         
                                                            
                                   
  
                           
                                                                                 
                                                                 
                                                                    
                                                                 
                                                                        
                                                               
                                                                
                                                                 
                                                                      
  
                       
                                                              
                                                         
                                                                  
                                                             
   
#include "vpc_mgmt_guard.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "vpc_model.h"

#define MGMT_LOG "vpc_mgmt"

static gchar *s_pending_path;
static gchar *s_flag_path;
static gchar *s_proc_tcp;
static gchar *s_proc_tcp6;

                                                   
                                         
void
pcv_vpc_mgmt_set_paths_for_test(const gchar *pending_path, const gchar *flag_path,
                                const gchar *proc_tcp, const gchar *proc_tcp6)
{
    g_free(s_pending_path);
    g_free(s_flag_path);
    g_free(s_proc_tcp);
    g_free(s_proc_tcp6);
    s_pending_path = g_strdup(pending_path);
    s_flag_path = g_strdup(flag_path);
    s_proc_tcp = g_strdup(proc_tcp);
    s_proc_tcp6 = g_strdup(proc_tcp6);
}

                                                                 
guint
pcv_vpc_mgmt_confirm_timeout_sec(void)
{
    gint v = pcv_config_get_int("vpc", "confirm_timeout_sec", PCV_VPC_MGMT_DEFAULT_TIMEOUT_SEC);
    if (v < 5 || v > 3600) {
        PCV_LOG_WARN(MGMT_LOG, "vpc.confirm_timeout_sec=%d 범위(5-3600) 밖 — 기본 %d초 사용",
                     v, PCV_VPC_MGMT_DEFAULT_TIMEOUT_SEC);
        return PCV_VPC_MGMT_DEFAULT_TIMEOUT_SEC;
    }
    return (guint)v;
}

static void
ports_add(GArray *arr, guint64 p)
{
    guint16 v = (guint16)p;
    for (guint i = 0; i < arr->len; i++) {
        if (g_array_index(arr, guint16, i) == v) return;
    }
    g_array_append_val(arr, v);
}

                                                                     
GArray *
pcv_vpc_mgmt_ports(void)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(guint16));
    gchar **parts = g_strsplit(pcv_config_get_string("vpc", "mgmt_ports", "22,80,443"), ",", -1);
    for (gchar **p = parts; *p; p++) {
        gchar *s = g_strstrip(*p);
        gchar *end = NULL;
        guint64 v = g_ascii_strtoull(s, &end, 10);
        if (*s && end && *end == '\0' && v >= 1 && v <= 65535) {
            ports_add(arr, v);
        } else if (*s) {
            PCV_LOG_WARN(MGMT_LOG, "vpc.mgmt_ports 항목 '%s' 무시", s);
        }
    }
    g_strfreev(parts);
    gint rest = pcv_config_get_rest_port();
    if (rest >= 1 && rest <= 65535) ports_add(arr, (guint64)rest);
    return arr;
}

                                                  
static gboolean
hex_to_u64(const gchar *s, gsize len, guint64 *out)
{
    if (len == 0 || len > 16) return FALSE;
    guint64 v = 0;
    for (gsize i = 0; i < len; i++) {
        gint d = g_ascii_xdigit_value(s[i]);
        if (!g_ascii_isxdigit(s[i])) return FALSE;
        v = (v << 4) | (guint64)d;
    }
    *out = v;
    return TRUE;
}

                                                                           
static gchar *
le_hex_to_ipv4(const gchar *hex8)
{
    guint64 v;
    if (!hex_to_u64(hex8, 8, &v)) return NULL;
    return g_strdup_printf("%u.%u.%u.%u", (guint)(v & 0xff), (guint)((v >> 8) & 0xff),
                           (guint)((v >> 16) & 0xff), (guint)((v >> 24) & 0xff));
}

static gboolean
port_in(const guint16 *ports, guint n, guint64 p)
{
    for (guint i = 0; i < n; i++) {
        if (ports[i] == p) return TRUE;
    }
    return FALSE;
}

                                                                 
                                                     
gboolean
pcv_vpc_mgmt_parse_proc_net_tcp(const gchar *text, gboolean ipv6, const guint16 *ports,
                                guint n_ports, GPtrArray *out_cidrs)
{
    if (!text || !out_cidrs) return FALSE;
    gchar **lines = g_strsplit(text, "\n", -1);
    gboolean header = FALSE;
    for (gchar **l = lines; *l; l++) {
        gchar **f = g_strsplit_set(*l, " \t", -1);
        gchar *tok[4] = {NULL, NULL, NULL, NULL};
        guint n = 0;
        for (gchar **t = f; *t && n < 4; t++) {
            if (**t) tok[n++] = *t;
        }
        if (n == 0) { g_strfreev(f); continue; }
        if (!header) {
            header = (g_strcmp0(tok[0], "sl") == 0 && n >= 3);
            g_strfreev(f);
            if (!header) break;
            continue;
        }
                                                   
        if (n < 4 || !g_str_has_suffix(tok[0], ":") || g_strcmp0(tok[3], "01") != 0) {
            g_strfreev(f);
            continue;
        }
        const gchar *lc = strchr(tok[1], ':');
        const gchar *rc = strchr(tok[2], ':');
        guint64 lport;
        gsize addr_len = ipv6 ? 32 : 8;
        if (!lc || !rc || (gsize)(lc - tok[1]) != addr_len || (gsize)(rc - tok[2]) != addr_len ||
            !hex_to_u64(lc + 1, strlen(lc + 1), &lport) || !port_in(ports, n_ports, lport)) {
            g_strfreev(f);
            continue;
        }
        gchar *ip = NULL;
        if (!ipv6) {
            ip = le_hex_to_ipv4(tok[2]);
        } else if (g_ascii_strncasecmp(tok[2], "0000000000000000FFFF0000", 24) == 0) {
            ip = le_hex_to_ipv4(tok[2] + 24);
        }
        if (ip) {
            gchar *cidr = g_strdup_printf("%s/32", ip);
            gboolean dup = FALSE;
            for (guint i = 0; i < out_cidrs->len; i++) {
                if (g_strcmp0(g_ptr_array_index(out_cidrs, i), cidr) == 0) { dup = TRUE; break; }
            }
            if (!dup && pcv_validate_private_cidr(cidr)) g_ptr_array_add(out_cidrs, cidr);
            else g_free(cidr);
            g_free(ip);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    return header;
}

                                                                              
static gboolean
collect_peers(const gchar *path, gboolean ipv6, gboolean optional, const guint16 *ports,
              guint n_ports, GPtrArray *set, GError **error)
{
    gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(path, &text, NULL, &e)) {
        if (optional && g_error_matches(e, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(e);
            return TRUE;
        }
        g_propagate_error(error, e);
        return FALSE;
    }
    gboolean ok = pcv_vpc_mgmt_parse_proc_net_tcp(text, ipv6, ports, n_ports, set);
    g_free(text);
    if (!ok) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "%s: 머리 행이 없는 /proc 형식", path);
    }
    return ok;
}

                                                                    
GPtrArray *
pcv_vpc_mgmt_reserved_set(guint *config_errors_out, GError **error)
{
    GPtrArray *set = g_ptr_array_new_with_free_func(g_free);
    guint errs = 0;

    gchar **parts = g_strsplit(pcv_config_get_string("vpc", "reserved_cidrs", ""), ",", -1);
    for (gchar **p = parts; *p; p++) {
        gchar *s = g_strstrip(*p);
        if (!*s) continue;
        gchar *canon = NULL;
        PcvVpcIpv4Cidr parsed;
        if (pcv_vpc_cidr_parse(s, &parsed, &canon, NULL) && g_strcmp0(canon, s) == 0) {
            gboolean dup = FALSE;
            for (guint i = 0; i < set->len; i++) {
                if (g_strcmp0(g_ptr_array_index(set, i), canon) == 0) { dup = TRUE; break; }
            }
            if (dup) g_free(canon);
            else g_ptr_array_add(set, canon);
        } else {
            g_free(canon);
            errs++;
            PCV_LOG_WARN(MGMT_LOG, "vpc.reserved_cidrs 항목 '%s' 무시(정규 CIDR 아님)", s);
        }
    }
    g_strfreev(parts);
    if (config_errors_out) *config_errors_out = errs;

    GArray *ports = pcv_vpc_mgmt_ports();
    const guint16 *pv = (const guint16 *)(gpointer)ports->data;
    gboolean ok = collect_peers(s_proc_tcp ? s_proc_tcp : "/proc/net/tcp", FALSE, FALSE, pv,
                                ports->len, set, error) &&
                  collect_peers(s_proc_tcp6 ? s_proc_tcp6 : "/proc/net/tcp6", TRUE, TRUE, pv,
                                ports->len, set, error);
    g_array_free(ports, TRUE);
    if (!ok) {
        g_ptr_array_free(set, TRUE);
        return NULL;
    }
    return set;
}

                                                                  
gboolean
pcv_vpc_mgmt_cidr_overlaps_any(const gchar *cidr, GPtrArray *set, const gchar **hit_out)
{
    PcvVpcIpv4Cidr a;
    if (!cidr || !set || !pcv_vpc_cidr_parse(cidr, &a, NULL, NULL)) return FALSE;
    for (guint i = 0; i < set->len; i++) {
        const gchar *item = g_ptr_array_index(set, i);
        PcvVpcIpv4Cidr b;
        if (pcv_vpc_cidr_parse(item, &b, NULL, NULL) && pcv_vpc_cidr_overlaps(&a, &b)) {
            if (hit_out) *hit_out = item;
            return TRUE;
        }
    }
    return FALSE;
}

                                                                                           
gboolean
pcv_vpc_mgmt_cidr_within(const gchar *inner, const gchar *outer)
{
    PcvVpcIpv4Cidr a, b;
    if (!inner || !outer || !pcv_vpc_cidr_parse(inner, &a, NULL, NULL) ||
        !pcv_vpc_cidr_parse(outer, &b, NULL, NULL)) {
        return FALSE;
    }
    return a.network >= b.network && a.broadcast <= b.broadcast;
}

                                                                           
                                                                 
gboolean
pcv_vpc_mgmt_pool_covers(GPtrArray *pool, const gchar *cidr)
{
    for (guint i = 0; pool && cidr && i < pool->len; i++) {
        if (pcv_vpc_mgmt_cidr_within(cidr, g_ptr_array_index(pool, i)))
            return TRUE;
    }
    return FALSE;
}

                                                                                   
                                                                       
gchar *
pcv_vpc_mgmt_pool_outside(GPtrArray *pool, JsonArray *subnet_cidrs)
{
    if (!subnet_cidrs)
        return g_strdup("(subnet list unavailable)");
    GString *out = NULL;
    for (guint i = 0; i < json_array_get_length(subnet_cidrs); i++) {
        JsonNode *n = json_array_get_element(subnet_cidrs, i);
        JsonObject *o = (n && JSON_NODE_HOLDS_OBJECT(n)) ? json_node_get_object(n) : NULL;
        JsonNode *cn = (o && json_object_has_member(o, "cidr")) ? json_object_get_member(o, "cidr") : NULL;
        const gchar *cidr = (cn && JSON_NODE_HOLDS_VALUE(cn) && json_node_get_value_type(cn) == G_TYPE_STRING)
                                ? json_node_get_string(cn) : NULL;
        if (cidr && pcv_vpc_mgmt_pool_covers(pool, cidr))
            continue;
        if (!out)
            out = g_string_new(NULL);
        else
            g_string_append(out, ", ");
        g_string_append(out, cidr ? cidr : "(unknown cidr)");
    }
    return out ? g_string_free(out, FALSE) : NULL;
}

                                                              
                                                        
PcvVpcMgmtFlag
pcv_vpc_mgmt_skip_flag_state(void)
{
    const gchar *path = s_flag_path ? s_flag_path : PCV_VPC_MGMT_SKIP_FLAG;
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return PCV_VPC_MGMT_FLAG_ABSENT;
        PCV_LOG_WARN(MGMT_LOG, "%s lstat 실패 — 플래그 무시", path);
        return PCV_VPC_MGMT_FLAG_IGNORED;
    }
    uid_t owner_ok = s_flag_path ? geteuid() : 0;
    if (!S_ISREG(st.st_mode) || st.st_uid != owner_ok) {
        PCV_LOG_WARN(MGMT_LOG, "%s 가 일반 파일·유효 소유자가 아니라 무시한다", path);
        return PCV_VPC_MGMT_FLAG_IGNORED;
    }
    return PCV_VPC_MGMT_FLAG_VALID;
}

                                                                    
gchar *
pcv_vpc_mgmt_warning_text(const gchar *hit, guint timeout_sec, const gchar *subnet_id)
{
    if (!subnet_id) {
        return g_strdup_printf("관리 대역 겹침(%s) — Job 완료 뒤 %u초 안에 vpc.subnet.confirm을 "
                               "보내지 않으면 자동 원복합니다", hit, timeout_sec);
    }
    return g_strdup_printf("관리 대역 겹침(%s) — %u초 안에 vpc.subnet.confirm(pcvctl vpc "
                           "subnet-confirm %s)을 보내지 않으면 자동 원복합니다",
                           hit, timeout_sec, subnet_id);
}

                                                                             

#define LEDGER_VERSION 1
#define REVERT_MAX_ATTEMPTS 3
#define REVERT_RETRY_SEC 30
#define AUTO_REVERT_METHOD "vpc.subnet.auto_revert"

static GMutex s_ledger_lock;
static GHashTable *s_ledger;                                                                  
                                                                           
                                                                     
static GHashTable *s_inflight;
static gint s_boot_skipped;               
                                                                       
                                                              
static gint s_ledger_unavailable;

static const gchar *
pending_path(void)
{
    return s_pending_path ? s_pending_path : PCV_VPC_MGMT_PENDING_PATH;
}

                                                  
void
pcv_vpc_mgmt_pending_free(PcvVpcMgmtPending *p)
{
    if (!p) return;
    g_free(p->subnet_id);
    g_free(p->vpc_id);
    g_free(p->tenant);
    g_free(p->cidr);
    g_free(p->requested_by);
    g_free(p);
}

static void
pending_destroy(gpointer p)
{
    pcv_vpc_mgmt_pending_free(p);
}

                                                          
PcvVpcMgmtPending *
pcv_vpc_mgmt_pending_copy(const PcvVpcMgmtPending *p)
{
    if (!p) return NULL;
    PcvVpcMgmtPending *c = g_new0(PcvVpcMgmtPending, 1);
    c->subnet_id = g_strdup(p->subnet_id);
    c->vpc_id = g_strdup(p->vpc_id);
    c->tenant = g_strdup(p->tenant);
    c->cidr = g_strdup(p->cidr);
    c->requested_by = g_strdup(p->requested_by);
    c->revert_vpc = p->revert_vpc;
    c->deadline_unix = p->deadline_unix;
    c->attempts = p->attempts;
    return c;
}

static GHashTable *
ledger_table_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, NULL, pending_destroy);
}

                               
static GHashTable *
ledger_locked(void)
{
    if (!s_ledger) s_ledger = ledger_table_new();
    return s_ledger;
}

                                           
static GHashTable *
inflight_locked(void)
{
    if (!s_inflight) s_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return s_inflight;
}

                                                 
static gint
pending_cmp(gconstpointer a, gconstpointer b)
{
    const PcvVpcMgmtPending *x = *(PcvVpcMgmtPending *const *)a;
    const PcvVpcMgmtPending *y = *(PcvVpcMgmtPending *const *)b;
    if (x->deadline_unix != y->deadline_unix) return x->deadline_unix < y->deadline_unix ? -1 : 1;
    return g_strcmp0(x->subnet_id, y->subnet_id);
}

                                                           
static GPtrArray *
ledger_sorted_locked(void)
{
    GHashTable *t = ledger_locked();
    GPtrArray *arr = g_ptr_array_sized_new(g_hash_table_size(t));
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, NULL, &v)) g_ptr_array_add(arr, v);
    g_ptr_array_sort(arr, pending_cmp);
    return arr;
}

                                                                            
                                                          
static void
json_add_str(JsonBuilder *b, const gchar *name, const gchar *v)
{
    json_builder_set_member_name(b, name);
    if (v) json_builder_add_string_value(b, v);
    else json_builder_add_null_value(b);
}

                                                                   
static gboolean
ledger_persist_locked(GError **error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, LEDGER_VERSION);
    json_builder_set_member_name(b, "pending");
    json_builder_begin_array(b);
    GPtrArray *arr = ledger_sorted_locked();
    for (guint i = 0; i < arr->len; i++) {
        const PcvVpcMgmtPending *p = g_ptr_array_index(arr, i);
        json_builder_begin_object(b);
        json_add_str(b, "subnet_id", p->subnet_id);
        json_add_str(b, "vpc_id", p->vpc_id);
        json_add_str(b, "tenant", p->tenant);
        json_add_str(b, "cidr", p->cidr);
        json_add_str(b, "requested_by", p->requested_by);
        json_builder_set_member_name(b, "revert_vpc");
        json_builder_add_boolean_value(b, p->revert_vpc);
        json_builder_set_member_name(b, "deadline_unix");
        json_builder_add_int_value(b, p->deadline_unix);
        json_builder_set_member_name(b, "attempts");
        json_builder_add_int_value(b, p->attempts);
        json_builder_end_object(b);
    }
    g_ptr_array_free(arr, TRUE);
    json_builder_end_array(b);
    json_builder_end_object(b);

    g_autoptr(JsonNode) root = json_builder_get_root(b);
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, root);
    g_autofree gchar *text = json_generator_to_data(gen, NULL);
    return g_file_set_contents_full(pending_path(), text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                    error);
}

                                        
static const gchar *
json_str_member(JsonObject *o, const gchar *name)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

                                                                 
static gboolean
json_opt_str_member(JsonObject *o, const gchar *name, const gchar **out)
{
    JsonNode *n = json_object_get_member(o, name);
    if (n && JSON_NODE_HOLDS_NULL(n)) {
        *out = NULL;
        return TRUE;
    }
    *out = json_str_member(o, name);
    return *out != NULL;
}

                                                          
                                      
static PcvVpcMgmtPending *
pending_from_json(JsonNode *n)
{
    if (!JSON_NODE_HOLDS_OBJECT(n)) return NULL;
    JsonObject *o = json_node_get_object(n);
    const gchar *sid = json_str_member(o, "subnet_id");
    const gchar *vid = json_str_member(o, "vpc_id");
    const gchar *cidr = json_str_member(o, "cidr");
    const gchar *ten = NULL;
    const gchar *by = NULL;
    JsonNode *rv = json_object_get_member(o, "revert_vpc");
    JsonNode *dl = json_object_get_member(o, "deadline_unix");
    JsonNode *at = json_object_get_member(o, "attempts");
    if (!sid || !*sid || !vid || !cidr || !json_opt_str_member(o, "tenant", &ten) ||
        !json_opt_str_member(o, "requested_by", &by) || !rv || !dl || !at ||
        !JSON_NODE_HOLDS_VALUE(rv) || json_node_get_value_type(rv) != G_TYPE_BOOLEAN ||
        !JSON_NODE_HOLDS_VALUE(dl) || json_node_get_value_type(dl) != G_TYPE_INT64 ||
        !JSON_NODE_HOLDS_VALUE(at) || json_node_get_value_type(at) != G_TYPE_INT64) {
        return NULL;
    }
    gint64 attempts = json_node_get_int(at);
    if (attempts < 0 || attempts > G_MAXUINT) return NULL;
    PcvVpcMgmtPending *p = g_new0(PcvVpcMgmtPending, 1);
    p->subnet_id = g_strdup(sid);
    p->vpc_id = g_strdup(vid);
    p->tenant = g_strdup(ten);
    p->cidr = g_strdup(cidr);
    p->requested_by = g_strdup(by);
    p->revert_vpc = json_node_get_boolean(rv);
    p->deadline_unix = json_node_get_int(dl);
    p->attempts = (guint)attempts;
    return p;
}

                                                                
static GHashTable *
ledger_parse(const gchar *text)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
    JsonObject *o = json_node_get_object(root);
    JsonNode *ver = json_object_get_member(o, "version");
    JsonNode *arr = json_object_get_member(o, "pending");
    if (!ver || !JSON_NODE_HOLDS_VALUE(ver) || json_node_get_value_type(ver) != G_TYPE_INT64 ||
        json_node_get_int(ver) != LEDGER_VERSION || !arr || !JSON_NODE_HOLDS_ARRAY(arr)) {
        return NULL;
    }
    GHashTable *t = ledger_table_new();
    JsonArray *a = json_node_get_array(arr);
    for (guint i = 0; i < json_array_get_length(a); i++) {
        PcvVpcMgmtPending *p = pending_from_json(json_array_get_element(a, i));
        if (!p || g_hash_table_contains(t, p->subnet_id)) {
            pcv_vpc_mgmt_pending_free(p);
            g_hash_table_unref(t);
            return NULL;
        }
        g_hash_table_insert(t, p->subnet_id, p);
    }
    return t;
}

                                                          
                                                                      
                                                          
gboolean
pcv_vpc_mgmt_pending_load(GError **error)
{
    const gchar *path = pending_path();
    gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(path, &text, NULL, &e)) {
        if (g_error_matches(e, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(e);
            g_mutex_lock(&s_ledger_lock);
            g_hash_table_remove_all(ledger_locked());
            g_hash_table_remove_all(inflight_locked());
            g_mutex_unlock(&s_ledger_lock);
            g_atomic_int_set(&s_ledger_unavailable, FALSE);
            return TRUE;
        }
        g_propagate_error(error, e);
        return FALSE;
    }
    GHashTable *fresh = ledger_parse(text);
    g_free(text);
    if (!fresh) {
        gchar *dst = g_strdup_printf("%s.corrupt-%" G_GINT64_FORMAT, path,
                                     g_get_real_time() / G_USEC_PER_SEC);
        if (g_rename(path, dst) == 0) {
            PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 %s 가 깨져 %s 로 옮기고 빈 원장으로 시작한다",
                         path, dst);
        } else {
            PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 %s 가 깨졌고 %s 로 옮기지 못했다: %s", path, dst,
                         g_strerror(errno));
        }
        g_mutex_lock(&s_ledger_lock);
        g_hash_table_remove_all(ledger_locked());
        g_hash_table_remove_all(inflight_locked());
        g_mutex_unlock(&s_ledger_lock);
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "%s: 깨진 확인 대기 원장(%s 로 옮김)",
                    path, dst);
        g_free(dst);
        return FALSE;
    }
    g_mutex_lock(&s_ledger_lock);
    if (s_ledger) g_hash_table_unref(s_ledger);
    s_ledger = fresh;
    g_hash_table_remove_all(inflight_locked());
    g_mutex_unlock(&s_ledger_lock);
    g_atomic_int_set(&s_ledger_unavailable, FALSE);
    return TRUE;
}

                                                         
                                                                
                                                                 
                                         
gboolean
pcv_vpc_mgmt_pending_add(const PcvVpcMgmtPending *p, GError **error)
{
    if (!p || !p->subnet_id || !*p->subnet_id || !p->vpc_id || !p->cidr) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "subnet_id·vpc_id·cidr가 없는 확인 대기 항목");
        return FALSE;
    }
    if (g_atomic_int_get(&s_ledger_unavailable)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "confirm ledger unavailable since boot — fix access to %s and restart the daemon",
                    pending_path());
        return FALSE;
    }
    PcvVpcMgmtPending *copy = pcv_vpc_mgmt_pending_copy(p);
    g_mutex_lock(&s_ledger_lock);
    GHashTable *t = ledger_locked();
    gpointer old_key = NULL;
    gpointer old = NULL;
    gboolean had = g_hash_table_steal_extended(t, copy->subnet_id, &old_key, &old);
    g_hash_table_insert(t, copy->subnet_id, copy);
    gboolean ok = ledger_persist_locked(error);
    if (!ok) {
        g_hash_table_remove(t, p->subnet_id);
        if (had) g_hash_table_insert(t, ((PcvVpcMgmtPending *)old)->subnet_id, old);
    } else if (had) {
        pcv_vpc_mgmt_pending_free(old);
    }
    g_mutex_unlock(&s_ledger_lock);
    return ok;
}

                                                                
                                                                    
                                                               
                        
PcvVpcMgmtPending *
pcv_vpc_mgmt_pending_claim(const gchar *subnet_id, GError **error)
{
    GError *e = NULL;
    gpointer key = NULL;
    gpointer v = NULL;
    g_mutex_lock(&s_ledger_lock);
    GHashTable *t = ledger_locked();
    if (subnet_id && g_hash_table_contains(inflight_locked(), subnet_id)) {
        g_mutex_unlock(&s_ledger_lock);
        g_set_error_literal(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                            "subnet auto-revert in progress");
        return NULL;
    }
    if (!subnet_id || !g_hash_table_steal_extended(t, subnet_id, &key, &v)) {
        g_mutex_unlock(&s_ledger_lock);
        g_set_error_literal(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                            "no pending confirmation for this subnet");
        return NULL;
    }
    gboolean ok = ledger_persist_locked(&e);
    if (!ok) g_hash_table_insert(t, ((PcvVpcMgmtPending *)v)->subnet_id, v);
    g_mutex_unlock(&s_ledger_lock);
    if (!ok) {
        PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 기록 실패 — %s 꺼내기를 취소한다: %s", subnet_id,
                     e ? e->message : "unknown");
        g_clear_error(&e);
        g_set_error_literal(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                            "confirm ledger write failed — subnet is still pending auto-revert");
        return NULL;
    }
    return v;
}

                                                                  
                                                                 
                                                                           
                                                                         
                                     
PcvVpcMgmtPending *
pcv_vpc_mgmt_pending_claim_for_revert(const gchar *subnet_id)
{
    if (!subnet_id) return NULL;
    PcvVpcMgmtPending *copy = NULL;
    g_mutex_lock(&s_ledger_lock);
    const PcvVpcMgmtPending *p = g_hash_table_lookup(ledger_locked(), subnet_id);
    GHashTable *inflight = inflight_locked();
    if (p && !g_hash_table_contains(inflight, subnet_id)) {
        g_hash_table_add(inflight, g_strdup(subnet_id));
        copy = pcv_vpc_mgmt_pending_copy(p);
    }
    g_mutex_unlock(&s_ledger_lock);
    return copy;
}

                                                                    
                                                                          
                                                                    
gboolean
pcv_vpc_mgmt_pending_lookup(const gchar *subnet_id, gint64 *deadline_out)
{
    if (!subnet_id) return FALSE;
    g_mutex_lock(&s_ledger_lock);
    const PcvVpcMgmtPending *p = g_hash_table_lookup(ledger_locked(), subnet_id);
    if (p && deadline_out) *deadline_out = p->deadline_unix;
    g_mutex_unlock(&s_ledger_lock);
    return p != NULL;
}

                                                            
                                                              
                                    
gboolean
pcv_vpc_mgmt_expiry_due(const gchar *subnet_id, gint64 now_unix, gint64 *remaining_out)
{
    gint64 deadline = 0;
    gint64 remaining = 0;
    gboolean due = FALSE;
    if (pcv_vpc_mgmt_pending_lookup(subnet_id, &deadline)) {
        if (now_unix < deadline) remaining = deadline - now_unix;
        else due = TRUE;
    }
    if (remaining_out) *remaining_out = remaining;
    return due;
}

                                                                     
                                          
guint
pcv_vpc_mgmt_pending_count(const gchar *tenant)
{
    guint n = 0;
    g_mutex_lock(&s_ledger_lock);
    GHashTable *t = ledger_locked();
    if (!tenant) {
        n = g_hash_table_size(t);
    } else {
        GHashTableIter it;
        gpointer v;
        g_hash_table_iter_init(&it, t);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            if (g_strcmp0(((const PcvVpcMgmtPending *)v)->tenant, tenant) == 0) n++;
        }
    }
    g_mutex_unlock(&s_ledger_lock);
    return n;
}

                                                          
GPtrArray *
pcv_vpc_mgmt_pending_snapshot(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(pending_destroy);
    g_mutex_lock(&s_ledger_lock);
    GPtrArray *arr = ledger_sorted_locked();
    for (guint i = 0; i < arr->len; i++) {
        g_ptr_array_add(out, pcv_vpc_mgmt_pending_copy(g_ptr_array_index(arr, i)));
    }
    g_ptr_array_free(arr, TRUE);
    g_mutex_unlock(&s_ledger_lock);
    return out;
}

                                                                   
                                                                           
                                   
gboolean
pcv_vpc_mgmt_confirm(const gchar *subnet_id, GError **error)
{
    PcvVpcMgmtPending *p = pcv_vpc_mgmt_pending_claim(subnet_id, error);
    if (!p) return FALSE;
    pcv_vpc_mgmt_pending_free(p);
    return TRUE;
}

                                                                
                                                           
static void
ledger_requeue(const PcvVpcMgmtPending *p)
{
    PcvVpcMgmtPending *copy = pcv_vpc_mgmt_pending_copy(p);
    GError *e = NULL;
    g_mutex_lock(&s_ledger_lock);
    g_hash_table_replace(ledger_locked(), copy->subnet_id, copy);
    g_hash_table_remove(inflight_locked(), p->subnet_id);
    gboolean ok = ledger_persist_locked(&e);
    g_mutex_unlock(&s_ledger_lock);
    if (!ok) {
        PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 기록 실패 — %s 는 메모리에만 남는다: %s",
                     p->subnet_id, e ? e->message : "unknown");
        g_clear_error(&e);
    }
}

                                                              
                                                                            
static void
ledger_finish_revert(const gchar *subnet_id)
{
    GError *e = NULL;
    gboolean ok = TRUE;
    g_mutex_lock(&s_ledger_lock);
    g_hash_table_remove(inflight_locked(), subnet_id);
    if (g_hash_table_remove(ledger_locked(), subnet_id)) ok = ledger_persist_locked(&e);
    g_mutex_unlock(&s_ledger_lock);
    if (!ok) {
        PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 기록 실패 — %s 는 원복됐지만 파일에 남는다(다음 기동이 "
                     "이미 없음으로 정리한다): %s", subnet_id, e ? e->message : "unknown");
        g_clear_error(&e);
    }
}

                                                                 
                                                                      
                                                           
                                       
gboolean
pcv_vpc_mgmt_revert(const PcvVpcMgmtOps *ops, PcvVpcMgmtPending *claimed, gboolean at_boot,
                    gint64 now_unix)
{
                                                                                              
    g_return_val_if_fail(ops != NULL, FALSE);
    if (!claimed) return FALSE;
    gchar *target = g_strdup_printf("%s/%s/%s[%s]%s", claimed->tenant ? claimed->tenant : "-",
                                    claimed->vpc_id,
                                    claimed->subnet_id, claimed->cidr, at_boot ? " boot" : "");
    GError *e = NULL;
    gboolean ok = ops->subnet_delete(claimed->subnet_id, claimed->tenant, &e);
    if (!ok && g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND)) {
        PCV_LOG_INFO(MGMT_LOG, "자동 원복: subnet %s 는 이미 없다", claimed->subnet_id);
        g_clear_error(&e);
        ok = TRUE;
    }
    if (ok && claimed->revert_vpc && !ops->vpc_delete(claimed->vpc_id, claimed->tenant, &e)) {
        if (g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND)) {
            g_clear_error(&e);
        } else if (g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT)) {
            PCV_LOG_WARN(MGMT_LOG, "자동 원복: VPC %s 에 다른 subnet이 있어 subnet %s 만 지우고 "
                         "VPC는 남긴다", claimed->vpc_id, claimed->subnet_id);
            g_clear_error(&e);
        } else {
            ok = FALSE;
        }
    }
    ops->audit(PCV_AUDIT_ACTOR_SYSTEM, AUTO_REVERT_METHOD, target, ok ? "ok" : "fail",
               ok ? 0 : PURE_RPC_ERR_INTERNAL_ERROR);
    if (ok) {
        ledger_finish_revert(claimed->subnet_id);
        PCV_LOG_INFO(MGMT_LOG, "자동 원복 완료: %s", target);
        g_free(target);
        pcv_vpc_mgmt_pending_free(claimed);
        return TRUE;
    }
    claimed->attempts++;
    claimed->deadline_unix = now_unix + REVERT_RETRY_SEC;
    PCV_LOG_WARN(MGMT_LOG, "자동 원복 실패(%u/%d): %s: %s", claimed->attempts,
                 REVERT_MAX_ATTEMPTS, target, e ? e->message : "unknown");
    g_clear_error(&e);
    ledger_requeue(claimed);
    if (claimed->attempts < REVERT_MAX_ATTEMPTS) {
        ops->arm(claimed);
    } else {
        PCV_LOG_WARN(MGMT_LOG, "자동 원복 %d회 실패 — %s 를 타이머 없이 원장에 남긴다(다음 기동이 "
                     "다시 원복한다)", REVERT_MAX_ATTEMPTS, target);
    }
    g_free(target);
    pcv_vpc_mgmt_pending_free(claimed);
    return FALSE;
}

                                                        
static void
ledger_rearm(const PcvVpcMgmtOps *ops, gint64 deadline)
{
    GError *e = NULL;
    g_mutex_lock(&s_ledger_lock);
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, ledger_locked());
    while (g_hash_table_iter_next(&it, NULL, &v)) ((PcvVpcMgmtPending *)v)->deadline_unix = deadline;
    gboolean ok = ledger_persist_locked(&e);
    g_mutex_unlock(&s_ledger_lock);
    if (!ok) {
        PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 기록 실패 — 새 기한은 메모리에만 있다: %s",
                     e ? e->message : "unknown");
        g_clear_error(&e);
    }
    GPtrArray *snap = pcv_vpc_mgmt_pending_snapshot();
    for (guint i = 0; i < snap->len; i++) ops->arm(g_ptr_array_index(snap, i));
    g_ptr_array_free(snap, TRUE);
}

                                                                       
                                                       
static gchar *
ledger_corrupt_leftover(void)
{
    g_autofree gchar *dir = g_path_get_dirname(pending_path());
    g_autofree gchar *base = g_path_get_basename(pending_path());
    g_autofree gchar *prefix = g_strconcat(base, ".corrupt-", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    gchar *first = NULL;
    const gchar *n;
    while ((n = g_dir_read_name(d))) {
        if (g_str_has_prefix(n, prefix) && (!first || g_strcmp0(n, first) < 0)) {
            g_free(first);
            first = g_strdup(n);
        }
    }
    g_dir_close(d);
    gchar *full = first ? g_build_filename(dir, first, NULL) : NULL;
    g_free(first);
    return full;
}

                                                               
                                                                  
                                                              
                                                              
                                                           
                                               
gboolean
pcv_vpc_mgmt_boot_with(const PcvVpcMgmtOps *ops, gint64 now_unix, GError **error)
{
    g_return_val_if_fail(ops != NULL, FALSE);
    GError *le = NULL;
    gboolean loaded = pcv_vpc_mgmt_pending_load(&le);
    g_autofree gchar *corrupt = ledger_corrupt_leftover();
                                                                              
                                                                        
                                                                                
    if (!loaded && (!corrupt || g_file_test(pending_path(), G_FILE_TEST_EXISTS)))
        g_atomic_int_set(&s_ledger_unavailable, TRUE);
    if (!loaded || corrupt) {
                                                                          
                                                            
                                                              
                                                                    
                                                                   
                                                               
        g_atomic_int_set(&s_boot_skipped, TRUE);
        if (!loaded) {
            PCV_LOG_WARN(MGMT_LOG, "확인 대기 원장 적재 실패: %s", le ? le->message : "unknown");
            g_clear_error(&le);
        }
                                                                               
                                                                             
        if (corrupt) {
            PCV_LOG_WARN(MGMT_LOG, "VPC reconcile skipped: confirm ledger was unreadable — inspect and "
                         "remove %s, then run vpc.reconcile", corrupt);
        } else {
            PCV_LOG_WARN(MGMT_LOG, "VPC reconcile skipped: confirm ledger unreadable — fix access to %s "
                         "(do not delete it), then restart the daemon or run vpc.reconcile",
                         pending_path());
        }
        ops->audit(PCV_AUDIT_ACTOR_SYSTEM, "vpc.reconcile.skip", pending_path(), "fail",
                   PURE_RPC_ERR_INTERNAL_ERROR);
        if (loaded) ledger_rearm(ops, now_unix + (gint64)pcv_vpc_mgmt_confirm_timeout_sec());
        return TRUE;
    }
    if (pcv_vpc_mgmt_skip_flag_state() == PCV_VPC_MGMT_FLAG_VALID) {
        const gchar *flag = s_flag_path ? s_flag_path : PCV_VPC_MGMT_SKIP_FLAG;
        g_atomic_int_set(&s_boot_skipped, TRUE);
        PCV_LOG_WARN(MGMT_LOG, "VPC reconcile skipped by %s — remove it after console recovery",
                     flag);
        ops->audit(PCV_AUDIT_ACTOR_SYSTEM, "vpc.reconcile.skip", flag, "ok", 0);
        ledger_rearm(ops, now_unix + (gint64)pcv_vpc_mgmt_confirm_timeout_sec());
        return TRUE;
    }
    g_atomic_int_set(&s_boot_skipped, FALSE);
    GPtrArray *snap = pcv_vpc_mgmt_pending_snapshot();
    GString *failed = g_string_new(NULL);                                    
    guint n_failed = 0;
    for (guint i = 0; i < snap->len; i++) {
        const PcvVpcMgmtPending *p = g_ptr_array_index(snap, i);
                                                                    
                                                                  
        PcvVpcMgmtPending *claimed = pcv_vpc_mgmt_pending_claim_for_revert(p->subnet_id);
        if (claimed && !pcv_vpc_mgmt_revert(ops, claimed, TRUE, now_unix)) {
            if (failed->len > 0) g_string_append_c(failed, ',');
            g_string_append(failed, p->subnet_id);
            n_failed++;
        }
    }
    g_ptr_array_free(snap, TRUE);
    if (n_failed > 0) {
                                                                       
                                                                     
                                                        
        g_atomic_int_set(&s_boot_skipped, TRUE);
        PCV_LOG_WARN(MGMT_LOG, "VPC reconcile skipped: auto-revert failed for %u pending subnet(s) — "
                     "fix the cause and run vpc.reconcile", n_failed);
        ops->audit(PCV_AUDIT_ACTOR_SYSTEM, "vpc.reconcile.skip", failed->str, "fail",
                   PURE_RPC_ERR_INTERNAL_ERROR);
        g_string_free(failed, TRUE);
        return TRUE;
    }
    g_string_free(failed, TRUE);
    return ops->reconcile(error);
}

                                                            
                       
gboolean
pcv_vpc_mgmt_boot_reconcile_skipped(void)
{
    return g_atomic_int_get(&s_boot_skipped) != 0;
}
