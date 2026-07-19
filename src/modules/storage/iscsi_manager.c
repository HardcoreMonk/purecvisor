
#include "iscsi_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <string.h>

#define ISCSI_LOG_DOM   "iscsi_mgr"

#define ISCSI_IQN_PFX   "iqn.2026-03.purecvisor"

#define ISCSI_MAX_TID   64

typedef struct {
    gchar *vm_name;
    gint   tid;
} IscsiTarget;

static struct {
    IscsiTarget targets[ISCSI_MAX_TID];
    gint        count;
    gint        next_tid;
    GMutex      mu;
    gboolean    initialized;
} G = {0};

static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(ISCSI_LOG_DOM, "cmd failed: %s → %s", cmd, std_err);
    g_free(std_err);
    g_strfreev(parsed);
    return ok;
}

static gboolean
_run_argv(const gchar *const *argv, gchar **out, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(ISCSI_LOG_DOM, "cmd failed: %s → %s",
                     (argv && argv[0]) ? argv[0] : "?", std_err);
    g_free(std_err);
    return ok;
}

static gchar *
_find_iscsi_device(const gchar *target_ip)
{
    const gchar *dir = "/dev/disk/by-path";
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    gchar *found = NULL;
    const gchar *name;
    while ((name = g_dir_read_name(d))) {
        if (g_strstr_len(name, -1, target_ip) && g_str_has_suffix(name, "lun-1")) {
            found = g_build_filename(dir, name, NULL);
            break;
        }
    }
    g_dir_close(d);
    return found;
}

static IscsiTarget *
_find_target(const gchar *vm_name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.targets[i].vm_name, vm_name) == 0)
            return &G.targets[i];
    return NULL;
}

void
pcv_iscsi_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;
    G.next_tid = 1;
    G.initialized = TRUE;
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI manager initialized");
}

void
pcv_iscsi_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++)
        g_free(G.targets[i].vm_name);
    G.count = 0;
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

gboolean
pcv_iscsi_target_create(const gchar *vm_name, const gchar *zvol_path, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "iSCSI not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (_find_target(vm_name)) {
        g_mutex_unlock(&G.mu);
        return TRUE;
    }
    if (G.count >= ISCSI_MAX_TID) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Max iSCSI targets reached");
        return FALSE;
    }

    gint tid = G.next_tid++;
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

    gchar *cmd1 = g_strdup_printf(
        "tgtadm --lld iscsi --op new --mode target --tid %d --targetname %s",
        tid, iqn);
    if (!_run(cmd1, NULL, error)) {
        g_free(cmd1); g_free(iqn);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    g_free(cmd1);

    gchar *cmd2 = g_strdup_printf(
        "tgtadm --lld iscsi --op new --mode logicalunit --tid %d --lun 1 --backing-store %s",
        tid, zvol_path);
    if (!_run(cmd2, NULL, error)) {
        g_free(cmd2); g_free(iqn);

        gchar *del = g_strdup_printf("tgtadm --lld iscsi --op delete --mode target --tid %d --force", tid);
        _run(del, NULL, NULL);
        g_free(del);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    g_free(cmd2);

    gchar *cmd3 = g_strdup_printf(
        "tgtadm --lld iscsi --op bind --mode target --tid %d --initiator-address ALL",
        tid);
    _run(cmd3, NULL, NULL);
    g_free(cmd3);

    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_pass = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (chap_user && chap_pass && *chap_user && *chap_pass) {

            const gchar *acc_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "new", "--mode", "account",
                                       "--user", chap_user, "--password", chap_pass, NULL};
            _run_argv(acc_argv, NULL, NULL);

            gchar *tid_str = g_strdup_printf("%d", tid);
            const gchar *bind_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "bind", "--mode", "account",
                                        "--tid", tid_str, "--user", chap_user, NULL};
            if (_run_argv(bind_argv, NULL, NULL))
                PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP account bound: user=%s tid=%d", chap_user, tid);
            else
                PCV_LOG_WARN(ISCSI_LOG_DOM, "CHAP bind failed for tid=%d (non-fatal)", tid);
            g_free(tid_str);
        }
        g_free(chap_pass);
    }

    IscsiTarget *t = &G.targets[G.count++];
    t->vm_name = g_strdup(vm_name);
    t->tid = tid;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI target created: tid=%d iqn=%s backing=%s",
                 tid, iqn, zvol_path);
    g_free(iqn);
    return TRUE;
}

