
#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "modules/network/network_manager.h"
#include "../../utils/pcv_validate.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/network/network_firewall.h"
#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_worker_pool.h"
#include "vm_iface.h"
#include <json-glib/json-glib.h>

static guint g_qos_reconcile_timer_id = 0;
static gint  g_qos_reconcile_inflight = 0;

#define QOS_PERSIST_PATH "/var/lib/purecvisor/qos_rules.json"

static void
_qos_ensure_dir(void)
{
    gchar *dir = g_path_get_dirname(QOS_PERSIST_PATH);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        PCV_LOG_WARN("QOS", "Cannot create dir %s: %s", dir, g_strerror(errno));
    }
    g_free(dir);
}

static void
_qos_persist_save(const gchar *iface, const gchar *direction,
                  gint rate_mbps, gint burst_kb)
{
    _qos_ensure_dir();

    JsonParser *parser = json_parser_new();
    JsonObject *root = NULL;
    if (g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        if (json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
            root = json_node_dup_object(json_parser_get_root(parser));
        }
    }
    if (!root) root = json_object_new();

    gchar *key = g_strdup_printf("%s:%s", iface, direction);
    JsonObject *rule = json_object_new();
    json_object_set_string_member(rule, "interface", iface);
    json_object_set_string_member(rule, "direction", direction);
    json_object_set_int_member(rule, "rate_mbps", rate_mbps);
    json_object_set_int_member(rule, "burst_kb", burst_kb);
    json_object_set_object_member(root, key, rule);
    g_free(key);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

static void
_qos_persist_remove(const gchar *iface, const gchar *direction)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }
    JsonNode *root_n = json_parser_get_root(parser);
    if (!root_n || json_node_get_node_type(root_n) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
        return;
    }
    JsonObject *root = json_node_dup_object(root_n);

    if (g_strcmp0(direction, "both") == 0) {
        gchar *key_eg = g_strdup_printf("%s:egress", iface);
        gchar *key_in = g_strdup_printf("%s:ingress", iface);
        json_object_remove_member(root, key_eg);
        json_object_remove_member(root, key_in);
        g_free(key_eg); g_free(key_in);
    } else {
        gchar *key = g_strdup_printf("%s:%s", iface, direction);
        json_object_remove_member(root, key);
        g_free(key);
    }

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

void
pcv_qos_restore(void)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }

    JsonNode *qos_root_n = json_parser_get_root(parser);
    if (!qos_root_n || json_node_get_node_type(qos_root_n) != JSON_NODE_OBJECT) {
        g_warning("[QOS] %s is not a valid JSON object — skipping restore",
                  QOS_PERSIST_PATH);
        g_object_unref(parser);
        return;
    }
    JsonObject *root = json_node_get_object(qos_root_n);
    GList *members = json_object_get_members(root);
    gint restored = 0;

    for (GList *l = members; l; l = l->next) {
        const gchar *member_key = l->data;
        JsonObject *rule = json_object_get_object_member(root, member_key);
        const gchar *iface = json_object_get_string_member(rule, "interface");
        const gchar *dir   = json_object_get_string_member(rule, "direction");
        gint rate  = (gint)json_object_get_int_member(rule, "rate_mbps");
        gint burst = (gint)json_object_get_int_member(rule, "burst_kb");

        if (!iface || rate <= 0) continue;

        gchar *sys_path = g_strdup_printf("/sys/class/net/%s", iface);
        gboolean iface_present = g_file_test(sys_path, G_FILE_TEST_EXISTS);
        g_free(sys_path);
        if (!iface_present) {
            PCV_LOG_INFO("QOS", "QoS restore skip %s (%s): iface 미존재 — 다음 reconcile 재시도",
                         iface, dir ? dir : "egress");
            continue;
        }

        PCV_LOG_INFO("QOS", "Restoring QoS for %s (%s): %d Mbps, %d KB burst",
                     iface, dir ? dir : "egress", rate, burst);

        if (!dir || g_strcmp0(dir, "egress") == 0) {
            const gchar *qdisc_argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            pcv_spawn_sync(qdisc_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dMbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *class_argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate_str, "burst", burst_str, NULL};
            pcv_spawn_sync(class_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        } else {

            const gchar *ing_del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_del_argv, NULL, NULL, NULL);

            const gchar *ing_argv[] = {"tc", "qdisc", "add", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dmbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *filter_argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all", "u32",
                "match", "u32", "0", "0",
                "police", "rate", rate_str, "burst", burst_str,
                "action", "drop", NULL};
            pcv_spawn_sync(filter_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        }
        restored++;
    }

    g_list_free(members);
    g_object_unref(parser);

    if (restored > 0)
        PCV_LOG_INFO("QOS", "Restored %d QoS rule(s) from %s",
                     restored, QOS_PERSIST_PATH);
}

/* PCV_SAFETY_CONTROL: qos-rehydrate — 부팅 후 늦게 생성된 vnet에도 persisted QoS를
 * 주기 reconcile로 최종 적용(부팅1회성 무동작 제거); 존재게이트로 무동작 카운터 교정 (NET-4) */
void
pcv_qos_reconcile(void)
{

    pcv_qos_restore();
}

static void
_qos_reconcile_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_qos_reconcile();
    g_atomic_int_set(&g_qos_reconcile_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

static gboolean
_qos_reconcile_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_qos_reconcile_inflight, 0, 1))
        return G_SOURCE_CONTINUE;
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _qos_reconcile_worker);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}

void
pcv_qos_reconcile_timer_init(void)
{
    gint interval = pcv_config_get_int("qos", "reconcile_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO("QOS", "QoS reconcile 타이머 비활성 (reconcile_interval_sec=%d)", interval);
        return;
    }
    g_qos_reconcile_timer_id = g_timeout_add_seconds((guint)interval, _qos_reconcile_tick, NULL);
    PCV_LOG_INFO("QOS", "QoS reconcile 타이머 등록 (%d초 주기)", interval);
}

