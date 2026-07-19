
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/network/network_firewall_host.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_validate.h"
#include "../../utils/pcv_log.h"

#define HOST_FW_LOG_DOM "network_fw_host"

static const gchar *_state_name(PcvHostFwState st) {
    switch (st) {
    case PCV_HOST_FW_UFW:           return "ufw";
    case PCV_HOST_FW_IPTABLES_DROP: return "iptables_drop";
    case PCV_HOST_FW_FIREWALLD:     return "firewalld";
    case PCV_HOST_FW_OPEN:          return "open";
    default:                        return "unknown";
    }
}

static gboolean _ufw_enabled(void) {
    gchar *content = NULL;
    if (!g_file_get_contents("/etc/ufw/ufw.conf", &content, NULL, NULL))
        return FALSE;

    gboolean enabled = FALSE;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *s = g_strstrip(g_strdup(*l));
        if (s[0] == '#' || s[0] == '\0') { g_free(s); continue; }
        if (g_str_has_prefix(s, "ENABLED")) {
            const gchar *eq = strchr(s, '=');
            if (eq) {
                gchar *val = g_strstrip(g_strdup(eq + 1));
                if (g_ascii_strcasecmp(val, "yes") == 0) enabled = TRUE;
                g_free(val);
            }
        }
        g_free(s);
    }
    g_strfreev(lines);
    g_free(content);
    return enabled;
}

static gboolean _iptables_forward_drop(void) {
    const gchar *argv[] = {"iptables", "-S", "FORWARD", NULL};
    gchar *out = NULL;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) {
        g_free(out);
        return FALSE;
    }
    gboolean drop = FALSE;
    gchar **lines = g_strsplit(out, "\n", -1);
    if (lines[0]) {
        gchar *first = g_strstrip(g_strdup(lines[0]));
        drop = (g_strcmp0(first, "-P FORWARD DROP") == 0);
        g_free(first);
    }
    g_strfreev(lines);
    g_free(out);
    return drop;
}

static gboolean _firewalld_active(void) {
    gchar *path = g_find_program_in_path("firewall-cmd");
    if (!path) return FALSE;
    g_free(path);
    const gchar *argv[] = {"firewall-cmd", "--state", NULL};
    gboolean running = pcv_spawn_sync(argv, NULL, NULL, NULL);
    return running;
}

PcvHostFwState pcv_host_fw_detect(void) {

    gchar *ufw = g_find_program_in_path("ufw");
    if (ufw) {
        gboolean en = _ufw_enabled();
        g_free(ufw);
        if (en) return PCV_HOST_FW_UFW;
    }

    if (_iptables_forward_drop())
        return PCV_HOST_FW_IPTABLES_DROP;

    if (_firewalld_active())
        return PCV_HOST_FW_FIREWALLD;

    return PCV_HOST_FW_OPEN;
}

GPtrArray *pcv_host_fw_plan(PcvHostFwState st, const gchar *bridge, gboolean remove) {
    GPtrArray *cmds = g_ptr_array_new_with_free_func(g_free);
    if (!bridge) return cmds;

    switch (st) {
    case PCV_HOST_FW_UFW:

        if (!remove) {
            g_ptr_array_add(cmds, g_strdup_printf("ufw route allow in on %s",  bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw route allow out on %s", bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw allow in on %s",        bridge));
        } else {
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete route allow in on %s",  bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete route allow out on %s", bridge));
            g_ptr_array_add(cmds, g_strdup_printf("ufw --force delete allow in on %s",        bridge));
        }
        break;

    case PCV_HOST_FW_IPTABLES_DROP: {

        const gchar *op = remove ? "-D" : "-I";
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s FORWARD -i %s -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p udp --dport 67 -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p udp --dport 53 -j ACCEPT", op, bridge));
        g_ptr_array_add(cmds, g_strdup_printf(
            "iptables %s INPUT -i %s -p tcp --dport 53 -j ACCEPT", op, bridge));
        break;
    }

    case PCV_HOST_FW_OPEN:
    case PCV_HOST_FW_FIREWALLD:
    default:

        break;
    }
    return cmds;
}

static gboolean _iptables_rule_exists(gchar **argv) {
    gchar *saved = argv[1];
    argv[1] = (gchar *) "-C";
    gboolean exists = pcv_spawn_sync((const gchar * const *) argv, NULL, NULL, NULL);
    argv[1] = saved;
    return exists;
}

static gboolean _host_fw_apply(const gchar *bridge, gboolean remove, GError **error) {
    const gchar *op_name = remove ? "network.host_fw_remove"
                                  : "network.host_fw_integrate";

    if (!pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid bridge name for host firewall: %s",
                    bridge ? bridge : "(null)");
        return FALSE;
    }

    PcvHostFwState st = pcv_host_fw_detect();

    if (st == PCV_HOST_FW_FIREWALLD) {
        PCV_LOG_WARN(HOST_FW_LOG_DOM,
                     "firewalld 활성 호스트 — 자동 개입 비범위. 브릿지 '%s' 를 "
                     "firewalld zone 에 수동 등록 필요 (예: firewall-cmd "
                     "--zone=trusted --add-interface=%s --permanent && "
                     "firewall-cmd --reload)", bridge, bridge);
        PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge,
                      "state=firewalld result=skipped (manual firewalld zone required)");
        return TRUE;
    }

    if (st == PCV_HOST_FW_OPEN) {
        PCV_LOG_DEBUG(HOST_FW_LOG_DOM,
                      "host firewall open — no coexistence rules needed (%s %s)",
                      op_name, bridge);
        return TRUE;
    }

    GPtrArray *cmds = pcv_host_fw_plan(st, bridge, remove);
    guint total = cmds->len, applied = 0, skipped = 0, failed = 0;
    gboolean all_ok = TRUE;

    for (guint i = 0; i < total; i++) {
        gchar **argv = g_strsplit(g_ptr_array_index(cmds, i), " ", -1);

        if (g_strcmp0(argv[0], "iptables") == 0) {
            gboolean exists = _iptables_rule_exists(argv);
            if (!remove && exists) {
                skipped++; g_strfreev(argv); continue;
            }
            if (remove && !exists) {
                skipped++; g_strfreev(argv); continue;
            }
        }

        GError *cmd_err = NULL;
        if (!pcv_spawn_sync((const gchar * const *) argv, NULL, NULL, &cmd_err)) {

            PCV_LOG_WARN(HOST_FW_LOG_DOM,
                         "host firewall 명령 실패 (계속): '%s' — %s",
                         (const gchar *) g_ptr_array_index(cmds, i),
                         cmd_err ? cmd_err->message : "unknown");
            g_clear_error(&cmd_err);
            failed++;
            all_ok = FALSE;
        } else {
            applied++;
        }
        g_strfreev(argv);
    }
    g_ptr_array_unref(cmds);

    PCV_LOG_AUDIT(HOST_FW_LOG_DOM, op_name, bridge,
                  "state=%s total=%u applied=%u skipped=%u failed=%u result=%s",
                  _state_name(st), total, applied, skipped, failed,
                  all_ok ? "ok" : "partial");

    if (!all_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "host firewall %s partial failure (%u of %u commands failed)",
                    op_name, failed, total);
        return FALSE;
    }
    return TRUE;
}

gboolean pcv_host_fw_integrate(const gchar *bridge, GError **error) {
    return _host_fw_apply(bridge, FALSE, error);
}

gboolean pcv_host_fw_remove(const gchar *bridge, GError **error) {
    return _host_fw_apply(bridge, TRUE, error);
}
