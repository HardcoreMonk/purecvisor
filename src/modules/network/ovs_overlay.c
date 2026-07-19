
#include "ovs_overlay.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include "utils/pcv_worker_pool.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <glib/gstdio.h>

#define OVERLAY_LOG_DOM   "ovs_overlay"

#define OVERLAY_META_DIR  "/var/lib/purecvisor/overlay"
#define OVERLAY_MAX       16

typedef struct {
    gchar    *name;
    gchar    *cidr;
    gint      vni;
    GPtrArray *peers;
    gboolean  active;
} OverlayNet;

static struct {
    gchar      *local_ip;
    OverlayNet  nets[OVERLAY_MAX];
    gint        count;
    GMutex      mu;
    gboolean    initialized;
} G = {0};

static guint g_overlay_reconcile_timer_id = 0;
static gint  g_overlay_reconcile_inflight = 0;

static gboolean _overlay_create_impl(const gchar *name, gint vni, const gchar *cidr,
                                     gboolean reconcile, GError **error);

static OverlayNet *
_find(const gchar *name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.nets[i].name, name) == 0)
            return &G.nets[i];
    return NULL;
}

static gchar *
_peer_port_name(const gchar *peer_ip)
{

    gchar **parts = g_strsplit(peer_ip, ".", -1);
    gchar *name = NULL;
    if (g_strv_length(parts) == 4)
        name = g_strdup_printf("vxlan-%s-%s", parts[2], parts[3]);
    else
        name = g_strdup_printf("vxlan-%s", peer_ip);
    g_strfreev(parts);
    return name;
}

static gboolean
_run_argv(const gchar * const *argv, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && std_err) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "cmd failed: %s → %s", argv[0], std_err);
    }
    g_free(std_err);
    return ok;
}

static void
_ensure_meta_dir(void)
{
    if (g_mkdir_with_parents(OVERLAY_META_DIR, 0700) != 0) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "Cannot create meta dir %s: %s",
                     OVERLAY_META_DIR, g_strerror(errno));
    }
}

static void
_save_meta(OverlayNet *net)
{
    _ensure_meta_dir();
    gchar *path = g_strdup_printf(OVERLAY_META_DIR "/overlay-%s.meta", net->name);
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", net->name);
    json_object_set_int_member(obj, "vni", net->vni);
    json_object_set_string_member(obj, "cidr", net->cidr ? net->cidr : "");

    JsonArray *peers = json_array_new();
    for (guint i = 0; i < net->peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(net->peers, i));
    json_object_set_array_member(obj, "peers", peers);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);
    gchar *data = json_to_string(node, FALSE);
    g_file_set_contents(path, data, -1, NULL);

    g_free(data);
    json_node_free(node);
    json_object_unref(obj);
    g_free(path);
}

void
pcv_overlay_init(const gchar *local_tunnel_ip)
{
    if (!local_tunnel_ip || !*local_tunnel_ip) {
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "No tunnel IP configured — overlay disabled");
        return;
    }
    g_mutex_init(&G.mu);
    G.local_ip = g_strdup(local_tunnel_ip);
    G.count = 0;
    G.initialized = TRUE;
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay manager initialized (tunnel_ip=%s)", local_tunnel_ip);
}

void
pcv_overlay_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        g_free(G.nets[i].name);
        g_free(G.nets[i].cidr);
        g_ptr_array_free(G.nets[i].peers, TRUE);
    }
    G.count = 0;
    g_free(G.local_ip);
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