void
pcv_qos_reconcile_timer_shutdown(void)
{
    if (g_qos_reconcile_timer_id) {
        g_source_remove(g_qos_reconcile_timer_id);
        g_qos_reconcile_timer_id = 0;
    }
}

typedef struct {
    gchar    *bridge_name;
    gchar    *cidr;
    gchar    *mode;
    gchar    *physical_if;
    gchar    *rpc_id;
    gchar    *dhcp_warning;
    gboolean  dns_enabled;
    gchar    *upstream_dns;
    gchar    *ipv6_prefix;
    gint      mtu;
    UdsServer *server;
    GSocketConnection *connection;
} NetworkCtx;

static GHashTable *g_net_inflight = NULL;
static GMutex      g_net_inflight_mu;

static void _net_inflight_init_once(void) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        g_net_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

static void free_network_ctx(gpointer data) {
    if (!data) return;
    NetworkCtx *ctx = (NetworkCtx *)data;
    g_free(ctx->bridge_name);
    g_free(ctx->cidr);
    g_free(ctx->mode);
    g_free(ctx->physical_if);
    g_free(ctx->rpc_id);
    g_free(ctx->dhcp_warning);
    g_free(ctx->upstream_dns);
    g_free(ctx->ipv6_prefix);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error) {

    {
        const gchar *argv[] = {"ip","link","add","name",bridge_name,"type","bridge","stp_state","0",NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Bridge creation failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }

    if (cidr && strlen(cidr) > 0) {
        const gchar *addr_argv[] = {"ip","addr","add",cidr,"dev",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(addr_argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"IP assignment failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local);

            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(std_err_local);
    }

    {
        gint eff_mtu = (mtu > 0) ? mtu : 1500;
        gchar mtu_str[16];
        g_snprintf(mtu_str, sizeof(mtu_str), "%d", eff_mtu);
        const gchar *mtu_argv[] = {"ip","link","set","dev",bridge_name,"mtu",mtu_str,NULL};
        gchar *mtu_err = NULL;
        if (!pcv_spawn_sync(mtu_argv, NULL, &mtu_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "MTU set failed: %s", mtu_err ? mtu_err : "unknown");
            g_free(mtu_err);

            if (cidr && *cidr) {
                const gchar *unaddr[] = {"ip","addr","del",cidr,"dev",bridge_name,NULL};
                pcv_spawn_fire(unaddr);
            }
            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(mtu_err);
    }

    {
        const gchar *up_argv[] = {"ip","link","set","dev",bridge_name,"up",NULL};
        gchar *up_err = NULL;
        if (!pcv_spawn_sync(up_argv, NULL, &up_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Bridge UP failed: %s", up_err ? up_err : "unknown");
            g_free(up_err);

            if (cidr && *cidr) {
                const gchar *unaddr[] = {"ip","addr","del",cidr,"dev",bridge_name,NULL};
                pcv_spawn_fire(unaddr);
            }
            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(up_err);
    }

    return TRUE;
}

gboolean network_bridge_bind_physical(const gchar *bridge_name, const gchar *physical_if, GError **error) {
    {
        const gchar *argv[] = {"ip","link","set",physical_if,"master",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,
                            "Failed to bind physical NIC '%s': %s",
                            physical_if, std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }
    { const gchar *a[] = {"ip","link","set","dev",physical_if,"up",NULL}; pcv_spawn_fire(a); }

    return TRUE;
}

gboolean network_bridge_delete(const gchar *bridge_name, GError **error) {
    gchar *pid_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",    bridge_name);
    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf",   bridge_name);
    gchar *lease_path= g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta",   bridge_name);
    { const gchar *a[] = {"pkill","-F",pid_path,NULL}; pcv_spawn_fire(a); }
    remove(conf_path); remove(pid_path); remove(lease_path); remove(meta_path);
    g_free(pid_path); g_free(conf_path); g_free(lease_path); g_free(meta_path);

    {
        gchar *brif_path = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
        GDir  *dir = g_dir_open(brif_path, 0, NULL);
        if (dir) {
            const gchar *slave;
            while ((slave = g_dir_read_name(dir)) != NULL) {
                const gchar *nm[] = {"ip","link","set",slave,"nomaster",NULL};
                pcv_spawn_fire(nm);
            }
            g_dir_close(dir);
        }
        g_free(brif_path);
    }

    const gchar *del_argv[] = {"ip","link","delete",bridge_name,"type","bridge",NULL};
    gchar *std_err = NULL;
    pcv_spawn_sync(del_argv, NULL, &std_err, error);

    if (error && *error) {
        const gchar *msg = (*error)->message ? (*error)->message : "";
        if (strstr(msg, "Cannot find device") || strstr(msg, "does not exist")) {

            g_error_free(*error);
            *error = NULL;
        } else {
            g_free(std_err);
            return FALSE;
        }
    }

    g_free(std_err);
    return TRUE;
}

static void _network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = g_strdup_printf(
        "{\"mode\":\"%s\",\"cidr\":\"%s\"}",
        mode  ? mode  : "nat",
        cidr  ? cidr  : "");
    if (g_file_set_contents(meta_path, content, -1, NULL)) {

        if (chmod(meta_path, 0600) != 0) {
            PCV_LOG_WARN("network", "chmod 0600 failed on %s: %s",
                         meta_path, g_strerror(errno));
        }
    }
    g_free(meta_path);
    g_free(content);
}

void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    _network_meta_save(bridge_name, mode, cidr);
}

static gchar *_network_meta_get_mode(const gchar *bridge_name) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = NULL;
    gchar *mode_out  = NULL;

    if (g_file_get_contents(meta_path, &content, NULL, NULL) && content) {

        const gchar *hit = strstr(content, "\"mode\":\"");
        if (hit) {
            hit += 8;
            const gchar *end = strchr(hit, '"');
            if (end) {
                mode_out = g_strndup(hit, end - hit);
            }
        }
        g_free(content);
    }
    g_free(meta_path);

    if (!mode_out) {

        if (g_str_has_prefix(bridge_name, "lxcbr"))
            mode_out = g_strdup("nat");

        else if (g_str_has_prefix(bridge_name, "virbr"))
            mode_out = g_strdup("nat");

        else if (g_strcmp0(bridge_name, "docker0") == 0)
            mode_out = g_strdup("nat");

        else if (g_str_has_prefix(bridge_name, "br-"))
            mode_out = g_strdup("bridge");

        else if (g_str_has_prefix(bridge_name, "pcvbr"))
            mode_out = g_strdup("bridge");

        else if (g_str_has_prefix(bridge_name, "pcvoverlay"))
            mode_out = g_strdup("bridge");
        else {

            gchar *lxc_conf = NULL;
            if (g_file_get_contents("/etc/lxc/default.conf", &lxc_conf, NULL, NULL)) {
                gchar *needle = g_strdup_printf("lxc.net.0.link = %s", bridge_name);
                if (strstr(lxc_conf, needle))
                    mode_out = g_strdup("nat");
                g_free(needle);
                g_free(lxc_conf);
            }

            if (!mode_out) {
                gchar *brif = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
                GDir *d = g_dir_open(brif, 0, NULL);
                if (d) {
                    const gchar *ifn;
                    while ((ifn = g_dir_read_name(d))) {

                        if (!g_str_has_prefix(ifn, "vnet") &&
                            !g_str_has_prefix(ifn, "tap") &&
                            !g_str_has_prefix(ifn, "veth")) {
                            mode_out = g_strdup("bridge");
                            break;
                        }
                    }
                    g_dir_close(d);
                }
                g_free(brif);
            }
            if (!mode_out)
                mode_out = g_strdup("unknown");
        }
    }

    return mode_out;
}

static void network_create_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;

    const gchar *cidr = (g_strcmp0(ctx->mode, "bridge") == 0) ? NULL : ctx->cidr;

    if (!network_bridge_create(ctx->bridge_name, cidr, ctx->mtu, &error)) {
        g_task_return_error(task, error);
        return;
    }

    if (g_strcmp0(ctx->mode, "nat") == 0 || ctx->mode == NULL) {

        if (!network_firewall_setup_nat(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {

            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "isolated") == 0) {

        if (!network_firewall_setup_isolated(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {
            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "routed") == 0) {

        if (!network_firewall_setup_routed(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
    }
    else if (g_strcmp0(ctx->mode, "bridge") == 0) {
        if (!ctx->physical_if) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Missing 'physical_if' for bridge mode");
            g_task_return_error(task, error);
            return;
        }
        if (!network_bridge_bind_physical(ctx->bridge_name, ctx->physical_if, &error)) {
            g_task_return_error(task, error);
            return;
        }
    }

    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *warn = g_strdup("IPv6 setup skipped: invalid ipv6_prefix");
        if (ctx->dhcp_warning) {
            gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
            g_free(ctx->dhcp_warning);
            ctx->dhcp_warning = merged;
            g_free(warn);
        } else {
            ctx->dhcp_warning = warn;
        }
    }
    else if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]) {

        {
            const gchar *slash = g_strrstr(ctx->ipv6_prefix, "/");
            gchar *v6_gw_cidr = NULL;
            gchar *prefix_base = slash
                ? g_strndup(ctx->ipv6_prefix, (gsize)(slash - ctx->ipv6_prefix))
                : g_strdup(ctx->ipv6_prefix);
            if (g_str_has_suffix(prefix_base, "::"))
                v6_gw_cidr = g_strdup_printf("%s1%s", prefix_base, slash ? slash : "/64");
            else if (g_str_has_suffix(prefix_base, ":"))
                v6_gw_cidr = g_strdup_printf("%s:1%s", prefix_base, slash ? slash : "/64");
            else
                v6_gw_cidr = g_strdup_printf("%s::1%s", prefix_base, slash ? slash : "/64");
            const gchar *v6_argv[] = {"ip", "-6", "addr", "add", v6_gw_cidr,
                "dev", ctx->bridge_name, NULL};
            pcv_spawn_fire(v6_argv);
            g_free(prefix_base);
            g_free(v6_gw_cidr);
        }
        GError *v6_err = NULL;
        if (!network_dhcp_start_v6(ctx->bridge_name, ctx->ipv6_prefix, &v6_err)) {
            gchar *warn = g_strdup_printf("IPv6 DHCP soft-fail: %s",
                v6_err ? v6_err->message : "unknown");
            if (ctx->dhcp_warning) {
                gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
                g_free(ctx->dhcp_warning);
                ctx->dhcp_warning = merged;
                g_free(warn);
            } else {
                ctx->dhcp_warning = warn;
            }
            if (v6_err) g_error_free(v6_err);
        }
    }

    _network_meta_save(ctx->bridge_name,
                       ctx->mode ? ctx->mode : "nat",
                       ctx->cidr);
    g_task_return_boolean(task, TRUE);
}

static void network_delete_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;

    network_firewall_teardown(ctx->bridge_name, NULL);

    if (!network_bridge_delete(ctx->bridge_name, &error)) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_boolean(task, TRUE);
}

static void network_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    NetworkCtx *ctx = (NetworkCtx *)user_data;
    GError *error = NULL;

    if (!g_task_propagate_boolean(task, &error)) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *result_obj = json_object_new();
        json_object_set_string_member(result_obj, "bridge", ctx->bridge_name);

        gboolean is_delete = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "is_delete"));

        if (!is_delete && g_net_inflight) {
            g_mutex_lock(&g_net_inflight_mu);
            g_hash_table_remove(g_net_inflight, ctx->bridge_name);
            g_mutex_unlock(&g_net_inflight_mu);
        }

        if (is_delete) {
            json_object_set_string_member(result_obj, "status", "deleted");
        } else {
            json_object_set_string_member(result_obj, "status", "created");

            if (ctx->dhcp_warning)
                json_object_set_string_member(result_obj, "dhcp_warning", ctx->dhcp_warning);
        }

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);

        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

void handle_network_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *br_name  = json_object_get_string_member(params, "bridge_name");
    const gchar *mode_raw = json_object_has_member(params, "mode")
                            ? json_object_get_string_member(params, "mode") : NULL;
    const gchar *cidr_raw = json_object_has_member(params, "cidr")
                            ? json_object_get_string_member(params, "cidr") : NULL;
    const gchar *phys_raw = json_object_has_member(params, "physical_if")
                            ? json_object_get_string_member(params, "physical_if") : NULL;

    GError *validate_err = NULL;
    if (!pcv_validate_network_create_params(br_name, mode_raw, cidr_raw, phys_raw, &validate_err)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            validate_err ? validate_err->message : "Invalid parameters");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        if (validate_err) g_error_free(validate_err);
        return;
    }

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name  = g_strdup(br_name);
    if (cidr_raw)  ctx->cidr         = g_strdup(cidr_raw);
    if (mode_raw)  ctx->mode         = g_strdup(mode_raw);
    if (phys_raw)  ctx->physical_if  = g_strdup(phys_raw);

    ctx->mtu = json_object_has_member(params, "mtu")
               ? (gint)json_object_get_int_member(params, "mtu") : 0;
    if (ctx->mtu != 0 && (ctx->mtu < 68 || ctx->mtu > 9216)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mtu — must be between 68 and 9216 (or 0 for default 1500)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        free_network_ctx(ctx);
        return;
    }

    ctx->dns_enabled  = json_object_has_member(params, "dns_enabled")
                        && json_object_get_boolean_member(params, "dns_enabled");
    if (json_object_has_member(params, "upstream_dns"))
        ctx->upstream_dns = g_strdup(json_object_get_string_member(params, "upstream_dns"));

    if (json_object_has_member(params, "ipv6_prefix"))
        ctx->ipv6_prefix = g_strdup(json_object_get_string_member(params, "ipv6_prefix"));

    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid ipv6_prefix — must be <ipv6-literal>/<0..128>, no spaces/newlines");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }

    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    _net_inflight_init_once();
    g_mutex_lock(&g_net_inflight_mu);
    if (g_hash_table_contains(g_net_inflight, br_name)) {
        g_mutex_unlock(&g_net_inflight_mu);
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Network creation already in progress for this bridge");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }
    g_hash_table_insert(g_net_inflight, g_strdup(br_name), GINT_TO_POINTER(TRUE));
    g_mutex_unlock(&g_net_inflight_mu);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

    g_task_set_task_data(task, ctx, free_network_ctx);

    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));

    g_task_run_in_thread(task, network_create_worker);
    g_object_unref(task);
}

