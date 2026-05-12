#include "ovn_manager.h"

#include "../../utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

#define OVN_LOG_DOM "ovn_single"
















static gboolean
pcv_ovn_single_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(OVN_LOG_DOM, "cmd failed: %s → %s", cmd, std_err);
    g_free(std_err);
    return ok;
}

static gboolean
pcv_ovn_single_get_encap_ip(gchar **ip_out, GError **error)
{
    const gchar *configured = pcv_config_get_string("ovn", "encap_ip", "");
    if (configured && *configured) {
        *ip_out = g_strdup(configured);
        return TRUE;
    }

    gchar *out = NULL;
    gboolean ok = pcv_ovn_single_run_shell(
        "ip route get 1.1.1.1 | sed -n 's/.* src \\([^ ]*\\).*/\\1/p' | head -n1",
        &out, error);
    if (!ok)
        return FALSE;

    g_strstrip(out);
    if (!out || !*out) {
        g_free(out);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to derive OVN encap IP from host routing table");
        return FALSE;
    }

    *ip_out = out;
    return TRUE;
}

static gboolean
pcv_ovn_single_get_hostname(gchar **host_out, GError **error)
{
    gchar *out = NULL;
    if (!pcv_ovn_single_run_shell("hostname", &out, error))
        return FALSE;

    g_strstrip(out);
    if (!out || !*out) {
        g_free(out);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to resolve host name");
        return FALSE;
    }

    *host_out = out;
    return TRUE;
}

static gboolean
pcv_ovn_single_get_desired_system_id(gchar **id_out, GError **error)
{
    gchar *host = NULL;
    if (!pcv_ovn_single_get_hostname(&host, error))
        return FALSE;

    gchar *cmd = g_strdup_printf(
        "ovn-sbctl --format=csv --data=bare --no-heading --columns=name,hostname list Chassis "
        "| awk -F, '$2 == \"%s\" {print $1; exit}'",
        host);
    gchar *out = NULL;
    if (!pcv_ovn_single_run_shell(cmd, &out, error)) {
        g_free(cmd);
        g_free(host);
        return FALSE;
    }
    g_free(cmd);

    g_strstrip(out);
    if (out && *out) {
        *id_out = out;
        g_free(host);
        return TRUE;
    }
    g_free(out);

    gchar *current = NULL;
    if (!pcv_ovn_single_run_shell(
            "ovs-vsctl --if-exists get Open_vSwitch . external_ids:system-id "
            "| tr -d '\"[] '",
            &current, error)) {
        g_free(host);
        return FALSE;
    }

    g_strstrip(current);
    if (current && *current) {
        *id_out = current;
        g_free(host);
        return TRUE;
    }
    g_free(current);

    *id_out = host;
    return TRUE;
}

static gboolean
pcv_ovn_single_get_controller_ctl(gchar **ctl_out, GError **error)
{
    gchar *pid = NULL;
    gsize len = 0;

    if (!g_file_get_contents("/var/run/ovn/ovn-controller.pid", &pid, &len, error))
        return FALSE;

    g_strstrip(pid);
    if (!pid || !*pid) {
        g_free(pid);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ovn-controller.pid is empty");
        return FALSE;
    }

    *ctl_out = g_strdup_printf("/var/run/ovn/ovn-controller.%s.ctl", pid);
    g_free(pid);
    return TRUE;
}

gboolean
pcv_ovn_single_prepare_local(GError **error)
{
    if (!pcv_ovn_is_available()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "OVN is unavailable");
        return FALSE;
    }

    gchar *encap_ip = NULL;
    if (!pcv_ovn_single_get_encap_ip(&encap_ip, error))
        return FALSE;

    gchar *system_id = NULL;
    if (!pcv_ovn_single_get_desired_system_id(&system_id, error)) {
        g_free(encap_ip);
        return FALSE;
    }

    const gchar *encap_type = pcv_config_get_string("ovn", "encap_type", "geneve");
    const gchar *setup_cmds[] = {
        "ovs-vsctl set Open_vSwitch . external_ids:ovn-remote=unix:/var/run/ovn/ovnsb_db.sock",
        NULL,
        NULL,
        NULL,
        "systemctl restart ovn-controller",
        NULL
    };

    gchar *cmd_system_id = g_strdup_printf(
        "ovs-vsctl set Open_vSwitch . external_ids:system-id=%s", system_id);
    gchar *cmd_encap_type = g_strdup_printf(
        "ovs-vsctl set Open_vSwitch . external_ids:ovn-encap-type=%s", encap_type);
    gchar *cmd_encap_ip = g_strdup_printf(
        "ovs-vsctl set Open_vSwitch . external_ids:ovn-encap-ip=%s", encap_ip);
    ((gchar **)setup_cmds)[1] = cmd_system_id;
    ((gchar **)setup_cmds)[2] = cmd_encap_type;
    ((gchar **)setup_cmds)[3] = cmd_encap_ip;

    gboolean ok = TRUE;
    for (guint i = 0; setup_cmds[i]; i++) {
        if (!pcv_ovn_single_run_shell(setup_cmds[i], NULL, error)) {
            ok = FALSE;
            break;
        }
    }

    gchar *ctl_path = NULL;
    if (ok && pcv_ovn_single_get_controller_ctl(&ctl_path, error)) {
        gchar *cmd_vlog = g_strdup_printf(
            "ovn-appctl -t %s vlog/set file:err", ctl_path);
        if (!pcv_ovn_single_run_shell(cmd_vlog, NULL, error)) {
            PCV_LOG_WARN(OVN_LOG_DOM,
                         "ovn-controller file log level change skipped: %s",
                         (*error && (*error)->message) ? (*error)->message : "unknown error");
            g_clear_error(error);
        }
        g_free(cmd_vlog);
    } else if (ok) {
        PCV_LOG_WARN(OVN_LOG_DOM,
                     "ovn-controller ctl socket path unavailable; file log containment skipped");
        g_clear_error(error);
    }

    if (ok) {
        PCV_LOG_INFO(OVN_LOG_DOM,
                     "Single Edge OVN local controller prepared (system-id=%s encap=%s ip=%s, ovn-controller file log=ERR)",
                     system_id, encap_type, encap_ip);
    }

    g_free(cmd_system_id);
    g_free(cmd_encap_type);
    g_free(cmd_encap_ip);
    g_free(ctl_path);
    g_free(system_id);
    g_free(encap_ip);
    return ok;
}
