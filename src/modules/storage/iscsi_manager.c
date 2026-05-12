





























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
            gchar *cmd_acc = g_strdup_printf(
                "tgtadm --lld iscsi --op new --mode account --user %s --password %s",
                chap_user, chap_pass);
            _run(cmd_acc, NULL, NULL);
            g_free(cmd_acc);

            gchar *cmd_bind = g_strdup_printf(
                "tgtadm --lld iscsi --op bind --mode account --tid %d --user %s",
                tid, chap_user);
            if (_run(cmd_bind, NULL, NULL))
                PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP account bound: user=%s tid=%d", chap_user, tid);
            else
                PCV_LOG_WARN(ISCSI_LOG_DOM, "CHAP bind failed for tid=%d (non-fatal)", tid);
            g_free(cmd_bind);
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
    _run(cmd, NULL, error);
    g_free(cmd);

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


    gchar *cmd_acc = g_strdup_printf(
        "tgtadm --lld iscsi --op new --mode account --user %s --password %s",
        chap_user, chap_password);
    _run(cmd_acc, NULL, NULL);
    g_free(cmd_acc);


    gchar *cmd_bind = g_strdup_printf(
        "tgtadm --lld iscsi --op bind --mode account --tid %d --user %s",
        tid, chap_user);
    gboolean ok = _run(cmd_bind, NULL, error);
    g_free(cmd_bind);

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


    gchar *cmd1 = g_strdup_printf(
        "iscsiadm -m discovery -t sendtargets -p %s 2>/dev/null", target_ip);
    _run(cmd1, NULL, NULL);
    g_free(cmd1);


    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_pass = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (chap_user && chap_pass && *chap_user && *chap_pass) {
            gchar *auth_method = g_strdup_printf(
                "iscsiadm -m node -T %s -p %s --op=update -n node.session.auth.authmethod -v CHAP",
                iqn, target_ip);
            _run(auth_method, NULL, NULL);
            g_free(auth_method);

            gchar *auth_user = g_strdup_printf(
                "iscsiadm -m node -T %s -p %s --op=update -n node.session.auth.username -v %s",
                iqn, target_ip, chap_user);
            _run(auth_user, NULL, NULL);
            g_free(auth_user);

            gchar *auth_pass = g_strdup_printf(
                "iscsiadm -m node -T %s -p %s --op=update -n node.session.auth.password -v %s",
                iqn, target_ip, chap_pass);
            _run(auth_pass, NULL, NULL);
            g_free(auth_pass);

            PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP auth configured for %s@%s", chap_user, target_ip);
        }
        g_free(chap_pass);
    }


    gchar *cmd2 = g_strdup_printf(
        "iscsiadm -m node --targetname %s --portal %s --login 2>&1", iqn, target_ip);
    if (!_run(cmd2, NULL, error)) {
        g_free(cmd2); g_free(iqn);
        return FALSE;
    }
    g_free(cmd2);


    if (device_path) {
        gchar *cmd3 = g_strdup_printf(
            "ls /dev/disk/by-path/*%s*lun-1 2>/dev/null | head -1", target_ip);
        gchar *dev = NULL;
        _run(cmd3, &dev, NULL);
        g_free(cmd3);
        if (dev && *dev) {
            *device_path = g_strstrip(g_strdup(dev));
        } else {
            *device_path = NULL;
        }
        g_free(dev);
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
    gchar *cmd = g_strdup_printf(
        "iscsiadm -m node --targetname %s --portal %s --logout 2>&1", iqn, target_ip);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    g_free(iqn);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator disconnected: %s@%s", vm_name, target_ip);
    return ok;
}