void handle_network_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *br = json_object_get_string_member(params, "bridge_name");

    if (!pcv_validate_bridge_name(br)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    {
        gboolean force = json_object_has_member(params, "force")
                       ? json_object_get_boolean_member(params, "force") : FALSE;
        if (!force) {
            gchar *brif_dir = g_strdup_printf("/sys/class/net/%s/brif", br);
            GDir *d = g_dir_open(brif_dir, 0, NULL);
            g_free(brif_dir);
            if (d) {
                const gchar *entry;
                gboolean has_vnet = FALSE;
                while ((entry = g_dir_read_name(d))) {
                    if (g_str_has_prefix(entry, "vnet")) { has_vnet = TRUE; break; }
                }
                g_dir_close(d);
                if (has_vnet) {
                    gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                        "Bridge has active VM interfaces — stop VMs or pass force=true");
                    pure_uds_server_send_response(server, connection, err_resp);
                    g_free(err_resp);
                    return;
                }
            }
        }
    }

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name = g_strdup(br);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

    g_task_set_task_data(task, ctx, free_network_ctx);

    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(TRUE));

    g_task_run_in_thread(task, network_delete_worker);
    g_object_unref(task);
}

#include "../../utils/pcv_log.h"
#include <net/if.h>

#define NET_LOG_DOM "network"