void
pcv_overlay_restore(void)
{
    if (!G.initialized) {
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay disabled — skipping restore");
        return;
    }

    _ensure_meta_dir();

    GError *derr = NULL;
    GDir *dir = g_dir_open(OVERLAY_META_DIR, 0, &derr);
    if (!dir) {

        if (derr) {
            PCV_LOG_INFO(OVERLAY_LOG_DOM, "No overlay meta to restore (%s): %s",
                         OVERLAY_META_DIR, derr->message);
            g_error_free(derr);
        }
        return;
    }

    gint restored = 0;
    const gchar *fname;
    while ((fname = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_prefix(fname, "overlay-") ||
            !g_str_has_suffix(fname, ".meta"))
            continue;

        gchar *path = g_build_filename(OVERLAY_META_DIR, fname, NULL);
        JsonParser *parser = json_parser_new();
        GError *perr = NULL;
        if (!json_parser_load_from_file(parser, path, &perr)) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM, "meta parse failed (%s): %s",
                         path, perr ? perr->message : "unknown");
            if (perr) g_error_free(perr);
            g_object_unref(parser);
            g_free(path);
            continue;
        }

        JsonNode *root_n = json_parser_get_root(parser);
        if (!root_n || json_node_get_node_type(root_n) != JSON_NODE_OBJECT) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM, "meta not a JSON object: %s", path);
            g_object_unref(parser);
            g_free(path);
            continue;
        }
        JsonObject *root = json_node_get_object(root_n);

        const gchar *name = json_object_has_member(root, "name")
            ? json_object_get_string_member(root, "name") : NULL;
        if (!name || !*name) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM, "meta missing 'name': %s", path);
            g_object_unref(parser);
            g_free(path);
            continue;
        }
        gint vni = json_object_has_member(root, "vni")
            ? (gint)json_object_get_int_member(root, "vni") : 100;
        const gchar *cidr = json_object_has_member(root, "cidr")
            ? json_object_get_string_member(root, "cidr") : NULL;

        GError *cerr = NULL;
        if (!_overlay_create_impl(name, vni,
                                (cidr && *cidr) ? cidr : NULL, TRUE, &cerr)) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM, "overlay '%s' restore failed: %s",
                         name, cerr ? cerr->message : "unknown");
            if (cerr) g_error_free(cerr);
            g_object_unref(parser);
            g_free(path);
            continue;
        }

        if (json_object_has_member(root, "peers")) {
            JsonArray *peers = json_object_get_array_member(root, "peers");
            guint n = peers ? json_array_get_length(peers) : 0;
            for (guint i = 0; i < n; i++) {
                const gchar *pip = json_array_get_string_element(peers, i);
                if (pip && *pip)
                    pcv_overlay_add_peer(name, pip, NULL);
            }
        }

        restored++;
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "Restored overlay '%s' (VNI=%d, CIDR=%s)",
                     name, vni, (cidr && *cidr) ? cidr : "-");
        g_object_unref(parser);
        g_free(path);
    }
    g_dir_close(dir);

    if (restored > 0)
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "Restored %d overlay network(s) from %s",
                     restored, OVERLAY_META_DIR);
}

void
pcv_overlay_reconcile(void)
{
    pcv_overlay_restore();
}

static void
_overlay_reconcile_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_overlay_reconcile();
    g_atomic_int_set(&g_overlay_reconcile_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

static gboolean
_overlay_reconcile_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_overlay_reconcile_inflight, 0, 1))
        return G_SOURCE_CONTINUE;
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _overlay_reconcile_worker);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}

void
pcv_overlay_reconcile_timer_init(void)
{
    gint interval = pcv_config_get_int("overlay", "reconcile_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "overlay reconcile 타이머 비활성 (reconcile_interval_sec=%d)", interval);
        return;
    }
    g_overlay_reconcile_timer_id = g_timeout_add_seconds((guint)interval, _overlay_reconcile_tick, NULL);
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "overlay reconcile 타이머 등록 (%d초 주기)", interval);
}

void
pcv_overlay_reconcile_timer_shutdown(void)
{
    if (g_overlay_reconcile_timer_id) {
        g_source_remove(g_overlay_reconcile_timer_id);
        g_overlay_reconcile_timer_id = 0;
    }
}

gboolean
pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error)
{

    return _overlay_create_impl(name, vni, cidr, FALSE, error);
}

static gboolean
_overlay_create_impl(const gchar *name, gint vni, const gchar *cidr,
                     gboolean reconcile, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Overlay not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (_find(name)) {
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    if (reconcile) {
        gchar *meta_path = g_strdup_printf(OVERLAY_META_DIR "/overlay-%s.meta", name);
        gboolean meta_exists = g_file_test(meta_path, G_FILE_TEST_EXISTS);
        g_free(meta_path);
        if (!meta_exists) {
            g_mutex_unlock(&G.mu);
            PCV_LOG_INFO(OVERLAY_LOG_DOM,
                "reconcile: overlay '%s' meta 부재 (동시 teardown) → 재생성 skip", name);
            return TRUE;
        }
    }
    if (G.count >= OVERLAY_MAX) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Max overlays reached");
        return FALSE;
    }

    { const gchar *a[] = {"ovs-vsctl","--may-exist","add-br",name,NULL};
      if (!_run_argv(a, error)) { g_mutex_unlock(&G.mu); return FALSE; } }

    { const gchar *a[] = {"ip","link","set",name,"up",NULL};
      _run_argv(a, NULL); }

    if (cidr && *cidr) {
        const gchar *a[] = {"ip","addr","add",cidr,"dev",name,NULL};
        _run_argv(a, NULL);
    }

    OverlayNet *net = &G.nets[G.count++];
    net->name   = g_strdup(name);
    net->cidr   = g_strdup(cidr ? cidr : "");
    net->vni    = vni > 0 ? vni : 100;
    net->peers  = g_ptr_array_new_with_free_func(g_free);
    net->active = TRUE;

    _save_meta(net);
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' created (VNI=%d, CIDR=%s)", name, net->vni, cidr ? cidr : "-");
    return TRUE;
}