gboolean
pcv_iscsi_target_delete(const gchar *vm_name, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    IscsiTarget *t = _find_target(vm_name);
    if (!t) {
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    gchar *cmd = g_strdup_printf(
        "tgtadm --lld iscsi --op delete --mode target --tid %d --force", t->tid);
    gboolean del_ok = _run(cmd, NULL, error);
    g_free(cmd);

    if (!del_ok) {

        PCV_LOG_WARN(ISCSI_LOG_DOM,
                     "tgtadm target delete failed (tid=%d, vm=%s) — may persist in tgtd",
                     t->tid, vm_name);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    g_free(t->vm_name);

    gint idx = (gint)(t - G.targets);
    if (idx < G.count - 1)
        G.targets[idx] = G.targets[G.count - 1];
    G.count--;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI target deleted: %s", vm_name);
    return TRUE;
}

JsonArray *
pcv_iscsi_target_list(void)
{
    JsonArray *arr = json_array_new();
    if (!G.initialized) return arr;

    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", G.targets[i].vm_name);
        json_object_set_int_member(obj, "tid", G.targets[i].tid);
        gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, G.targets[i].vm_name);
        json_object_set_string_member(obj, "iqn", iqn);
        g_free(iqn);
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

gboolean
pcv_iscsi_target_set_chap(const gchar *vm_name, const gchar *chap_user,
                           const gchar *chap_password, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "iSCSI not initialized");
        return FALSE;
    }
    if (!vm_name || !chap_user || !chap_password ||
        !*chap_user || !*chap_password) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "CHAP user and password are required");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    IscsiTarget *t = _find_target(vm_name);
    if (!t) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "iSCSI target not found: %s", vm_name);
        return FALSE;
    }
    gint tid = t->tid;
    g_mutex_unlock(&G.mu);

    const gchar *acc_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "new", "--mode", "account",
                               "--user", chap_user, "--password", chap_password, NULL};
    _run_argv(acc_argv, NULL, NULL);

    gchar *tid_str = g_strdup_printf("%d", tid);
    const gchar *bind_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "bind", "--mode", "account",
                                "--tid", tid_str, "--user", chap_user, NULL};
    gboolean ok = _run_argv(bind_argv, NULL, error);
    g_free(tid_str);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP set for %s: user=%s tid=%d",
                     vm_name, chap_user, tid);
    return ok;
}

gboolean
pcv_iscsi_initiator_connect(const gchar *target_ip, const gchar *vm_name,
                             gchar **device_path, GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

    const gchar *disc[] = { "iscsiadm", "-m", "discovery", "-t", "sendtargets",
                            "-p", target_ip, NULL };
    _run_argv(disc, NULL, NULL);

    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_pass = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (chap_user && chap_pass && *chap_user && *chap_pass) {
            const gchar *a_method[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.authmethod", "-v", "CHAP", NULL };
            _run_argv(a_method, NULL, NULL);

            const gchar *a_user[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.username", "-v", chap_user, NULL };
            _run_argv(a_user, NULL, NULL);

            const gchar *a_pass[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.password", "-v", chap_pass, NULL };
            _run_argv(a_pass, NULL, NULL);

            PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP auth configured for %s@%s", chap_user, target_ip);
        }
        g_free(chap_pass);
    }

    const gchar *login[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                             "--portal", target_ip, "--login", NULL };
    if (!_run_argv(login, NULL, error)) {
        g_free(iqn);
        return FALSE;
    }

    if (device_path) {
        *device_path = _find_iscsi_device(target_ip);
    }

    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator connected: %s@%s", iqn, target_ip);
    g_free(iqn);
    return TRUE;
}

gboolean
pcv_iscsi_initiator_disconnect(const gchar *target_ip, const gchar *vm_name,
                                GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

    const gchar *logout[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                              "--portal", target_ip, "--logout", NULL };
    gboolean ok = _run_argv(logout, NULL, error);
    g_free(iqn);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator disconnected: %s@%s", vm_name, target_ip);
    return ok;
}