static JsonArray *_read_bridge_slaves(const gchar *bridge_name)
{
    JsonArray *arr  = json_array_new();
    gchar     *path = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
    GDir      *dir  = g_dir_open(path, 0, NULL);

    if (dir) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL)
            json_array_add_string_element(arr, entry);
        g_dir_close(dir);
    }

    g_free(path);
    return arr;
}

static const gchar *_bridge_carrier(const gchar *bridge_name)
{
    gchar *path = g_strdup_printf("/sys/class/net/%s/carrier", bridge_name);
    gchar *content = NULL;
    gboolean ok = g_file_get_contents(path, &content, NULL, NULL);
    g_free(path);

    const gchar *state = "unknown";
    if (ok) {
        g_strstrip(content);
        state = (content[0] == '1') ? "up" : "down";
        g_free(content);
    }
    return state;
}

static gboolean
_pid_file_process_alive(const gchar *pid_path)
{
    gchar *pid_str = NULL;
    gboolean alive = FALSE;

    if (!pid_path || !g_file_get_contents(pid_path, &pid_str, NULL, NULL)) {
        return FALSE;
    }

    g_strstrip(pid_str);
    if (pid_str[0] != '\0') {
        gchar *proc_path = g_strdup_printf("/proc/%s/status", pid_str);
        alive = g_file_test(proc_path, G_FILE_TEST_EXISTS);
        g_free(proc_path);
    }

    g_free(pid_str);
    return alive;
}