gboolean
pcv_overlay_delete(const gchar *name, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    { const gchar *a[] = {"ovs-vsctl","--if-exists","del-br",name,NULL};
      _run_argv(a, error); }

    gchar *meta = g_strdup_printf(OVERLAY_META_DIR "/overlay-%s.meta", name);
    remove(meta);
    g_free(meta);

    g_free(net->name);
    g_free(net->cidr);
    g_ptr_array_free(net->peers, TRUE);
    gint idx = (gint)(net - G.nets);
    if (idx < G.count - 1)
        G.nets[idx] = G.nets[G.count - 1];
    G.count--;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' deleted", name);
    return TRUE;
}

JsonArray *
pcv_overlay_list(void)
{
    JsonArray *arr = json_array_new();
    if (!G.initialized) return arr;

    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        OverlayNet *net = &G.nets[i];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", net->name);
        json_object_set_int_member(obj, "vni", net->vni);
        json_object_set_string_member(obj, "cidr", net->cidr);
        json_object_set_int_member(obj, "peer_count", net->peers->len);
        json_object_set_boolean_member(obj, "active", net->active);
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

JsonObject *
pcv_overlay_info(const gchar *name)
{
    JsonObject *obj = json_object_new();
    if (!G.initialized) {
        json_object_set_string_member(obj, "error", "overlay disabled");
        return obj;
    }

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        json_object_set_string_member(obj, "error", "not found");
        return obj;
    }

    json_object_set_string_member(obj, "name", net->name);
    json_object_set_int_member(obj, "vni", net->vni);
    json_object_set_string_member(obj, "cidr", net->cidr);
    json_object_set_string_member(obj, "local_tunnel_ip", G.local_ip);

    JsonArray *peers = json_array_new();
    for (guint i = 0; i < net->peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(net->peers, i));
    json_object_set_array_member(obj, "peers", peers);

    g_mutex_unlock(&G.mu);
    return obj;
}

gboolean
pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error)
{
    if (!G.initialized || !peer_tunnel_ip || !*peer_tunnel_ip) return FALSE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Overlay '%s' not found", name);
        return FALSE;
    }

    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            g_mutex_unlock(&G.mu);
            return TRUE;
        }
    }

    gchar *port_name = _peer_port_name(peer_tunnel_ip);
    gchar *key_opt = g_strdup_printf("options:key=%d", net->vni);
    gchar *rip_opt = g_strdup_printf("options:remote_ip=%s", peer_tunnel_ip);
    const gchar *a[] = {"ovs-vsctl","--may-exist","add-port",net->name,port_name,
                         "--","set","interface",port_name,"type=vxlan",key_opt,rip_opt,NULL};
    gboolean ok = _run_argv(a, error);
    g_free(key_opt);
    g_free(rip_opt);

    if (ok) {
        g_ptr_array_add(net->peers, g_strdup(peer_tunnel_ip));
        _save_meta(net);
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "VXLAN peer %s added to '%s' (port=%s, VNI=%d)",
                     peer_tunnel_ip, net->name, port_name, net->vni);
    }
    g_free(port_name);
    g_mutex_unlock(&G.mu);
    return ok;
}

gboolean
pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) { g_mutex_unlock(&G.mu); return TRUE; }

    gchar *port_name = _peer_port_name(peer_tunnel_ip);
    { const gchar *a[] = {"ovs-vsctl","--if-exists","del-port",net->name,port_name,NULL};
      _run_argv(a, error); }
    g_free(port_name);

    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            g_ptr_array_remove_index(net->peers, i);
            break;
        }
    }
    _save_meta(net);
    g_mutex_unlock(&G.mu);
    return TRUE;
}