static gboolean
_libvirt_dhcp_active_for_bridge(const gchar *bridge_name)
{
    GDir *dir = NULL;
    const gchar *entry = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    dir = g_dir_open("/var/lib/libvirt/dnsmasq", 0, NULL);
    if (!dir) {
        return FALSE;
    }

    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".conf")) {
            continue;
        }

        gchar *conf_path = g_build_filename("/var/lib/libvirt/dnsmasq", entry, NULL);
        gchar *content = NULL;
        if (!g_file_get_contents(conf_path, &content, NULL, NULL) || !content) {
            g_free(conf_path);
            continue;
        }

        gchar *iface_line = g_strdup_printf("interface=%s\n", bridge_name);
        gboolean matches_bridge = strstr(content, iface_line) != NULL;
        gboolean has_dhcp_range = strstr(content, "\ndhcp-range=") != NULL ||
                                  g_str_has_prefix(content, "dhcp-range=");
        g_free(iface_line);
        g_free(content);

        if (matches_bridge && has_dhcp_range) {
            gchar *network_name = g_strndup(entry, strlen(entry) - strlen(".conf"));
            gchar *pid_file = g_strdup_printf("%s.pid", network_name);
            gchar *pid_path = g_build_filename("/var/lib/libvirt/dnsmasq", pid_file, NULL);
            active = _pid_file_process_alive(pid_path);
            g_free(pid_path);
            g_free(pid_file);
            g_free(network_name);
        }

        g_free(conf_path);
        if (active) {
            break;
        }
    }

    g_dir_close(dir);
    return active;
}

static gboolean
_dhcp_socket_active_for_bridge(const gchar *bridge_name)
{
    const gchar *argv[] = {"ss", "-lun", NULL};
    gchar *stdout_buf = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, NULL) || !stdout_buf) {
        g_free(stdout_buf);
        return FALSE;
    }

    gchar *needle = g_strdup_printf("%%%s:67", bridge_name);
    active = strstr(stdout_buf, needle) != NULL;
    g_free(needle);
    g_free(stdout_buf);
    return active;
}

static gboolean
_network_dhcp_active(const gchar *bridge_name)
{
    gchar *pid_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", bridge_name);
    gboolean active = _pid_file_process_alive(pid_path);
    g_free(pid_path);

    if (active) {
        return TRUE;
    }

    if (_libvirt_dhcp_active_for_bridge(bridge_name)) {
        return TRUE;
    }

    return _dhcp_socket_active_for_bridge(bridge_name);
}

static gchar *_get_bridge_ip(const gchar *bridge_name)
{
    const gchar *argv[] = {"ip", "-4", "addr", "show",
                            "dev", bridge_name, NULL};
    gchar *stdout_buf = NULL;
    pcv_spawn_sync(argv, &stdout_buf, NULL, NULL);
    if (!stdout_buf) return g_strdup("");

    const gchar *hit = strstr(stdout_buf, "inet ");
    gchar *result = g_strdup("");
    if (hit) {
        hit += 5;
        const gchar *end = strchr(hit, ' ');
        if (!end) end = strchr(hit, '\n');
        if (end) {
            g_free(result);
            result = g_strndup(hit, end - hit);
        }
    }
    g_free(stdout_buf);
    return result;
}

void handle_network_list_request(JsonObject *params __attribute__((unused)),
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    const gchar *argv[] = {"ip", "-o", "link", "show",
                            "type", "bridge", NULL};
    gchar *stdout_buf = NULL;
    GError *err = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, &err)) {
        const gchar *msg = err ? err->message : "ip link failed";
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonArray *bridges = json_array_new();

    gchar **lines = g_strsplit(stdout_buf ? g_strstrip(stdout_buf) : "", "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!**l) continue;

        const gchar *colon = strchr(*l, ':');
        if (!colon) continue;
        colon++;
        while (*colon == ' ') colon++;
        const gchar *name_end = strchr(colon, ':');
        if (!name_end) continue;
        gchar *br_name = g_strndup(colon, name_end - colon);
        g_strstrip(br_name);

        gchar *at = strchr(br_name, '@');
        if (at) *at = '\0';

        gboolean is_up = (strstr(*l, "UP") != NULL);

        gchar     *ip_cidr = _get_bridge_ip(br_name);
        JsonArray *slaves  = _read_bridge_slaves(br_name);

        gchar *br_mode = _network_meta_get_mode(br_name);

        gboolean dhcp_on = _network_dhcp_active(br_name);

        gchar *phys_uplink = g_strdup("-");
        {
            gchar *brif_path = g_strdup_printf("/sys/class/net/%s/brif", br_name);
            GDir  *brif_dir  = g_dir_open(brif_path, 0, NULL);
            if (brif_dir) {
                const gchar *ifname;
                while ((ifname = g_dir_read_name(brif_dir)) != NULL) {

                    if (!g_str_has_prefix(ifname, "vnet") &&
                        !g_str_has_prefix(ifname, "tap")  &&
                        !g_str_has_prefix(ifname, "veth")) {
                        g_free(phys_uplink);
                        phys_uplink = g_strdup(ifname);
                        break;
                    }
                }
                g_dir_close(brif_dir);
            }
            g_free(brif_path);
        }

        gchar *subnet = g_strdup("-");
        if (ip_cidr && ip_cidr[0] && g_strcmp0(ip_cidr, "") != 0) {
            gchar **cidr_parts = g_strsplit(ip_cidr, "/", 2);
            if (cidr_parts[0] && cidr_parts[1]) {
                gchar **octets = g_strsplit(cidr_parts[0], ".", 4);
                int prefix = atoi(cidr_parts[1]);
                if (octets[0]&&octets[1]&&octets[2]&&octets[3]) {

                    unsigned int addr =
                        ((unsigned)atoi(octets[0]) << 24) |
                        ((unsigned)atoi(octets[1]) << 16) |
                        ((unsigned)atoi(octets[2]) <<  8) |
                         (unsigned)atoi(octets[3]);

                    unsigned int mask = (prefix == 0) ? 0 : (~0u << (32 - prefix));

                    unsigned int net  = addr & mask;
                    g_free(subnet);
                    subnet = g_strdup_printf("%u.%u.%u.%u/%d",
                        (net>>24)&0xFF, (net>>16)&0xFF,
                        (net>>8)&0xFF,  net&0xFF, prefix);
                }
                g_strfreev(octets);
            }
            g_strfreev(cidr_parts);
        }

        JsonObject *obj = json_object_new();
        json_object_set_string_member (obj, "name",       br_name);
        json_object_set_string_member (obj, "state",      is_up ? "up" : "down");
        json_object_set_string_member (obj, "ip_cidr",    ip_cidr);
        json_object_set_string_member (obj, "mode",       br_mode);
        json_object_set_boolean_member(obj, "dhcp",       dhcp_on);
        json_object_set_string_member (obj, "phys",       phys_uplink);
        json_object_set_string_member (obj, "subnet",     subnet);
        g_free(phys_uplink);
        g_free(subnet);
        g_free(br_mode);

        JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(slaves_node, slaves);
        json_object_set_member(obj, "slaves", slaves_node);

        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        json_array_add_element(bridges, node);

        g_free(br_name);
        g_free(ip_cidr);
    }

    g_strfreev(lines);
    g_free(stdout_buf);

    JsonNode *result = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result, bridges);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_network_info_request(JsonObject *params,
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                          "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *br_name = json_object_get_string_member(params,
                               "bridge_name");

    gchar *sys_path = g_strdup_printf("/sys/class/net/%s", br_name);
    gboolean exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
    g_free(sys_path);

    if (!exists) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                          "Bridge not found");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    gchar     *ip_cidr  = _get_bridge_ip(br_name);
    JsonArray *slaves   = _read_bridge_slaves(br_name);
    const gchar *carrier = _bridge_carrier(br_name);

    gboolean has_dhcp = _network_dhcp_active(br_name);

    guint slave_count = json_array_get_length(slaves);

    gchar *info_mode = _network_meta_get_mode(br_name);

    JsonObject *info = json_object_new();
    json_object_set_string_member(info, "name",        br_name);
    json_object_set_string_member(info, "state",       carrier);
    json_object_set_string_member(info, "ip_cidr",     ip_cidr);
    json_object_set_string_member(info, "mode",        info_mode);
    json_object_set_boolean_member(info, "dhcp_active", has_dhcp);
    json_object_set_int_member(info, "slave_count", (gint64)slave_count);
    g_free(info_mode);

    JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(slaves_node, slaves);
    json_object_set_member(info, "slaves", slaves_node);

    JsonNode *result = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(ip_cidr);

    PCV_LOG_INFO(NET_LOG_DOM, "network.info: %s → %s slaves=%u",
                 br_name, carrier, slave_count);
}

void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server,
                                     GSocketConnection *connection) {

    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "mode")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameters: name, mode");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    const gchar *br   = json_object_get_string_member(params, "name");
    const gchar *mode = json_object_get_string_member(params, "mode");
    const gchar *cidr = json_object_has_member(params, "cidr")
                        ? json_object_get_string_member(params, "cidr") : NULL;

    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    if (g_strcmp0(mode, "nat")      != 0 &&
        g_strcmp0(mode, "isolated") != 0 &&
        g_strcmp0(mode, "routed")   != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mode: must be nat | isolated | routed");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    if (!cidr || strlen(cidr) == 0) {

        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: cidr (required for mode change)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    if (!pcv_validate_private_cidr(cidr)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid cidr — must be a private CIDR (RFC1918/RFC6598/fc00::/7)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    GError *error = NULL;

    network_firewall_teardown(br, NULL);

    gboolean ok = FALSE;
    if      (g_strcmp0(mode, "nat")      == 0) ok = network_firewall_setup_nat     (br, cidr, &error);
    else if (g_strcmp0(mode, "isolated") == 0) ok = network_firewall_setup_isolated(br, cidr, &error);
    else if (g_strcmp0(mode, "routed")   == 0) {

        gchar *pid = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);
        const gchar *kill_a[] = {"pkill", "-F", pid, NULL};
        pcv_spawn_fire(kill_a);
        g_free(pid);
        ok = network_firewall_setup_routed(br, cidr, &error);
    }

    if (!ok) {
        const gchar *msg = error ? error->message : "firewall setup failed";
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

    if (ok && (g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0)) {
        gchar    *pid_chk = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);
        gboolean  dhcp_alive = g_file_test(pid_chk, G_FILE_TEST_EXISTS);
        g_free(pid_chk);
        if (!dhcp_alive) {
            GError *dhcp_err = NULL;
            if (!network_dhcp_start(br, cidr, &dhcp_err)) {

                PCV_LOG_INFO(NET_LOG_DOM,
                    "network.mode_set: DHCP restart soft-fail for %s: %s",
                    br, dhcp_err ? dhcp_err->message : "unknown");
                if (dhcp_err) g_error_free(dhcp_err);
            }
        }
    }

    _network_meta_save(br, mode, cidr);

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "bridge", br);
    json_object_set_string_member(res_obj, "mode",   mode);
    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    PCV_LOG_INFO(NET_LOG_DOM, "network.mode_set: %s → %s", br, mode);
}

void handle_network_bind_phys_request(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *br    = json_object_has_member(params, "bridge")
                         ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *iface = json_object_has_member(params, "iface")
                         ? json_object_get_string_member(params, "iface")  : NULL;

    if (!br || !strlen(br) || !iface || !strlen(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing params: 'bridge' and 'iface' are required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    if (!pcv_validate_bridge_name(br) || !pcv_validate_iface_name(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid bridge or iface name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    GError *error = NULL;
    if (!network_bridge_bind_physical(br, iface, &error)) {
        const gchar *msg = error ? error->message : "bind failed";
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "iface",  iface);
    json_object_set_string_member(res, "status", "bound");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    PCV_LOG_INFO(NET_LOG_DOM, "network.bind_phys: %s → %s", iface, br);
}

void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
                      ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || !strlen(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Missing param: 'bridge' is required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                       "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    gboolean enable = TRUE;
    if (json_object_has_member(params, "enable"))
        enable = json_object_get_boolean_member(params, "enable");

    if (enable) {

        gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", br);
        gchar *cidr = NULL;
        GError *meta_err = NULL;
        gchar  *meta_data = NULL;

        if (g_file_get_contents(meta_path, &meta_data, NULL, &meta_err)) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, meta_data, -1, NULL)) {
                JsonObject *mo = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(mo, "cidr"))
                    cidr = g_strdup(json_object_get_string_member(mo, "cidr"));
            }
            g_object_unref(p);
            g_free(meta_data);
        }
        if (meta_err) g_error_free(meta_err);
        g_free(meta_path);

        if (!cidr || !strlen(cidr)) {
            g_free(cidr);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                           "Cannot read CIDR from bridge meta — set mode first");
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            return;
        }

        GError *dhcp_err = NULL;
        if (!network_dhcp_start(br, cidr, &dhcp_err)) {
            const gchar *msg = dhcp_err ? dhcp_err->message : "dnsmasq start failed";
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            if (dhcp_err) g_error_free(dhcp_err);
            g_free(cidr);
            return;
        }
        g_free(cidr);
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP started on %s", br);
    } else {

        gchar *pid_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_fire(kill_argv);
        g_free(pid_path);
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP stopped on %s", br);
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "bridge", br);
    json_object_set_boolean_member(res, "dhcp",  enable);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--may-exist", "add-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge create failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    const gchar *up_argv[] = {"ip", "link", "set", br, "up", NULL};
    pcv_spawn_sync(up_argv, NULL, NULL, NULL);

    g_message("[OVS] Bridge '%s' created.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "type", "ovs");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge delete failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] Bridge '%s' deleted.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br       = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port     = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;
    const gchar *remote   = json_object_has_member(params, "remote_ip")
        ? json_object_get_string_member(params, "remote_ip") : NULL;
    gint64 vni = json_object_has_member(params, "vni")
        ? json_object_get_int_member(params, "vni") : 100;

    if (!br || !port || !remote) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: bridge, port_name, remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_bridge_name(br) || !pcv_validate_ip_literal(remote)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name or remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gchar *vni_str = g_strdup_printf("%ld", (long)vni);
    const gchar *argv[] = {
        "ovs-vsctl", "--may-exist", "add-port", br, port,
        "--", "set", "interface", port,
        "type=vxlan",
        NULL, NULL, NULL, NULL
    };

    gchar *opt_key    = g_strdup_printf("options:key=%s", vni_str);
    gchar *opt_remote = g_strdup_printf("options:remote_ip=%s", remote);
    argv[10] = opt_key;
    argv[11] = opt_remote;
    argv[12] = NULL;

    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN add failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); g_free(vni_str);
        g_free(opt_key); g_free(opt_remote); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' added to '%s' (remote=%s, vni=%ld)",
              port, br, remote, (long)vni);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    json_object_set_string_member(res, "remote_ip", remote);
    json_object_set_int_member(res, "vni", vni);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(vni_str); g_free(opt_key); g_free(opt_remote);
}

void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br   = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;

    if (!br || !port) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: bridge, port_name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-port", br, port, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN del failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' removed from '%s'", port, br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static gchar *
_find_vm_vnet(const gchar *vm_name)
{
    GPtrArray *ifaces = pcv_vm_iface_list(vm_name);
    gchar *vnet = (ifaces->len > 0)
        ? g_strdup(g_ptr_array_index(ifaces, 0)) : NULL;
    g_ptr_array_unref(ifaces);
    return vnet;
}

void handle_network_qos_set(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    gint rate_mbps = json_object_has_member(params, "rate_mbps")
        ? (gint)json_object_get_int_member(params, "rate_mbps") : 0;
    gint burst_kb = json_object_has_member(params, "burst_kb")
        ? (gint)json_object_get_int_member(params, "burst_kb") : 256;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

    gchar *auto_iface = NULL;
    if ((!iface || !iface[0]) && json_object_has_member(params, "vm_name")) {
        const gchar *vm_name = json_object_get_string_member(params, "vm_name");
        if (vm_name && *vm_name) {

            if (!pcv_validate_vm_name(vm_name)) {
                gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                    "Invalid vm_name");
                pure_uds_server_send_response(server, connection, err);
                g_free(err); g_free(auto_iface); return;
            }
            auto_iface = _find_vm_vnet(vm_name);
            if (auto_iface) {
                iface = auto_iface;
                PCV_LOG_INFO("NET", "Resolved vm_name '%s' → interface '%s'",
                             vm_name, iface);
            }
        }
    }

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface (or vm_name)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }

    if (!pcv_validate_iface_name(iface)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid interface name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (rate_mbps <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "rate_mbps must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (burst_kb <= 0) burst_kb = 256;

    if (g_strcmp0(direction, "egress") != 0 && g_strcmp0(direction, "ingress") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "direction must be 'egress' or 'ingress'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }

    if (g_strcmp0(direction, "ingress") == 0) {

        {
            const gchar *del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;

            pcv_spawn_sync(del_argv, NULL, &std_err, &error);
            g_free(std_err);
            if (error) g_error_free(error);
        }
        {
            const gchar *argv[] = {"tc", "qdisc", "add", "dev", iface,
                "handle", "ffff:", "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc ingress qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;
            }
            g_free(std_err);
        }

        {
            gchar *rate = g_strdup_printf("%dmbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all",
                "u32", "match", "u32", "0", "0",
                "police", "rate", rate, "burst", burst, "drop", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc ingress filter failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;
            }
            g_free(std_err);
        }

        g_message("[QoS] Set ingress policing %dMbit burst=%dk on %s",
                  rate_mbps, burst_kb, iface);
    } else {

        {
            const gchar *argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;
            }
            g_free(std_err);
        }

        {
            gchar *rate = g_strdup_printf("%dMbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate, "burst", burst, NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc class failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;
            }
            g_free(std_err);
        }

        g_message("[QoS] Set egress %dMbit burst=%dk on %s", rate_mbps, burst_kb, iface);
    }

    _qos_persist_save(iface, direction, rate_mbps, burst_kb);

    JsonObject *qos_res = json_object_new();
    json_object_set_string_member(qos_res, "status", "ok");
    json_object_set_string_member(qos_res, "interface", iface);
    json_object_set_string_member(qos_res, "direction", direction);
    json_object_set_int_member(qos_res, "rate_mbps", rate_mbps);
    json_object_set_int_member(qos_res, "burst_kb", burst_kb);
    JsonNode *qos_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qos_node, qos_res);
    gchar *qos_resp = pure_rpc_build_success_response(rpc_id, qos_node);
    pure_uds_server_send_response(server, connection, qos_resp);
    g_free(qos_resp);
    g_free(auto_iface);
}

void handle_network_qos_get(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"tc", "-s", "class", "show", "dev", iface, NULL};
    gchar *stdout_buf = NULL;
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, &std_err, &error)) {
        gchar *msg = g_strdup_printf("tc query failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(stdout_buf); g_free(std_err); return;
    }
    g_free(std_err);

    JsonObject *qget_res = json_object_new();
    json_object_set_string_member(qget_res, "interface", iface);
    json_object_set_string_member(qget_res, "tc_output", stdout_buf ? stdout_buf : "");
    json_object_set_boolean_member(qget_res, "egress_active",
        stdout_buf && strstr(stdout_buf, "htb") != NULL);
    g_free(stdout_buf);

    {
        const gchar *ing_argv[] = {"tc", "qdisc", "show", "dev", iface, "ingress", NULL};
        gchar *ing_stdout = NULL;
        gchar *ing_stderr = NULL;
        GError *ing_error = NULL;
        gboolean ing_ok = pcv_spawn_sync(ing_argv, &ing_stdout, &ing_stderr, &ing_error);
        json_object_set_boolean_member(qget_res, "ingress_active",
            ing_ok && ing_stdout && strstr(ing_stdout, "ingress") != NULL);
        g_free(ing_stdout); g_free(ing_stderr);
        if (ing_error) g_error_free(ing_error);
    }

    JsonNode *qget_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qget_node, qget_res);
    gchar *qget_resp = pure_rpc_build_success_response(rpc_id, qget_node);
    pure_uds_server_send_response(server, connection, qget_resp);
    g_free(qget_resp);
}

void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gboolean remove_egress  = (g_strcmp0(direction, "egress") == 0 || g_strcmp0(direction, "all") == 0);
    gboolean remove_ingress = (g_strcmp0(direction, "ingress") == 0 || g_strcmp0(direction, "all") == 0);

    if (remove_egress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "root", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {

            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (strstr(emsg, "No such file") || strstr(emsg, "Cannot delete")) {
                if (error) g_error_free(error);
                g_free(std_err);
            } else {
                gchar *msg = g_strdup_printf("tc qdisc del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
        } else {
            g_free(std_err);
        }
    }

    if (remove_ingress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "ingress", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {

            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (!strstr(emsg, "No such file") && !strstr(emsg, "Cannot delete")
                && !strstr(emsg, "Invalid argument")) {
                gchar *msg = g_strdup_printf("tc ingress del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
            if (error) g_error_free(error);
            g_free(std_err);
        } else {
            g_free(std_err);
        }
    }

    g_message("[QoS] Removed tc qdisc (%s) on %s", direction, iface);

    _qos_persist_remove(iface,
        g_strcmp0(direction, "all") == 0 ? "both" : direction);

    JsonObject *qrm_res = json_object_new();
    json_object_set_string_member(qrm_res, "status", "removed");
    json_object_set_string_member(qrm_res, "interface", iface);
    JsonNode *qrm_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qrm_node, qrm_res);
    gchar *qrm_resp = pure_rpc_build_success_response(rpc_id, qrm_node);
    pure_uds_server_send_response(server, connection, qrm_resp);
    g_free(qrm_resp);
}

gboolean
pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

    gchar *filter_path = g_strdup_printf("/sys/class/net/%s/bridge/vlan_filtering",
                                          bridge);
    GError *werr = NULL;
    if (!g_file_set_contents(filter_path, "1", -1, &werr)) {
        PCV_LOG_WARN("NET", "Failed to enable VLAN filtering on %s: %s",
                     bridge, werr ? werr->message : "unknown");
        if (werr) g_error_free(werr);
        g_free(filter_path);
        return FALSE;
    }
    g_free(filter_path);

    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "add", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Added VLAN %d to %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {
        PCV_LOG_WARN("NET", "Failed to add VLAN %d to %s: %s",
                     vlan_id, iface,
                     error ? error->message : (std_err ? std_err : "unknown"));
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}

gboolean
pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "del", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Removed VLAN %d from %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {

        const gchar *emsg = error ? error->message : (std_err ? std_err : "");
        if (strstr(emsg, "No such") || strstr(emsg, "does not exist")) {
            ok = TRUE;
        } else {
            PCV_LOG_WARN("NET", "Failed to remove VLAN %d from %s: %s",
                         vlan_id, iface, emsg);
        }
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}
